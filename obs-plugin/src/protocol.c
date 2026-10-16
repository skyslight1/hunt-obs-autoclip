/* hunt-obs-autoclip — protocol/session/planner implementation.
 * Behavioral port of Mlovo/HuntOpenHighlights (MIT): HighlightsSession.cs,
 * BusFraming.cs, HighlightPlanner.cs. Wire golden frames: tests/*.json.
 */
#include "protocol.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define MAX_FRAME 4 * 1024 * 1024
#define GFE_VERSION "3.28.0.412"
#define COALESCE_GAP_MS 15000LL
#define MIN_INTERVAL_MS 3000LL

/* ---------------- logging (set by plugin.c) ---------------- */
void obs_plugin_log(int level, const char *fmt, ...);
#define LOGI(...) obs_plugin_log(300, __VA_ARGS__)
#define LOGW(...) obs_plugin_log(200, __VA_ARGS__)

/* ---------------- buffer ---------------- */

void buf_init(buf_t *b) { b->p = NULL; b->len = b->cap = 0; }

void buf_free(buf_t *b) { free(b->p); b->p = NULL; b->len = b->cap = 0; }

static void buf_reserve(buf_t *b, size_t extra)
{
	if (b->len + extra > b->cap) {
		size_t nc = b->cap ? b->cap * 2 : 4096;
		while (nc < b->len + extra) nc *= 2;
		b->p = realloc(b->p, nc);
		b->cap = nc;
	}
}

void buf_put(buf_t *b, const void *data, size_t n)
{
	buf_reserve(b, n);
	memcpy(b->p + b->len, data, n);
	b->len += n;
}

void buf_put_u8(buf_t *b, uint8_t v) { buf_put(b, &v, 1); }

void buf_put_varint(buf_t *b, uint64_t v)
{
	uint8_t tmp[10];
	int n = 0;
	while (v >= 0x80) { tmp[n++] = (uint8_t)(v | 0x80); v >>= 7; }
	tmp[n++] = (uint8_t)v;
	buf_put(b, tmp, (size_t)n);
}

void buf_put_tag(buf_t *b, uint32_t field, int wire)
{
	buf_put_varint(b, ((uint64_t)field << 3) | (uint32_t)wire);
}

void buf_put_str(buf_t *b, uint32_t field, const char *s, size_t len)
{
	buf_put_tag(b, field, 2);
	buf_put_varint(b, len);
	buf_put(b, s, len);
}

void buf_put_msg(buf_t *b, uint32_t field, const buf_t *sub)
{
	buf_put_tag(b, field, 2);
	buf_put_varint(b, sub->len);
	buf_put(b, sub->p, sub->len);
}

/* ---------------- reader ---------------- */

typedef struct { const uint8_t *p, *end; } rd_t;

static bool rd_varint(rd_t *r, uint64_t *out)
{
	uint64_t v = 0;
	int shift = 0;
	while (r->p < r->end) {
		uint8_t b = *r->p++;
		v |= (uint64_t)(b & 0x7F) << shift;
		if (!(b & 0x80)) { *out = v; return true; }
		shift += 7;
		if (shift > 63) return false;
	}
	return false;
}

static bool rd_tag(rd_t *r, uint32_t *field, int *wire)
{
	uint64_t t;
	if (!rd_varint(r, &t)) return false;
	*field = (uint32_t)(t >> 3);
	*wire = (int)(t & 7);
	return true;
}

static bool rd_skip(rd_t *r, int wire)
{
	uint64_t v;
	switch (wire) {
	case 0: return rd_varint(r, &v);
	case 1: if (r->end - r->p < 8) return false; r->p += 8; return true;
	case 2: {
		uint64_t l;
		if (!rd_varint(r, &l) || (uint64_t)(r->end - r->p) < l) return false;
		r->p += l; return true;
	}
	case 5: if (r->end - r->p < 4) return false; r->p += 4; return true;
	default: return false;
	}
}

/* copy a length-delimited field into a fixed buffer (nul-terminated) */
static bool rd_copy_str(rd_t *r, char *dst, size_t cap, size_t *out_len)
{
	uint64_t l;
	if (!rd_varint(r, &l) || (uint64_t)(r->end - r->p) < l || l >= cap)
		return false;
	memcpy(dst, r->p, (size_t)l);
	dst[l] = 0;
	if (out_len) *out_len = (size_t)l;
	r->p += l;
	return true;
}

/* ---------------- framing ---------------- */

int frame_read(HANDLE pipe, buf_t *out)
{
	uint8_t hdr[4];
	DWORD got = 0;
	out->len = 0;
	if (!ReadFile(pipe, hdr, 1, &got, NULL) || got == 0)
		return 0; /* clean EOF or broken pipe */
	/* first byte read; need 3 more with a deadline handled by caller-side
	 * timeouts — for simplicity we do blocking reads (client is the game,
	 * frame header always arrives complete; partial TCP-like splits are
	 * practically not observed on local named-pipe byte mode). */
	if (!ReadFile(pipe, hdr + 1, 3, &got, NULL) || got != 3)
		return -1;
	uint32_t len = ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16) |
		       ((uint32_t)hdr[2] << 8) | (uint32_t)hdr[3];
	if (len == 0 || len > MAX_FRAME)
		return -1;
	buf_reserve(out, len);
	DWORD total = 0;
	while (total < len) {
		if (!ReadFile(pipe, out->p + total, len - total, &got, NULL) || got == 0)
			return -1;
		total += got;
	}
	out->len = len;
	return 1;
}

bool frame_write(HANDLE pipe, const uint8_t *data, size_t len)
{
	uint8_t hdr[4] = {(uint8_t)(len >> 24), (uint8_t)(len >> 16),
			  (uint8_t)(len >> 8), (uint8_t)len};
	DWORD w;
	if (!WriteFile(pipe, hdr, 4, &w, NULL))
		return false;
	return WriteFile(pipe, data, (DWORD)len, &w, NULL) && w == len;
}

/* ---------------- session ---------------- */

struct session {
	uint32_t client_pid;
	bool authorized;
	char game_name[128];
	char definitions[16][64];
	int n_definitions;
	char groups[8][64];
	int group_counts[8];
	int n_groups;
	char last_request_id[160];
	char current_group[64];
};

session_t *session_create(uint32_t client_pid)
{
	session_t *s = calloc(1, sizeof(*s));
	if (s) s->client_pid = client_pid;
	return s;
}

void session_destroy(session_t *s) { free(s); }

const char *session_game_name(const session_t *s) { return s->game_name; }

static int find_group(session_t *s, const char *gid)
{
	for (int i = 0; i < s->n_groups; i++)
		if (!strcmp(s->groups[i], gid)) return i;
	return -1;
}

static int add_group(session_t *s, const char *gid)
{
	int i = find_group(s, gid);
	if (i >= 0) return i;
	if (s->n_groups >= 8) return -1;
	snprintf(s->groups[s->n_groups], sizeof(s->groups[0]), "%s", gid);
	return s->n_groups++;
}

/* ---- response builders ---- */

static void resp_error(buf_t *inner, const char *status)
{
	inner->len = 0;
	/* SDKIPCResponse{1: retval=-1, 2: retstatus} */
	buf_put_tag(inner, 1, 0);
	buf_put_varint(inner, (uint64_t)-1); /* varint of negative = 10 bytes */
	buf_put_tag(inner, 2, 2);
	buf_put_varint(inner, strlen(status));
	buf_put(inner, status, strlen(status));
}

static void resp_ok_empty(buf_t *inner)
{
	inner->len = 0;
	buf_put_tag(inner, 1, 0);
	buf_put_varint(inner, 0); /* retval = 0 */
}

static void new_uid(char out[40])
{
	static const char *hexd = "0123456789abcdef";
	unsigned seed = (unsigned)GetTickCount() ^ (unsigned)(uintptr_t)out;
	for (int i = 0; i < 32; i++) {
		seed = seed * 1103515245u + 12345u;
		out[i] = hexd[(seed >> 16) & 0xF];
	}
	out[32] = 0;
}

/* Build one reply BusMessage. ref_uid/ref_len = request correlation id. */
static void build_reply(buf_t *out, const char *ref_uid, size_t ref_len,
			const char *src_sys, const char *src_mod,
			const char *tgt_sys, const char *tgt_mod,
			int generic_type, const buf_t *inner, bool joined_flag,
			bool with_peer)
{
	out->len = 0;
	char uid[40];
	new_uid(uid);
	buf_put_str(out, 1, uid, strlen(uid));
	if (ref_len) buf_put_str(out, 10, ref_uid, ref_len);
	buf_put_str(out, 2, src_sys, strlen(src_sys));
	buf_put_str(out, 3, src_mod, strlen(src_mod));
	buf_put_str(out, 6, tgt_sys, strlen(tgt_sys));
	buf_put_str(out, 7, tgt_mod, strlen(tgt_mod));
	if (joined_flag) {
		buf_put_tag(out, 5, 0);
		buf_put_u8(out, 1);
	}
	if (with_peer) {
		buf_t peer;
		buf_init(&peer);
		buf_put_str(&peer, 1, "NvNode", 6);
		buf_put_str(&peer, 2, "SDKAPI", 6);
		buf_put_msg(out, 15, &peer);
		buf_free(&peer);
	}
	if (inner) {
		buf_t gen;
		buf_init(&gen);
		buf_put_tag(&gen, 1, 0);
		buf_put_varint(&gen, (uint64_t)generic_type);
		buf_put_msg(&gen, 2, inner);
		buf_put_msg(out, 20, &gen);
		buf_free(&gen);
	}
	{
		buf_t st;
		buf_init(&st);
		buf_put_tag(&st, 1, 0);
		buf_put_varint(&st, 2); /* Completed */
		buf_put_msg(out, 21, &st);
		buf_free(&st);
	}
}

/* clean-room envelope walker (the one actually used) */
typedef void (*envelope_cb)(void *ud, uint32_t field, int wire,
			    const uint8_t *data, size_t len, uint64_t varint);

static bool envelope_walk(const uint8_t *frame, size_t len, envelope_cb cb, void *ud)
{
	rd_t r = {frame, frame + len};
	uint32_t f;
	int w;
	while (rd_tag(&r, &f, &w)) {
		if (w == 2) {
			uint64_t l;
			if (!rd_varint(&r, &l) || (uint64_t)(r.end - r.p) < l)
				return false;
			cb(ud, f, 2, r.p, (size_t)l, 0);
			r.p += l;
		} else if (w == 0) {
			uint64_t v;
			if (!rd_varint(&r, &v)) return false;
			cb(ud, f, 0, NULL, 0, v);
		} else {
			if (!rd_skip(&r, w)) return false;
		}
	}
	return true;
}

typedef struct {
	char ref_uid[160];
	char tgt_sys[128];
	char tgt_mod[128];
	bool joined;
	bool has_generic;
	int generic_type;
	buf_t generic_data;
} envelope_t;

static void envelope_collect(void *ud, uint32_t field, int wire,
			     const uint8_t *data, size_t len, uint64_t varint)
{
	envelope_t *e = ud;
	switch (field) {
	case 1:
		if (len < sizeof(e->ref_uid)) { memcpy(e->ref_uid, data, len); e->ref_uid[len] = 0; }
		break;
	case 2:
		if (len < sizeof(e->tgt_sys)) { memcpy(e->tgt_sys, data, len); e->tgt_sys[len] = 0; }
		break;
	case 3:
		if (len < sizeof(e->tgt_mod)) { memcpy(e->tgt_mod, data, len); e->tgt_mod[len] = 0; }
		break;
	case 5:
		e->joined = varint != 0;
		break;
	case 20:
		/* generic submessage: {1: type varint, 2: data bytes} */
		e->has_generic = true;
		{
			rd_t g = {data, data + len};
			uint32_t gf;
			int gw;
			while (rd_tag(&g, &gf, &gw)) {
				if (gw == 0) {
					uint64_t v;
					if (!rd_varint(&g, &v)) return;
					if (gf == 1) e->generic_type = (int)v;
				} else if (gw == 2) {
					uint64_t l;
					if (!rd_varint(&g, &l) || (uint64_t)(g.end - g.p) < l) return;
					if (gf == 2) {
						e->generic_data.len = 0;
						buf_put(&e->generic_data, g.p, (size_t)l);
					} else {
						g.p += l;
					}
				} else if (!rd_skip(&g, gw)) return;
			}
		}
		break;
	default:
		break;
	}
}

/* SDK request oneof field numbers (from sdk.proto) */
enum {
	REQ_getUILanguage = 10,
	REQ_openGroup = 11,
	REQ_closeGroup = 12,
	REQ_setScreenshot = 13,
	REQ_setVideoHighlight = 14,
	REQ_toggleOverlay = 15,
	REQ_configureHighlights = 16,
	REQ_openGroupSummary = 17,
	REQ_createSDKInstance = 19,
	REQ_destroySDKInstance = 20,
	REQ_isClientAlive = 21,
	REQ_getNumberOfHighlightsInGroup = 22,
	REQ_getUserConfig = 23,
	REQ_requestPermission = 24,
	REQ_opsOptimize = 25,
};

/* walk SDKIPCRequest oneof; returns field number or -1 */
typedef struct {
	int request_field;
	int32_t version_major;
	char group_id[64];
	char highlight_id[64];
	int32_t start_delta, end_delta;
	uint32_t client_pid;
	char client_process_name[128];
	char game_name[128];
	int n_definitions;
	char defs[16][64];
} sdk_request_t;

static void request_collect(void *ud, uint32_t field, int wire,
			    const uint8_t *data, size_t len, uint64_t varint)
{
	sdk_request_t *q = ud;
	if (wire == 0) {
		if (field == 1) q->version_major = (int32_t)varint;
		return;
	}
	if (wire != 2) return;
	switch (field) {
	case REQ_openGroup:
	case REQ_closeGroup:
	case REQ_getNumberOfHighlightsInGroup: {
		rd_t r = {data, data + len};
		uint32_t f;
		int w;
		while (rd_tag(&r, &f, &w)) {
			if (w == 2 && f == 1) {
				rd_copy_str(&r, q->group_id, sizeof(q->group_id), NULL);
			} else if (!rd_skip(&r, w)) return;
		}
		q->request_field = (int)field;
		break;
	}
	case REQ_setVideoHighlight: {
		rd_t r = {data, data + len};
		uint32_t f;
		int w;
		while (rd_tag(&r, &f, &w)) {
			if (w == 2 && f == 2) rd_copy_str(&r, q->group_id, sizeof(q->group_id), NULL);
			else if (w == 2 && f == 3) rd_copy_str(&r, q->highlight_id, sizeof(q->highlight_id), NULL);
			else if (w == 0 && f == 4) { uint64_t v; rd_varint(&r, &v); q->start_delta = (int32_t)v; }
			else if (w == 0 && f == 5) { uint64_t v; rd_varint(&r, &v); q->end_delta = (int32_t)v; }
			else if (!rd_skip(&r, w)) return;
		}
		q->request_field = (int)field;
		break;
	}
	case REQ_createSDKInstance: {
		rd_t r = {data, data + len};
		uint32_t f;
		int w;
		while (rd_tag(&r, &f, &w)) {
			if (w == 2 && f == 1) rd_copy_str(&r, q->client_process_name, sizeof(q->client_process_name), NULL);
			else if (w == 0 && f == 2) { uint64_t v; rd_varint(&r, &v); q->client_pid = (uint32_t)v; }
			else if (w == 2 && f == 4) rd_copy_str(&r, q->game_name, sizeof(q->game_name), NULL);
			else if (!rd_skip(&r, w)) return;
		}
		q->request_field = (int)field;
		break;
	}
	case REQ_configureHighlights: {
		rd_t r = {data, data + len};
		uint32_t f;
		int w;
		while (rd_tag(&r, &f, &w)) {
			if (w == 2 && f == 3) { /* highlightTable entry */
				rd_t e = {r.p, r.p + 0};
				uint64_t l;
				/* entry length prefix */
				rd_t tmp = r;
				if (!rd_varint(&tmp, &l) || (uint64_t)(tmp.end - tmp.p) < l) return;
				e = (rd_t){tmp.p, tmp.p + l};
				r.p = tmp.p + l;
				uint32_t ef;
				int ew;
				while (rd_tag(&e, &ef, &ew)) {
					if (ew == 2 && ef == 1 && q->n_definitions < 16) {
						rd_copy_str(&e, q->defs[q->n_definitions], sizeof(q->defs[0]), NULL);
						q->n_definitions++;
					} else if (!rd_skip(&e, ew)) break;
				}
			} else if (!rd_skip(&r, w)) return;
		}
		q->request_field = (int)field;
		break;
	}
	default:
		/* zero-field requests: getUILanguage/isClientAlive/getUserConfig/
		 * destroySDKInstance/requestPermission/openGroupSummary/etc. */
		q->request_field = (int)field;
		break;
	}
}

/* ---- planner ---- */

static struct {
	bool has_open_plan;
	char group[64];
	char events[8][64];
	int n_events;
	long long last_event_ms;
	long long last_save_ms;
	bool throttle_pending;
} g_planner;

static bool event_enabled(const char *id)
{
	/* default-enabled events; mirror Python DEFAULTS */
	return !strcmp(id, "HUNTER_KILLED") || !strcmp(id, "PLAYER_DOWNED");
}

void planner_reset(void)
{
	memset(&g_planner, 0, sizeof(g_planner));
}

static long long now_ms(void) { return (long long)GetTickCount64(); }

bool planner_register(const char *highlight_id, const char *group_id)
{
	if (!event_enabled(highlight_id)) {
		LOGI("event %s ignored (not enabled)", highlight_id);
		return true; /* valid no-op for the game */
	}
	long long now = now_ms();
	if (g_planner.has_open_plan &&
	    !strcmp(g_planner.group, group_id) &&
	    now - g_planner.last_event_ms <= COALESCE_GAP_MS) {
		if (g_planner.n_events < 8)
			snprintf(g_planner.events[g_planner.n_events++], 64, "%s", highlight_id);
		g_planner.last_event_ms = now;
		LOGI("coalesced %s into open plan (%d events)", highlight_id, g_planner.n_events);
		return true;
	}
	g_planner.has_open_plan = true;
	snprintf(g_planner.group, sizeof(g_planner.group), "%s", group_id);
	g_planner.n_events = 1;
	snprintf(g_planner.events[0], 64, "%s", highlight_id);
	g_planner.last_event_ms = now;
	if (now - g_planner.last_save_ms < MIN_INTERVAL_MS)
		g_planner.throttle_pending = true;
	LOGI("new clip plan for %s: %s", group_id, highlight_id);
	return true;
}

bool planner_poll_should_save(void)
{
	long long now = now_ms();
	if (!g_planner.has_open_plan)
		return false;
	long long gap = g_planner.throttle_pending
				? (MIN_INTERVAL_MS > COALESCE_GAP_MS ? MIN_INTERVAL_MS : COALESCE_GAP_MS)
				: COALESCE_GAP_MS;
	if (now - g_planner.last_event_ms < gap)
		return false;
	g_planner.has_open_plan = false;
	g_planner.throttle_pending = false;
	g_planner.last_save_ms = now;
	return true;
}

/* ---- session processing ---- */

session_t *g_log_session; /* reserved */

bool session_process(session_t *s, const uint8_t *frame, size_t len, buf_t *out_replies)
{
	envelope_t env = {0};
	buf_init(&env.generic_data);
	strcpy(env.tgt_sys, "Hunt1896");
	strcpy(env.tgt_mod, "GfeSDK");
	if (!envelope_walk(frame, len, envelope_collect, &env)) {
		buf_free(&env.generic_data);
		return false;
	}

	if (env.has_generic && (strstr((const char *)env.generic_data.p, "\x08\x02") == env.generic_data.p)) {
		/* generic.type==2 would be a response; game->server never sends those. ignore */
	}

	/* JOIN frame */
	if (env.joined && !env.has_generic) {
		s->authorized = false;
		buf_t reply;
		buf_init(&reply);
		/* ack with bus_peer */
		build_reply(&reply, env.ref_uid, strlen(env.ref_uid),
			    "MessageBusBroadcast", "Broadcaster",
			    env.tgt_sys, env.tgt_mod, 0, NULL, false, true);
		buf_put(out_replies, reply.p, reply.len);
		buf_free(&reply);
		/* second reply: our own JOIN announcement */
		buf_init(&reply);
		build_reply(&reply, NULL, 0, "NvNode", "SDKAPI", "", "", 0, NULL, true, false);
		buf_put(out_replies, reply.p, reply.len);
		buf_free(&reply);
		LOGI("pipe client joined: %s:%s pid=%u", env.tgt_sys, env.tgt_mod, s->client_pid);
		buf_free(&env.generic_data);
		return true;
	}

	if (!env.has_generic || env.generic_data.len == 0) {
		buf_free(&env.generic_data);
		return true; /* bus-level ACK etc. — ignore */
	}

	/* parse SDKIPCMessage{1: request} */
	rd_t msg = {env.generic_data.p, env.generic_data.p + env.generic_data.len};
	uint32_t mf;
	int mw;
	int req_field = -1;
	sdk_request_t q = {0};
	bool has_request = false;
	bool encrypted = false;
	while (rd_tag(&msg, &mf, &mw)) {
		if (mw == 2 && mf == 1) {
			uint64_t l;
			if (!rd_varint(&msg, &l) || (uint64_t)(msg.end - msg.p) < l) goto bad;
			memset(&q, 0, sizeof(q));
			q.request_field = -1;
			request_collect(&q, 0, 0, NULL, 0, 0);
			{
				rd_t rq = {msg.p, msg.p + l};
				uint32_t rf;
				int rw;
				while (rd_tag(&rq, &rf, &rw)) {
					/* reuse collector for subfields: special-case by
					 * dispatching directly */
					if (rw == 0 && rf == 1) { uint64_t v; rd_varint(&rq, &v); q.version_major = (int32_t)v; }
					else if (rw == 2) {
						uint64_t sl;
						if (!rd_varint(&rq, &sl) || (uint64_t)(rq.end - rq.p) < sl) goto bad;
						request_collect(&q, rf, 2, rq.p, (size_t)sl, 0);
						rq.p += sl;
						has_request = true;
					} else if (!rd_skip(&rq, rw)) goto bad;
				}
			}
			msg.p += l;
		} else if (!rd_skip(&msg, mw)) goto bad;
	}
	(void)encrypted;

	buf_t inner;
	buf_init(&inner);
	const char *reject = NULL;

	if (!has_request) {
		buf_free(&inner);
		buf_free(&env.generic_data);
		return true;
	}

	if (q.version_major != 0 && q.version_major != 1)
		reject = "UNSUPPORTED_IPC_VERSION";

	if (!reject) {
		switch (q.request_field) {
		case REQ_createSDKInstance: {
			if (q.client_pid != s->client_pid) {
				reject = "CLIENT_PID_MISMATCH";
				break;
			}
			/* CreateSDKInstanceResponse{1: gfeVer, 2: major, 3: minor, 4: scopes} */
			resp_ok_empty(&inner);
			/* rewrite inner with response payload */
			inner.len = 0;
			buf_put_tag(&inner, 1, 0); buf_put_varint(&inner, 0);
			buf_put_str(&inner, 1, GFE_VERSION, strlen(GFE_VERSION));
			buf_put_tag(&inner, 2, 0); buf_put_varint(&inner, 1);
			buf_put_tag(&inner, 3, 0); buf_put_varint(&inner, 0);
			for (uint32_t sc = 1; sc <= 7; sc++) {
				buf_t sp;
				buf_init(&sp);
				buf_put_tag(&sp, 1, 0); buf_put_varint(&sp, sc);
				buf_put_tag(&sp, 2, 0); buf_put_varint(&sp, 1);
				buf_put_msg(&inner, 4, &sp);
				buf_free(&sp);
			}
			s->authorized = true;
			snprintf(s->game_name, sizeof(s->game_name), "%s",
				 q.game_name[0] ? q.game_name : q.client_process_name);
			LOGI("authorized game: '%s'", s->game_name);
			break;
		}
		case REQ_configureHighlights: {
			if (!s->authorized) { reject = "SDK_INSTANCE_REQUIRED"; break; }
			if (q.n_definitions > 256) { reject = "TOO_MANY_DEFINITIONS"; break; }
			s->n_definitions = q.n_definitions;
			memcpy(s->definitions, q.defs, sizeof(s->definitions));
			char list[512] = {0};
			int off = 0;
			for (int i = 0; i < q.n_definitions && off < 480; i++)
				off += snprintf(list + off, sizeof(list) - off, "%s%s", i ? ", " : "", q.defs[i]);
			LOGI("event definitions registered: %s", list);
			resp_ok_empty(&inner);
			break;
		}
		case REQ_openGroup: {
			if (!s->authorized) { reject = "SDK_INSTANCE_REQUIRED"; break; }
			if (!q.group_id[0]) { reject = "INVALID_GROUP_ID"; break; }
			add_group(s, q.group_id);
			snprintf(s->current_group, sizeof(s->current_group), "%s", q.group_id);
			LOGI("match started: '%s'", q.group_id);
			resp_ok_empty(&inner);
			break;
		}
		case REQ_closeGroup: {
			if (!s->authorized) { reject = "SDK_INSTANCE_REQUIRED"; break; }
			int gi = find_group(s, q.group_id);
			if (gi < 0) { reject = "GROUP_NOT_FOUND"; break; }
			LOGI("match ended: '%s'", q.group_id);
			resp_ok_empty(&inner);
			break;
		}
		case REQ_setVideoHighlight: {
			if (!s->authorized) { reject = "SDK_INSTANCE_REQUIRED"; break; }
			int gi = find_group(s, q.group_id);
			if (gi < 0) { reject = "GROUP_NOT_FOUND"; break; }
			bool known = false;
			for (int i = 0; i < s->n_definitions; i++)
				if (!strcmp(s->definitions[i], q.highlight_id)) known = true;
			if (!known) { reject = "HIGHLIGHT_NOT_CONFIGURED"; break; }
			if (!planner_register(q.highlight_id, q.group_id)) {
				reject = "CAPTURE_NOT_READY_OR_QUEUE_FULL";
				break;
			}
			s->group_counts[gi]++;
			resp_ok_empty(&inner);
			break;
		}
		case REQ_getUserConfig: {
			if (!s->authorized) { reject = "SDK_INSTANCE_REQUIRED"; break; }
			inner.len = 0;
			buf_put_tag(&inner, 1, 0); buf_put_varint(&inner, 1); /* gameEnabled */
			for (int i = 0; i < s->n_definitions; i++) {
				buf_t hc;
				buf_init(&hc);
				buf_put_str(&hc, 1, s->definitions[i], strlen(s->definitions[i]));
				buf_put_tag(&hc, 2, 0); buf_put_varint(&hc, 1);
				buf_put_msg(&inner, 2, &hc);
				buf_free(&hc);
			}
			break;
		}
		case REQ_getUILanguage: {
			inner.len = 0;
			buf_put_tag(&inner, 1, 0); buf_put_varint(&inner, 0);
			buf_put_str(&inner, 1, "en-US", 5);
			break;
		}
		case REQ_isClientAlive: {
			inner.len = 0;
			buf_put_tag(&inner, 1, 0); buf_put_varint(&inner, 0);
			buf_put_tag(&inner, 1, 0); buf_put_varint(&inner, 1); /* alive=true */
			break;
		}
		case REQ_getNumberOfHighlightsInGroup: {
			int gi = find_group(s, q.group_id);
			inner.len = 0;
			buf_put_tag(&inner, 1, 0); buf_put_varint(&inner, 0);
			buf_put_tag(&inner, 1, 0);
			buf_put_varint(&inner, (uint64_t)(gi >= 0 ? s->group_counts[gi] : 0));
			break;
		}
		case REQ_destroySDKInstance: {
			s->authorized = false;
			s->game_name[0] = 0;
			s->n_definitions = 0;
			s->n_groups = 0;
			resp_ok_empty(&inner);
			break;
		}
		case REQ_requestPermission:
		case REQ_openGroupSummary:
			resp_ok_empty(&inner);
			break;
		case REQ_setScreenshot:
		case REQ_toggleOverlay:
		case REQ_opsOptimize:
			reject = "NOT_IMPLEMENTED_BY_STANDALONE_VIDEO_RECORDER";
			break;
		default:
			reject = "UNRECOGNIZED_REQUEST";
			break;
		}
	}

	if (reject) {
		resp_error(&inner, reject);
		LOGW("refused request #%d: %s", q.request_field, reject);
	}

	/* wrap: SDKIPCMessage{2: response=inner} -> generic{type:2} -> BusMessage */
	buf_t ipc;
	buf_init(&ipc);
	buf_put_msg(&ipc, 2, &inner);
	build_reply(out_replies, env.ref_uid, strlen(env.ref_uid),
		    "NvNode", "SDKAPI", env.tgt_sys, env.tgt_mod, 2, &ipc, false, false);
	buf_free(&ipc);
	buf_free(&inner);
	buf_free(&env.generic_data);
	return true;

bad:
	buf_free(&env.generic_data);
	return false;
}
