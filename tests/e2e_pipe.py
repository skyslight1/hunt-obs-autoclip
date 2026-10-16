#!/usr/bin/env python3
"""End-to-end pipe test: starts the real pipe server in a subprocess (dry-run
mode), then connects as a real pipe client and replays the golden wire frames
exactly as the game would, asserting every reply byte-parses and the kill
frame triggers a dry-run SAVE TRIGGER log line.

Run:  .venv/Scripts/python.exe tests/e2e_pipe.py
"""
from __future__ import annotations

import base64
import json
import subprocess
import sys
import time
from pathlib import Path

import win32file
import win32pipe

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "protocol"))

PIPE_NAME = r"\\.\pipe\NvMessageBusBroadcast"
LOG_FILE = Path(ROOT / "tests" / "e2e_server.log")


def read_frame(handle) -> bytes:
    _, header = win32file.ReadFile(handle, 4)
    while len(header) < 4:
        _, more = win32file.ReadFile(handle, 4 - len(header))
        header += more
    length = int.from_bytes(header, "big")
    payload = b""
    while len(payload) < length:
        _, chunk = win32file.ReadFile(handle, length - len(payload))
        payload += chunk
    return payload


def main() -> int:
    import BusMessage_pb2 as bus_pb2
    import sdk_pb2 as sdk_pb2

    if LOG_FILE.exists():
        LOG_FILE.unlink()
    server = subprocess.Popen(
        [str(ROOT / ".venv" / "Scripts" / "python.exe"), str(ROOT / "hunt_obs_autoclip.py"),
         "--dry-run", "--verbose", "--coalesce-gap", "1", "--min-interval", "0"],
        cwd=ROOT, stdout=open(LOG_FILE, "w", encoding="utf-8"), stderr=subprocess.STDOUT,
    )
    try:
        # wait for pipe creation
        deadline = time.time() + 15
        while time.time() < deadline:
            try:
                win32pipe.WaitNamedPipe(PIPE_NAME, 1000)
                break
            except Exception:
                if server.poll() is not None:
                    raise RuntimeError(f"server exited early: {server.returncode}")
                time.sleep(0.3)
        else:
            raise RuntimeError("pipe never appeared")

        handle = win32file.CreateFile(
            PIPE_NAME, win32file.GENERIC_READ | win32file.GENERIC_WRITE,
            0, None, win32file.OPEN_EXISTING, 0, None)

        frames = json.loads((ROOT / "tests" / "python-wire.json").read_text(encoding="utf-8"))
        # The server verifies createSDKInstanceRequest.clientPID against the real
        # pipe client PID (anti-spoofing). Rewrite the golden frame's fake PID
        # (42) to our actual PID before sending, like the real game does.
        import os
        my_pid = os.getpid()
        for f in frames:
            if f["name"] != "create":
                continue
            msg = bus_pb2.BusMessage()
            msg.ParseFromString(base64.b64decode(f["base64"]))
            inner = sdk_pb2.SDKIPCMessage()
            inner.ParseFromString(msg.generic.data)
            inner.request.createSDKInstanceRequest.clientPID = my_pid
            msg.generic.data = inner.SerializeToString()
            f["base64"] = base64.b64encode(msg.SerializeToString()).decode("ascii")
        events_seen, errors = [], []
        for f in frames:
            payload = base64.b64decode(f["base64"])
            win32file.WriteFile(handle, len(payload).to_bytes(4, "big") + payload)
            replies = []
            # read all reply frames for this request (JOIN produces 2)
            import google.protobuf.message
            while True:
                try:
                    payload = read_frame(handle)
                except Exception as exc:
                    errors.append(f"read failed at {f['name']}: {exc}")
                    break
                msg = bus_pb2.BusMessage()
                try:
                    msg.ParseFromString(payload)
                except google.protobuf.message.DecodeError as exc:
                    errors.append(f"reply not parseable at {f['name']}: {exc}")
                    break
                replies.append(msg)
                if not msg.joined and msg.HasField("generic") and msg.generic.data:
                    inner = sdk_pb2.SDKIPCMessage()
                    inner.ParseFromString(msg.generic.data)
                    if inner.WhichOneof("message") == "response":
                        if inner.response.retval != 0:
                            errors.append(f"{f['name']}: refused {inner.response.retstatus}")
                        if f["name"] == "kill":
                            events_seen.append(inner.response.retstatus)
                    break  # one SDK response per request; JOIN ack already counted
                if msg.joined and msg.source_system == "NvNode":
                    continue  # second JOIN reply pending
                break

        handle.close()

        time.sleep(2.0)  # allow coalesce window + worker poll
        server.terminate()
        server.wait(timeout=10)
        log = LOG_FILE.read_text(encoding="utf-8", errors="replace")

        ok = True
        def check(name, cond, detail=""):
            nonlocal ok
            print(f"  {'PASS' if cond else 'FAIL'}  {name}" + (f"  ({detail})" if not cond else ""))
            ok = ok and cond

        print("[e2e] real pipe client -> real pipe server (dry-run)")
        check("no protocol errors", not errors, "; ".join(errors))
        check("kill request answered SUCCESS", events_seen == ["SUCCESS"], str(events_seen))
        check("server logged game authorization", "authorized game" in log)
        check("server logged event definitions", "HUNTER_KILLED" in log and "PLAYER_DOWNED" in log)
        check("server logged match start", "match started" in log)
        check("SAVE TRIGGER fired in dry-run", "SAVE TRIGGER" in log)
        check("event log written", (ROOT / "events.jsonl").exists())
        return 0 if ok else 1
    finally:
        if server.poll() is None:
            server.terminate()
            server.wait(timeout=10)


if __name__ == "__main__":
    sys.exit(main())
