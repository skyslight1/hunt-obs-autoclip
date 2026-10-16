/* hunt-obs-autoclip — OBS plugin entry.
 * Layers here: environment detection + shim installer menu + pipe server
 * thread + planner tick + obs_frontend replay buffer save.
 * Protocol/state machine live in protocol.c.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdbool.h>

#include "protocol.h"
#pragma comment(lib, "shell32.lib")

/* ---- OBS bindings (resolved at runtime; no import lib needed) ---- */

typedef void (*obs_log_t)(int level, const char *format, ...);
typedef void (*frontend_save_t)(void);
typedef bool (*frontend_active_t)(void);
typedef void (*add_menu_t)(const char *name, void (*cb)(void *), void *data);
typedef const char *(*module_name_t)(void);

static obs_log_t obs_log;
static frontend_save_t fe_save;
static frontend_active_t fe_active;
static add_menu_t fe_add_menu;

void obs_plugin_log(int level, const char *fmt, ...)
{
	char buf[1024];
	va_list a;
	va_start(a, fmt);
	vsnprintf(buf, sizeof(buf), fmt, a);
	va_end(a);
	if (obs_log)
		obs_log(level, "[hunt-obs-autoclip] %s", buf);
	else
		OutputDebugStringA(buf);
}
#define LOGI(...) obs_plugin_log(300, __VA_ARGS__)
#define LOGW(...) obs_plugin_log(200, __VA_ARGS__)
#define LOGE(...) obs_plugin_log(100, __VA_ARGS__)

static bool bind_obs(void)
{
	HMODULE fe = GetModuleHandleA("obs-frontend-api.dll");
	HMODULE core = GetModuleHandleA("obs.dll");
	if (!fe || !core)
		return false;
	obs_log = (obs_log_t)(void *)GetProcAddress(core, "obs_log");
	fe_save = (frontend_save_t)(void *)GetProcAddress(fe, "obs_frontend_replay_buffer_save");
	fe_active = (frontend_active_t)(void *)GetProcAddress(fe, "obs_frontend_replay_buffer_active");
	fe_add_menu = (add_menu_t)(void *)GetProcAddress(fe, "obs_frontend_add_tools_menu_item");
	return fe_save && fe_active;
}

/* ---- module decl macro replacements (we don't link libobs headers) ---- */
static bool obs_module_loaded;
__declspec(dllexport) bool obs_module_load(void);
__declspec(dllexport) void obs_module_unload(void);
__declspec(dllexport) void obs_module_set_pointer(void *module);
__declspec(dllexport) uint64_t obs_module_ver(void);

/* ---- state ---- */

static HANDLE g_pipe_thread, g_tick_thread, g_stop_event;
static char g_plugin_dir[MAX_PATH];

/* ---- environment detection ---- */

static bool nvidia_service_running(void)
{
	SC_HANDLE scm = OpenSCManager(NULL, NULL, SC_MANAGER_CONNECT);
	if (!scm) return false;
	bool found = false;
	static const wchar_t *svc_names[] = {L"NVDisplay.ContainerLocalSystem",
					     L"NvContainerLocalSystem"};
	for (int i = 0; i < 2 && !found; i++) {
		SC_HANDLE svc = OpenServiceW(scm, svc_names[i], SERVICE_QUERY_STATUS);
		if (svc) {
			SERVICE_STATUS st;
			if (QueryServiceStatus(svc, &st) &&
			    (st.dwCurrentState == SERVICE_RUNNING ||
			     st.dwCurrentState == SERVICE_START_PENDING))
				found = true;
			CloseServiceHandle(svc);
		}
	}
	CloseServiceHandle(scm);
	return found;
}

static bool shim_ready(void)
{
	char path[MAX_PATH];
	snprintf(path, sizeof(path), "%s\\MessageBus.dll", SHIM_DIR);
	DWORD a = GetFileAttributesA(path);
	if (a == INVALID_FILE_ATTRIBUTES)
		return false;
	HKEY k;
	if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
			  "SOFTWARE\\NVIDIA Corporation\\NvContainer\\MessageBus",
			  0, KEY_READ, &k) != ERROR_SUCCESS)
		return false;
	char val[MAX_PATH];
	DWORD sz = sizeof(val), type;
	LONG r = RegQueryValueExA(k, "InstallPath", NULL, &type, (BYTE *)val, &sz);
	RegCloseKey(k);
	return r == ERROR_SUCCESS && type == REG_SZ && !_stricmp(val, SHIM_DIR);
}

/* Look for user-supplied MessageBus.dll: plugin dir first, then Downloads. */
static bool find_user_dll(char *out, DWORD cap)
{
	char downloads[MAX_PATH];
	char p1[MAX_PATH], p2[MAX_PATH];
	DWORD n = GetEnvironmentVariableA("USERPROFILE", downloads, MAX_PATH);
	if (n == 0 || n >= MAX_PATH - 12)
		downloads[0] = 0;
	else
		strcat_s(downloads, MAX_PATH, "\\Downloads");
	snprintf(p1, sizeof(p1), "%s\\MessageBus.dll", g_plugin_dir);
	snprintf(p2, sizeof(p2), "%s\\MessageBus.dll", downloads);
	if (GetFileAttributesA(p1) != INVALID_FILE_ATTRIBUTES) {
		snprintf(out, cap, "%s", p1);
		return true;
	}
	if (downloads[0] && GetFileAttributesA(p2) != INVALID_FILE_ATTRIBUTES) {
		snprintf(out, cap, "%s", p2);
		return true;
	}
	return false;
}

/* write the PS1 installer next to the plugin and launch it elevated */
static void run_installer(void);
static void run_installer_menu(void *data) { (void)data; run_installer(); }

static const char *INSTALLER_PS_TEXT =
"$ErrorActionPreference = 'Stop'\r\n"
"$dll = $args[0]\r\n"
"$expect = $args[1]\r\n"
"$h = (Get-FileHash -Algorithm SHA256 $dll).Hash.ToLower()\r\n"
"if ($h -ne $expect) { throw \\\"hash mismatch: $h\\\" }\r\n"
"$sig = Get-AuthenticodeSignature $dll\r\n"
"if ($sig.Status -ne 'Valid') { throw \\\"signature: $($sig.Status)\\\" }\r\n"
"if ($sig.SignerCertificate.Subject -notmatch 'NVIDIA Corporation') { throw \\\"signer NVIDIA\\\" }\r\n"
"$dir = 'C:\\NvidiaShim'\r\n"
"New-Item -ItemType Directory -Force -Path $dir | Out-Null\r\n"
"Copy-Item $dll \\\"$dir\\MessageBus.dll\\\" -Force\r\n"
"$backup = @{}\r\n"
"foreach ($view in @('HKLM:\\SOFTWARE','HKLM:\\SOFTWARE\\WOW6432Node')) {\r\n"
"  $k = \\\"$view\\NVIDIA Corporation\\NvContainer\\MessageBus\\\"\r\n"
"  if (Test-Path $k) { foreach ($n in @InstallPath','LogPath','LogLevel')) { $v = (Get-ItemProperty $k -ErrorAction SilentlyContinue).$n; if ($null -ne $v) { $backup[\\\"$k|$n\\\"] = $v } } }\r\n"
"}\r\n"
"New-Item -ItemType Directory -Force -Path \\\"$env:ProgramData\\hunt-obs-autoclip\\\" | Out-Null\r\n"
"$backup | ConvertTo-Json | Set-Content \\\"$env:ProgramData\\hunt-obs-autoclip\\shim-backup.json\\\" -Force\r\n"
"foreach ($view in @('HKLM:\\SOFTWARE','HKLM:\\SOFTWARE\\WOW6432Node')) {\r\n"
"  $k = \\\"$view\\NVIDIA Corporation\\NvContainer\\MessageBus\\\"\r\n"
"  New-Item -Force -Path $k | Out-Null\r\n"
"  Set-ItemProperty $k -Name InstallPath -Value $dir -Type String\r\n"
"  Set-ItemProperty $k -Name LogPath -Value \\\"$env:ProgramData\\hunt-obs-autoclip\\MessageBusLogs\\\" -Type String\r\n"
"  Set-ItemProperty $k -Name LogLevel -Value 0 -Type DWord\r\n"
"  $m = \\\"$view\\NVIDIA Corporation\\NvContainer\\ModuleMap\\\"\r\n"
"  New-Item -Force -Path $m | Out-Null\r\n"
"  Set-ItemProperty $m -Name 'MessageBus.dll' -Value \\\"$dir\\MessageBus.dll\\\" -Type String\r\n"
"}\r\n"
"Write-Output 'SHIM_INSTALL_OK'\r\n";


static void run_installer(void)
{
	char ps1[MAX_PATH];
	snprintf(ps1, sizeof(ps1), "%s\\install-shim.ps1", g_plugin_dir);
	FILE *f = fopen(ps1, "wb");
	if (!f) {
		LOGE("cannot write installer script");
		return;
	}
	fputs(INSTALLER_PS_TEXT, f);
	fclose(f);

	char dll[MAX_PATH];
	if (!find_user_dll(dll, sizeof(dll))) {
		LOGW("no MessageBus.dll found next to plugin or in Downloads");
		MessageBoxW(NULL,
			L"MessageBus.dll not found.\n\n"
			L"it in this plugin folder or your Downloads folder, then retry.",
			L"hunt-obs-autoclip", MB_ICONWARNING);
		return;
	}

	char cmd[1024];
	snprintf(cmd, sizeof(cmd),
		 "-NoProfile -ExecutionPolicy Bypass -File \"%s\" \"%s\" \"%s\"",
		 ps1, dll, DLL_SHA256);

	SHELLEXECUTEINFOA sei = {0};
	sei.cbSize = sizeof(sei);
	sei.lpVerb = "runas"; /* UAC */
	sei.lpFile = "powershell.exe";
	sei.lpParameters = cmd;
	sei.nShow = SW_SHOWNORMAL;
	if (!ShellExecuteExA(&sei)) {
		LOGW("elevation declined or failed (%lu)", GetLastError());
		return;
	}
	LOGI("shim installer launched (elevated); verify with Tools menu after completion");
	MessageBoxW(NULL,
		L"Shim installer launched (UAC requested).\n\n"
		L"After it finishes:\n"
		L"1. Restart OBS (the plugin needs the pipe)\n"
		L"2. Start the game\n\n"
		L"Use Tools > Hunt Autoclip: Status to verify.",
		L"hunt-obs-autoclip", MB_ICONINFORMATION);
}

/* ---- status / install menu entries ---- */

static void status_menu(void *data)
{
	(void)data;
	char msg[1024];
	bool svc = nvidia_service_running();
	bool shim = shim_ready();
	bool rb = fe_active ? fe_active() : false;
	snprintf(msg, sizeof(msg),

		"hunt-obs-autoclip\n\n"

		"NVIDIA Container service: %s\n"

		"Shim (C:\\NvidiaShim + registry): %s\n"

		"OBS replay buffer: %s\n"

		"Pipe server: %s\n\n"

		"%s",

		svc ? "running (official NVIDIA App/GFE owns the pipe - disable its Highlights)" : "not running",

		shim ? "installed" : "NOT installed (required on non-NVIDIA GPUs)",

		rb ? "started" : "not started (Settings > Output > Recording: enable Replay Buffer, 35s+)",

		g_pipe_thread ? "running" : "not running",

		svc ? "Note: NVIDIA App occupies the pipe; this plugin cannot receive events while it runs."

		     : (shim ? "Environment ready. Start the game to receive kill events."

		             : "Missing NVIDIA infrastructure: use Tools > Install Shim (supply MessageBus.dll)."));

	MessageBoxA(NULL, msg, "hunt-obs-autoclip", MB_OK);

}

/* ---- pipe server thread ---- */

static DWORD WINAPI pipe_thread(LPVOID param)
{
	(void)param;
	while (WaitForSingleObject(g_stop_event, 0) != WAIT_OBJECT_0) {
		HANDLE pipe = CreateNamedPipeA(
			PIPE_NAME, PIPE_ACCESS_DUPLEX,
			PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
			PIPE_UNLIMITED_INSTANCES, 65536, 65536, 0, NULL);
		if (pipe == INVALID_HANDLE_VALUE) {
			LOGE("cannot create pipe (err %lu): NVIDIA App/GFE or another "
			     "instance owns it; plugin idle", GetLastError());
			return 0;
		}
		LOGI("listening on %s", PIPE_NAME);
		if (!ConnectNamedPipe(pipe, NULL) && GetLastError() != ERROR_PIPE_CONNECTED) {
			CloseHandle(pipe);
			continue;
		}
		/* client PID (anti-spoof: createSDKInstance must match) */
		ULONG pid = 0;
		typedef BOOL(WINAPI * fn_GNPCPID)(HANDLE, PULONG);
		static fn_GNPCPID p_GNPCPID;
		if (!p_GNPCPID)
			p_GNPCPID = (fn_GNPCPID)(void *)GetProcAddress(
				GetModuleHandleA("kernel32.dll"),
				"GetNamedPipeClientProcessId");
		if (p_GNPCPID && !p_GNPCPID(pipe, &pid))
			pid = 0;

		session_t *s = session_create(pid);
		buf_t in, replies;
		buf_init(&in);
		buf_init(&replies);
		LOGI("serving pipe client (pid=%lu)", (unsigned long)pid);
		for (;;) {
			int rc = frame_read(pipe, &in);
			if (rc <= 0)
				break;
			replies.len = 0;
			if (!session_process(s, in.p, in.len, &replies))
				break;
			if (replies.len && !frame_write(pipe, replies.p, replies.len))
				break;
		}
		LOGI("pipe client gone; waiting for next connection");
		buf_free(&in);
		buf_free(&replies);
		session_destroy(s);
		DisconnectNamedPipe(pipe);
		CloseHandle(pipe);
	}
	return 0;
}

/* ---- planner tick thread: fires coalesced clips ---- */

static DWORD WINAPI tick_thread(LPVOID param)
{
	(void)param;
	while (WaitForSingleObject(g_stop_event, 500) == WAIT_TIMEOUT) {
		if (planner_poll_should_save()) {
			LOGI(">>> SAVE TRIGGER (coalesced window closed)");
			if (fe_save)
				fe_save();
		}
	}
	return 0;
}

/* ---- module lifecycle ---- */

static void *g_obs_module_ptr;

void obs_module_set_pointer(void *module) { g_obs_module_ptr = module; }

/* OBS reads a 64-bit packed version (major<<32 | minor). */
uint64_t obs_module_ver(void) { return (1ULL << 32) | 0; }

bool obs_module_load(void)
{
	if (!bind_obs()) {
		MessageBoxW(NULL,
		L"hunt-obs-autoclip: failed to bind OBS internal interfaces. Requires OBS 28+ x64.",
		L"hunt-obs-autoclip", MB_ICONERROR);
		return false; /* unload */
	}
	obs_module_loaded = true;

	/* plugin dir = <plugin>\data\obs-plugins\hunt-obs-autoclip\.. upward:
	 * resolve from this DLL's own path: <plugin>\<dll>, data at <plugin>\data */
	char dllpath[MAX_PATH];
	GetModuleFileNameA(GetModuleHandleA("hunt-obs-autoclip.dll"), dllpath, MAX_PATH);
	char *slash = strrchr(dllpath, '\\');
	if (slash) *slash = 0;
	snprintf(g_plugin_dir, sizeof(g_plugin_dir), "%s", dllpath);

	planner_reset();

	if (fe_add_menu) {
		fe_add_menu("Hunt Autoclip: Status", status_menu, NULL);
		fe_add_menu("Hunt Autoclip: Install Shim (non-NVIDIA GPUs)", run_installer_menu, NULL);
	}

	g_stop_event = CreateEventA(NULL, TRUE, FALSE, NULL);
	g_pipe_thread = CreateThread(NULL, 0, pipe_thread, NULL, 0, NULL);
	g_tick_thread = CreateThread(NULL, 0, tick_thread, NULL, 0, NULL);

	LOGI("loaded · coalesce=15s · NVIDIA service=%s · shim=%s",
	     nvidia_service_running() ? "running" : "absent",
	     shim_ready() ? "installed" : "missing");
	if (!nvidia_service_running() && !shim_ready())
		LOGW("no NVIDIA environment detected: the game will not emit events "
		     "until the shim is installed (Tools menu) with a user-supplied "
		     "MessageBus.dll");
	return true;
}

void obs_module_unload(void)
{
	if (g_stop_event) {
		SetEvent(g_stop_event);
		if (g_pipe_thread) { WaitForSingleObject(g_pipe_thread, 3000); CloseHandle(g_pipe_thread); }
		if (g_tick_thread) { WaitForSingleObject(g_tick_thread, 3000); CloseHandle(g_tick_thread); }
		CloseHandle(g_stop_event);
	}
	LOGI("unloaded");
}
