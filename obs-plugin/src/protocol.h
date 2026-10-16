/* hunt-obs-autoclip OBS plugin — protocol + session + planner.
 *
 * Hand-written protobuf wire decoding/encoding for the exact subset of the
 * NVIDIA GfeSDK IPC that Hunt's Highlights client uses. Behavioral reference:
 * Mlovo/HuntOpenHighlights HighlightsSession.cs (MIT). Golden frames in
 * ../tests reproduce this traffic byte-for-byte.
 */
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ---- dynamic buffers ---- */
typedef struct {
	uint8_t *p;
	size_t len, cap;
} buf_t;

void buf_init(buf_t *b);
void buf_free(buf_t *b);
void buf_put(buf_t *b, const void *data, size_t n);
void buf_put_u8(buf_t *b, uint8_t v);
void buf_put_varint(buf_t *b, uint64_t v);
void buf_put_tag(buf_t *b, uint32_t field, int wire);
void buf_put_str(buf_t *b, uint32_t field, const char *s, size_t len);
void buf_put_msg(buf_t *b, uint32_t field, const buf_t *sub);

/* ---- wire frame IO (4-byte big-endian length prefix) ---- */
/* returns 1 frame ok, 0 clean EOF, -1 error */
int frame_read(HANDLE pipe, buf_t *out);
bool frame_write(HANDLE pipe, const uint8_t *data, size_t len);

/* ---- session ---- */
typedef struct session session_t;

session_t *session_create(uint32_t client_pid);
void session_destroy(session_t *s);
const char *session_game_name(const session_t *s);

/* Process one incoming BusMessage frame; appends 0..2 reply frames to out.
 * Returns false on fatal protocol error (caller closes pipe). */
bool session_process(session_t *s, const uint8_t *frame, size_t len, buf_t *out);

/* ---- planner (coalescing) ---- */
void planner_reset(void);
/* returns true if the event was accepted (game gets SUCCESS) */
bool planner_register(const char *highlight_id, const char *group_id);
/* called periodically; fires coalesced clips: returns true when a save
 * should happen now (and resets the throttle). */
bool planner_poll_should_save(void);

/* shim install target (no NVIDIA environment path) */
#define SHIM_DIR "C:\NvidiaShim"
#define DLL_SHA256 "64e300997b5796da22ad8baa49e4eb919cdc170157cbe6770279f94508a84650"

#define PIPE_NAME "\\\\.\\pipe\\NvMessageBusBroadcast"
