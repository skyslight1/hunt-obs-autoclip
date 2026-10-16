"""Quick diagnostic script: query whether OBS Replay Buffer is currently active via obs-websocket."""
import obsws_python, json, os
cfg = json.load(open(os.path.expandvars(r"%APPDATA%") + r"\obs-studio\plugin_config\obs-websocket\config.json", encoding="utf-8"))
req = obsws_python.ReqClient(host="127.0.0.1", port=4455, password=cfg["server_password"], timeout=5)
print("RB active:", req.get_replay_buffer_status().output_active)
req.disconnect()
