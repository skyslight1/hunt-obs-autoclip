#!/usr/bin/env python3
"""hunt_obs_autoclip.py — Hunt: Showdown 1896 kill-to-OBS auto clipper.

Acts as a local NVIDIA Highlights IPC server: Hunt's built-in GfeSDK connects
to \\\\.\\pipe\\NvMessageBusBroadcast believing it is talking to GeForce
Experience. On SetVideoHighlightRequest (HUNTER_KILLED / PLAYER_DOWNED, ...)
this script calls obs-websocket SaveReplayBuffer, so OBS's replay buffer
(event pre-roll included) is saved as a clip per kill, with multi-kills
coalesced into one clip.

Protocol modules (protocol/*_pb2.py) and the wire fixtures (tests/*.json) are
from Mlovo/HuntOpenHighlights (MIT). Everything else: no game injection, no
memory reads, no key emulation — a passive named-pipe server.

Tested protocol compatibility path: tests/ golden frames; live game traffic
is the remaining unknown (see README).
"""
from __future__ import annotations

import argparse
import json
import logging
import os
import queue
import re
import socket
import sys
import threading
import time
import uuid
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent / "protocol"))

import google.protobuf.message
import win32file
import win32pipe

import BusMessage_pb2 as bus_pb2
import sdk_pb2 as sdk_pb2

# --------------------------------------------------------------------------
# Configuration

PIPE_NAME = r"\\.\pipe\NvMessageBusBroadcast"
MAX_FRAME_BYTES = 4 * 1024 * 1024  # matches HuntOpenHighlights BusFraming

DEFAULTS = {
    "obs_host": "127.0.0.1",
    "obs_port": 4455,
    "obs_password": "",
    "pre_roll": 20.0,        # replay buffer length to cover startDelta=-20000
    "coalesce_gap": 15.0,    # kills within this window merge into one clip
    "min_interval": 3.0,     # min seconds between SaveReplayBuffer calls
    "events": ["HUNTER_KILLED", "PLAYER_DOWNED"],
    "event_log": str(Path(__file__).parent / "events.jsonl"),
}

GFE_VERSION = "3.28.0.412"
SDK_VERSION = (1, 0)

log = logging.getLogger("hunt-obs-autoclip")


# --------------------------------------------------------------------------
# Wire framing: 4-byte big-endian length + protobuf BusMessage
# (mirrors HuntOpenHighlights BusFraming.cs)

def read_frame(handle) -> bytes | None:
    """Read one length-prefixed frame; None on clean EOF (pipe closed)."""
    try:
        _, header = win32file.ReadFile(handle, 4)
        while len(header) < 4:
            _, more = win32file.ReadFile(handle, 4 - len(header))
            if not more:
                return None
            header += more
        length = int.from_bytes(header, "big")
        if length == 0 or length > MAX_FRAME_BYTES:
            raise ValueError(f"invalid frame length {length}")
        payload = b""
        while len(payload) < length:
            _, chunk = win32file.ReadFile(handle, length - len(payload))
            if not chunk:
                return None
            payload += chunk
        return payload
    except win32file.error as exc:
        # ERROR_BROKEN_PIPE / ERROR_PIPE_DISCONNECTED = client went away
        if exc.winerror in (109, 232, 233):
            return None
        raise


def write_frame(handle, payload: bytes) -> None:
    win32file.WriteFile(handle, len(payload).to_bytes(4, "big") + payload)


# --------------------------------------------------------------------------
# Session: one protocol state machine per pipe connection
# (behavioral port of HuntOpenHighlights HighlightsSession.cs)

class Session:
    def __init__(self, client_pid: int, on_highlight, log_fn):
        self.client_pid = client_pid
        self.on_highlight = on_highlight
        self.log = log_fn
        self.game_name: str | None = None
        self.definitions: set[str] = set()
        self.groups: dict[str, int] = {}
        self.authorized = False
        self.joined = False
        self._req_seq = 0
        self._last_request_id = ""

    # -- envelope helpers ---------------------------------------------------

    @staticmethod
    def _envelope(src: bus_pb2.BusMessage, system: str, module: str) -> bus_pb2.BusMessage:
        reply = bus_pb2.BusMessage(
            uniqueid=str(uuid.uuid4()),
            reference_uniqueid=src.uniqueid,
            source_system=system,
            source_module=module,
            target_system=src.source_system or "Hunt1896",
            target_module=src.source_module or "GfeSDK",
        )
        reply.status.code = bus_pb2.BusMessage.Status.Completed
        return reply

    @staticmethod
    def _failure(status: str) -> sdk_pb2.SDKIPCResponse:
        return sdk_pb2.SDKIPCResponse(retval=-1, retstatus=status)

    def _sdk_response(self, src: bus_pb2.BusMessage, response: sdk_pb2.SDKIPCResponse) -> bus_pb2.BusMessage:
        envelope = self._envelope(src, "NvNode", "SDKAPI")
        envelope.generic.type = 2
        envelope.generic.data = sdk_pb2.SDKIPCMessage(response=response).SerializeToString()
        return envelope

    # -- request processing -------------------------------------------------

    def process(self, msg: bus_pb2.BusMessage) -> list[bus_pb2.BusMessage]:
        if msg.HasField("encryptedBusMessage") or msg.HasField("encryptionsetup"):
            raise ValueError("encrypted NVIDIA IPC is not supported")

        request = None
        if not msg.joined:
            if not msg.HasField("generic") or not msg.generic.data:
                return []
            inner = sdk_pb2.SDKIPCMessage()
            inner.ParseFromString(msg.generic.data)
            if inner.WhichOneof("message") != "request":
                return []
            request = inner.request

        kind = "JOIN" if msg.joined else f"SDK/{request.WhichOneof('request')}"
        if not msg.uniqueid or len(msg.uniqueid) > 128:
            raise ValueError(f"invalid correlation id for {kind}")
        if not msg.joined and not (msg.source_system and msg.source_module):
            raise ValueError(f"missing routing identity for {kind}")

        if msg.joined:
            self.joined = True
            reply = self._envelope(msg, "MessageBusBroadcast", "Broadcaster")
            peer = reply.bus_peer.add()
            peer.system, peer.module = "NvNode", "SDKAPI"
            ack = bus_pb2.BusMessage(
                uniqueid=str(uuid.uuid4()), source_system="NvNode",
                source_module="SDKAPI", joined=True,
            )
            self.log(f"pipe client joined: {msg.source_system}:{msg.source_module} pid={self.client_pid}")
            return [reply, ack]

        if request is None:
            return []
        self._req_seq += 1
        ordinal = self._req_seq
        self._last_request_id = msg.uniqueid
        case = request.WhichOneof("request")
        if case != "isClientAliveRequest":
            self.log(f"#{ordinal} {case}")
        response = self._handle(request, case)
        if response.retval != 0:
            self.log(f"#{ordinal} refused {case}: {response.retstatus}")
        return [self._sdk_response(msg, response)]

    def _handle(self, request, case: str) -> sdk_pb2.SDKIPCResponse:
        if request.version_major != 1:
            return self._failure("UNSUPPORTED_IPC_VERSION")
        instance_gated = case not in (
            "createSDKInstanceRequest", "getUILanguageRequest", "isClientAliveRequest",
        )
        if instance_gated and not self.authorized:
            return self._failure("SDK_INSTANCE_REQUIRED")

        response = sdk_pb2.SDKIPCResponse(retval=0, retstatus="SUCCESS")

        if case == "createSDKInstanceRequest":
            create = request.createSDKInstanceRequest
            if create.clientPID != self.client_pid:
                return self._failure("CLIENT_PID_MISMATCH")
            inst = response.createSDKInstanceResponse
            inst.installedGfeVersion = GFE_VERSION
            inst.installedGfeSDKIPCVersionMajor = SDK_VERSION[0]
            inst.installedGfeSDKIPCVersionMinor = SDK_VERSION[1]
            scopes = list(create.scopes) or [1, 2, 3, 4, 5, 6, 7]
            if len(scopes) > 32:
                return self._failure("TOO_MANY_SCOPES")
            for scope in scopes:
                perm = inst.scopePermission.add()
                perm.scope, perm.permission = scope, 1 if 1 <= scope <= 7 else 0
            self.authorized = True
            self.game_name = create.gameName or create.clientProcessName or "unknown"
            self.log(f"authorized game: {self.game_name!r}")

        elif case == "configureHighlightsRequest":
            table = request.configureHighlightsRequest.highlightTable
            if len(table) > 256:
                return self._failure("TOO_MANY_DEFINITIONS")
            self.definitions = {item.highlightId for item in table}
            self.log(f"event definitions registered: {', '.join(sorted(self.definitions)) or '(none)'}")

        elif case == "openGroupRequest":
            group = request.openGroupRequest.groupId
            if not group or len(group) > 256:
                return self._failure("INVALID_GROUP_ID")
            self.groups[group] = self.groups.get(group, 0)
            self.log(f"match started: {group!r}")

        elif case == "closeGroupRequest":
            group = request.closeGroupRequest.groupId
            if group not in self.groups:
                return self._failure("GROUP_NOT_FOUND")
            self.log(f"match ended: {group!r}")

        elif case == "setVideoHighlightRequest":
            video = request.setVideoHighlightRequest
            if not video.highlightId or len(video.highlightId) > 128:
                return self._failure("INVALID_HIGHLIGHT_ID")
            if not video.groupId or len(video.groupId) > 256:
                return self._failure("INVALID_GROUP_ID")
            if video.groupId not in self.groups:
                return self._failure("GROUP_NOT_FOUND")
            if video.highlightId not in self.definitions:
                return self._failure("HIGHLIGHT_NOT_CONFIGURED")
            if not self.on_highlight(video.highlightId, video.groupId,
                                     video.startDelta, video.endDelta):
                return self._failure("CAPTURE_NOT_READY_OR_QUEUE_FULL")
            self.groups[video.groupId] += 1

        elif case == "getUserConfigRequest":
            config = response.getUserConfigResponse
            config.gameEnabled = True
            for hid in sorted(self.definitions):
                cfg = config.highlightTable.add()
                cfg.highlightId, cfg.userEnabled = hid, True

        elif case == "getUILanguageRequest":
            response.getUILanguageResponse.localeName = "en-US"

        elif case == "isClientAliveRequest":
            response.isClientAliveResponse.alive = True

        elif case == "getNumberOfHighlightsInGroupRequest":
            group = request.getNumberOfHighlightsInGroupRequest.groupId
            if not group or len(group) > 256:
                return self._failure("INVALID_GROUP_ID")
            response.getNumberOfHighlightsInGroupResponse.numberOfHighlights = self.groups.get(group, 0)

        elif case == "destroySDKInstanceRequest":
            self.authorized = False
            self.game_name = None
            self.definitions.clear()
            self.groups.clear()

        elif case in ("requestPermissionRequest", "openGroupSummaryRequest"):
            pass  # accept silently

        elif case in ("setScreenshotHighlightRequest", "toggleOverlayRequest", "opsOptimizeRequest"):
            return self._failure("NOT_IMPLEMENTED_BY_STANDALONE_VIDEO_RECORDER")

        return response


# --------------------------------------------------------------------------
# HighlightPlanner: coalesce multi-kills, throttle saves
# (rule port of HuntOpenHighlights HighlightPlanner.cs, OBS-flavoured)

class ClipPlanner:
    def __init__(self, enabled_events: set[str], coalesce_gap: float, min_interval: float):
        self.enabled_events = enabled_events
        self.coalesce_gap = coalesce_gap
        self.min_interval = min_interval
        self.pending: dict[str, dict] = {}   # group -> plan state
        self.ready: queue.Queue[dict] = queue.Queue()
        self._last_save = 0.0
        self._lock = threading.Lock()

    def register(self, highlight_id: str, group_id: str,
                 start_delta: int, end_delta: int) -> bool:
        """Called from pipe threads. Returns False when the event is not wanted."""
        if highlight_id not in self.enabled_events:
            log.info("event %s ignored (not enabled)", highlight_id)
            return True  # valid no-op for the game
        now = time.monotonic()
        with self._lock:
            plan = self.pending.get(group_id)
            if plan and now - plan["last"] <= self.coalesce_gap:
                plan["last"] = now
                plan["events"].append(highlight_id)
                log.info("coalesced %s into open plan for %s (%d events)",
                         highlight_id, group_id, len(plan["events"]))
                return True
            if plan and now - plan["last"] > self.coalesce_gap:
                self._flush(group_id, plan)
            # throttle: if we just saved, delay this plan until the buffer refilled
            ready_at = max(now, self._last_save + self.min_interval)
            self.pending[group_id] = {
                "first": now, "last": now, "ready_at": ready_at,
                "events": [highlight_id], "start_delta": start_delta,
                "end_delta": end_delta,
            }
            log.info("new clip plan for %s: %s (start_delta=%d end_delta=%d)",
                     group_id, highlight_id, start_delta, end_delta)
        return True

    def _flush(self, group_id: str, plan: dict) -> None:
        self.ready.put({
            "group": group_id, "events": plan["events"], "at": plan["last"],
            "start_delta": plan["start_delta"], "end_delta": plan["end_delta"],
        })

    def poll_ready(self) -> list[dict]:
        """Fire plans whose coalescing window has closed. Called from the OBS thread."""
        now = time.monotonic()
        fired = []
        with self._lock:
            for group_id, plan in list(self.pending.items()):
                if now - plan["last"] >= self.coalesce_gap and now >= plan["ready_at"]:
                    self._flush(group_id, plan)
                    del self.pending[group_id]
                    self._last_save = now
                    fired.append(self.ready.get_nowait())
        return fired


# --------------------------------------------------------------------------
# OBS side: obs-websocket v5 SaveReplayBuffer

class ObsClient:
    def __init__(self, host: str, port: int, password: str):
        self.host, self.port, self.password = host, port, password
        self._client = None

    def _connect(self):
        import simpleobsws  # imported lazily so protocol tests run without it
        client = simpleobsws.WebSocketClient(
            url=f"ws://{self.host}:{self.port}",
            password=self.password or None,
            identification_method=simpleobsws.IdentificationParameters(
                rpcVersion=1, eventSubscriptions=int(1 << 9),  # Outputs: ReplayBufferSaved
            ),
        )
        loop_ok = client.connect()
        if not loop_ok:
            raise ConnectionError("obs-websocket connect failed")
        log.info("connected to obs-websocket %s:%d", self.host, self.port)
        self._client = client
        return client

    def save_replay_buffer(self) -> str | None:
        import asyncio
        client = self._client

        async def _run():
            if not client or not await client.wait_until_authenticated():
                return None
            result = await client.call("SaveReplayBuffer")
            if not result or not result.ok():
                log.error("SaveReplayBuffer failed: %s", result)
                return None
            return True

        try:
            asyncio.run(_run())
        except Exception as exc:  # OBS closed / websocket dropped
            log.warning("obs connection lost (%s); will reconnect next event", exc)
            self._client = None
            return None
        return "ok"

    def listen_saved_paths(self, out_cb):
        """Blocking: reconnect loop, prints ReplayBufferSaved payload paths."""
        import asyncio
        import simpleobsws

        async def _on_saved(data):
            path = data.get("savedReplayPath", "")
            log.info("OBS saved replay: %s", path)
            out_cb(path)

        async def _runner():
            while True:
                client = None
                try:
                    client = simpleobsws.WebSocketClient(
                        url=f"ws://{self.host}:{self.port}",
                        password=self.password or None,
                        identification_method=simpleobsws.IdentificationParameters(
                            rpcVersion=1, eventSubscriptions=int(1 << 9),
                        ),
                    )
                    await client.connect()
                    if not await client.wait_until_authenticated():
                        raise ConnectionError("auth failed")
                    log.info("event listener connected to obs-websocket")
                    client.register_event_callback(_on_saved, "ReplayBufferSaved")
                    while True:
                        await asyncio.sleep(3600)
                except Exception as exc:
                    log.warning("event listener disconnected (%s); retrying in 5s", exc)
                    await asyncio.sleep(5)
                finally:
                    if client:
                        try:
                            await client.disconnect()
                        except Exception:
                            pass

        threading.Thread(target=lambda: asyncio.run(_runner()), daemon=True).start()


# --------------------------------------------------------------------------
# Pipe server

class PipeServer:
    def __init__(self, on_highlight):
        self.on_highlight = on_highlight

    def _client_pid(self, handle) -> int:
        """Best-effort client PID via GetNamedPipeClientProcessId."""
        import ctypes
        from ctypes import wintypes
        pid = wintypes.DWORD(0)
        if ctypes.windll.kernel32.GetNamedPipeClientProcessId(handle, ctypes.byref(pid)):
            return pid.value
        return 0

    def serve_forever(self, stop: threading.Event):
        while not stop.is_set():
            try:
                handle = win32pipe.CreateNamedPipe(
                    PIPE_NAME,
                    win32pipe.PIPE_ACCESS_DUPLEX,
                    win32pipe.PIPE_TYPE_BYTE | win32pipe.PIPE_READMODE_BYTE | win32pipe.PIPE_WAIT,
                    win32pipe.PIPE_UNLIMITED_INSTANCES,
                    65536, 65536, 0,
                    None,
                )
            except win32pipe.error as exc:
                # pipe name taken: NVIDIA container or another instance owns it
                log.error("cannot create %s (winerror=%s): NVIDIA App/GFE running, "
                          "or another instance already owns the pipe. Exiting.",
                          PIPE_NAME, exc.winerror)
                return False
            log.info("listening on %s", PIPE_NAME)
            try:
                win32pipe.ConnectNamedPipe(handle, None)
            except win32pipe.error as exc:
                if exc.winerror == 535:  # ERROR_PIPE_CONNECTED: client raced us
                    pass
                else:
                    win32file.CloseHandle(handle)
                    continue
            if stop.is_set():
                win32file.CloseHandle(handle)
                break
            client_pid = self._client_pid(handle.handle if hasattr(handle, "handle") else handle)
            session = Session(client_pid, self.on_highlight, lambda m: log.info(m))
            self._serve_client(handle, session)
        return True

    def _serve_client(self, handle, session: Session):
        log.info("serving pipe client (pid=%s)", session.client_pid)
        try:
            while True:
                frame = read_frame(handle)
                if frame is None:
                    break
                try:
                    msg = bus_pb2.BusMessage()
                    msg.ParseFromString(frame)
                    replies = session.process(msg)
                except (google.protobuf.message.DecodeError, ValueError) as exc:
                    log.warning("protocol error, dropping connection: %s", exc)
                    break
                for reply in replies:
                    write_frame(handle, reply.SerializeToString())
        except win32file.error as exc:
            log.info("pipe client disconnected (winerror=%s)", exc.winerror)
        finally:
            win32file.CloseHandle(handle)
            log.info("pipe client gone; waiting for next connection")


# --------------------------------------------------------------------------
# main

def sanitize_group(name: str) -> str:
    return re.sub(r'[<>:"/\\|?*\x00-\x1f]', "_", name)[:64] or "match"


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--obs-host", default=DEFAULTS["obs_host"])
    parser.add_argument("--obs-port", type=int, default=DEFAULTS["obs_port"])
    parser.add_argument("--obs-password", default=os.environ.get("OBS_WS_PASSWORD", DEFAULTS["obs_password"]))
    parser.add_argument("--events", default=",".join(DEFAULTS["events"]),
                        help="comma-separated highlightIds to react to")
    parser.add_argument("--coalesce-gap", type=float, default=DEFAULTS["coalesce_gap"],
                        help="seconds; kills within this window merge into one save")
    parser.add_argument("--min-interval", type=float, default=DEFAULTS["min_interval"],
                        help="min seconds between two SaveReplayBuffer calls")
    parser.add_argument("--dry-run", action="store_true",
                        help="print events instead of calling obs-websocket")
    parser.add_argument("--verbose", action="store_true")
    return parser


def main(argv=None) -> int:
    args = build_arg_parser().parse_args(argv)
    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)-7s %(message)s", datefmt="%H:%M:%S",
    )

    enabled = {e.strip() for e in args.events.split(",") if e.strip()}
    planner = ClipPlanner(enabled, args.coalesce_gap, args.min_interval)
    event_log_path = Path(DEFAULTS["event_log"])

    def on_highlight(highlight_id, group_id, start_delta, end_delta) -> bool:
        # NB: called on a pipe thread; ClipPlanner.register is thread-safe.
        event_log_path.parent.mkdir(parents=True, exist_ok=True)
        with event_log_path.open("a", encoding="utf-8") as fh:
            fh.write(json.dumps({
                "ts": time.strftime("%Y-%m-%dT%H:%M:%S"),
                "event": highlight_id, "group": group_id,
                "startDelta": start_delta, "endDelta": end_delta,
            }) + "\n")
        return planner.register(highlight_id, group_id, start_delta, end_delta)

    if args.dry_run:
        log.info("dry-run: events will be logged, OBS will not be touched")
        obs = None
    else:
        obs = ObsClient(args.obs_host, args.obs_port, args.obs_password)
        obs.listen_saved_paths(lambda path: log.info("clip ready: %s", path))
        # eager first connect so the first kill isn't delayed by handshake
        obs.save_replay_buffer()

    def obs_worker():
        while True:
            time.sleep(0.5)
            for plan in planner.poll_ready():
                names = ",".join(plan["events"])
                tag = sanitize_group(plan["group"])
                log.info(">>> SAVE TRIGGER: %s (group=%s, events=%d)", names, tag, len(plan["events"]))
                if obs is not None:
                    obs.save_replay_buffer()

    threading.Thread(target=obs_worker, daemon=True).start()

    log.info("hunt-obs-autoclip running · events=%s · coalesce=%.0fs · obs=%s:%d%s",
             ",".join(sorted(enabled)), args.coalesce_gap, args.obs_host, args.obs_port,
             " (dry-run)" if args.dry_run else "")
    stop = threading.Event()
    try:
        ok = PipeServer(on_highlight).serve_forever(stop)
        return 0 if ok else 2
    except KeyboardInterrupt:
        log.info("bye")
        return 0


if __name__ == "__main__":
    sys.exit(main())
