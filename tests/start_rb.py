"""Quick diagnostic script: start OBS Replay Buffer via obs-websocket and verify it became active."""
import obsws_python, json, os, time
cfg = json.load(open(os.path.expandvars(r"%APPDATA%") + r"\obs-studio\plugin_config\obs-websocket\config.json", encoding="utf-8"))
req = obsws_python.ReqClient(host="127.0.0.1", port=4455, password=cfg["server_password"], timeout=5)
req.start_replay_buffer()
time.sleep(3)
print("RB active after start:", req.get_replay_buffer_status().output_active)
req.disconnect()
