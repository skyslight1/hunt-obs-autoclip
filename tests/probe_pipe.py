import sys, os, json, base64
sys.path.insert(0, "protocol")
import win32file, win32pipe
import BusMessage_pb2 as bus, sdk_pb2 as sdk

PIPE = r"\\.\pipe\NvMessageBusBroadcast"
try:
    win32pipe.WaitNamedPipe(PIPE, 5000)
except Exception as e:
    print("WAIT FAIL:", e); sys.exit(2)
h = win32file.CreateFile(PIPE, win32file.GENERIC_READ | win32file.GENERIC_WRITE,
                         0, None, win32file.OPEN_EXISTING, 0, None)

def send(p): win32file.WriteFile(h, len(p).to_bytes(4, "big") + p)

def recv():
    _, hdr = win32file.ReadFile(h, 4)
    while len(hdr) < 4:
        _, more = win32file.ReadFile(h, 4 - len(hdr)); hdr += more
    n = int.from_bytes(hdr, "big"); buf = b""
    while len(buf) < n:
        _, c = win32file.ReadFile(h, n - len(buf)); buf += c
    return buf

def parse_sdk(raw):
    m = bus.BusMessage(); m.ParseFromString(raw)
    r = sdk.SDKIPCMessage(); r.ParseFromString(m.generic.data)
    return r.response

frames = {f["name"]: base64.b64decode(f["base64"])
          for f in json.load(open("tests/python-wire.json"))}

send(frames["join"]); a, b = recv(), recv()
print("join: 2 replies:", len(a) > 0 and len(b) > 0)

m = bus.BusMessage(); m.ParseFromString(frames["create"])
inner = sdk.SDKIPCMessage(); inner.ParseFromString(m.generic.data)
inner.request.createSDKInstanceRequest.clientPID = os.getpid()
m.generic.data = inner.SerializeToString()
send(m.SerializeToString())
r = parse_sdk(recv())
print("create:", r.retval, r.retstatus)

send(frames["configure"])
r = parse_sdk(recv())
print("configure:", r.retval, r.retstatus)

send(frames["open"])
r = parse_sdk(recv())
print("open:", r.retval, r.retstatus)

send(frames["kill"])
r = parse_sdk(recv())
print("kill:", r.retval, r.retstatus)

send(frames["close"])
r = parse_sdk(recv())
print("close:", r.retval, r.retstatus)
h.close()
print("PROBE OK" if r.retval == 0 else "PROBE FAIL")
sys.exit(0 if r.retval == 0 else 1)
