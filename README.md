# hunt-obs-autoclip

猎杀对决（Hunt: Showdown 1896）击杀自动剪辑：游戏检测到你击杀/击倒猎人时，自动触发 OBS 保存回放缓冲区（Replay Buffer）为片段文件。

原理：本工具在本机创建 NVIDIA MessageBus 命名管道（`\\.\pipe\NvMessageBusBroadcast`）的**服务端**，扮演 GeForce Experience。游戏内置的 NVIDIA Highlights SDK 会主动连上来并把击杀事件（`HUNTER_KILLED` / `PLAYER_DOWNED`）发过来——不注入游戏、不读内存、不模拟按键。事件到达后通过 obs-websocket 调 `SaveReplayBuffer`，OBS 侧保存的片段天然包含事件前 20 秒（游戏建议的 `startDelta=-20000`）。

协议解析层（`protocol/*_pb2.py`）与黄金测试帧（`tests/*.json`）取自 [Mlovo/HuntOpenHighlights](https://github.com/Mlovo/HuntOpenHighlights)（MIT，见 `LICENSE-HuntOpenHighlights.txt`）。

## 其他游戏可以用吗？

**协议层通用，事件 ID 每游戏一换**。本工具实现的是 NVIDIA GfeSDK 的标准 IPC 协议——任何集成了 NVIDIA Highlights SDK 的游戏都走同一条管道、同一套握手。游戏不同只体现在两处，都已在脚本里做成动态的：

1. **事件 ID**：游戏在对局载入时通过 `configureHighlightsRequest` 自己上报事件清单（如 Hunt 的 `HUNTER_KILLED`、`PLAYER_DOWNED`），脚本会用 `--verbose` 日志打印 `event definitions registered: ...`——先空跑一局，把日志里出现的事件 ID 填进 `--events` 即可。
2. **对局分组**：groupId 同样由游戏上报，连杀合并、事件流水账自动按新游戏的分组工作。

集成过 NVIDIA Highlights SDK 的游戏（官方 SDK 2024 年起已是 legacy，但已集成的游戏继续有效）：**PUBG、Fortnite**（官方页点名的两款）、**Hunt: Showdown**，以及 2017-2019 年间采用 UE4 4.18+/Unity 5.6+ 插件或直接集成 SDK 的游戏（当年 GFE 里有 Highlights 开关的游戏基本都算，可在 NVIDIA App/GeForce Experience 的 Highlights 设置里翻历史列表）。**不在名单里的游戏（CS2、Valorant、Apex 等）不会有任何事件**——它们没集成 SDK，管道上什么都不会发生；这类游戏需要另走 GSI/OCR 等事件源（见最初调研），本工具的 OBS 对接半边（Planner + SaveReplayBuffer）依然可以复用。

注意：多款 Highlights 游戏同时装也没问题——管道按连接区分，`Session` 每连接一个，`--events` 可以填多个游戏的事件 ID 并集。

## 前提

- Windows 10/11，Python 3.12+（仅需运行，不需编译）
- OBS Studio 28+（obs-websocket 已内置），已启用 WebSocket 服务器并设置密码（工具 → WebSocket 服务器设置）
- OBS 中：开启 **回放缓冲区**（录制设置里），时长建议 **35 秒**（换算依据见下文"片段多长？"：击杀前 20 秒 pre-roll + 15 秒合并窗口）
- **关闭 NVIDIA App / GeForce Experience 的 Highlights**（或整个 nvcontainer）。两者抢同一个管道，NVIDIA 占住时本工具无法启动

## 使用

```powershell
# 一键启动（首次自动建 venv 装依赖；需要先 cd 到本目录）
powershell -ExecutionPolicy Bypass -File .\start_autoclip.ps1 -DryRun          # 空跑看事件
powershell -ExecutionPolicy Bypass -File .\start_autoclip.ps1 -ObsPassword xxx # 正式运行
# 参数：-ObsHost -ObsPort -CoalesceGap -MinInterval -Events -DryRun -VerboseLog
# 密码也可用环境变量：$env:OBS_WS_PASSWORD = "xxx" 后不带 -ObsPassword 直接跑

# 或手动方式：
python -m venv .venv
.venv\Scripts\pip install protobuf==7.35.1 pywin32 obsws-python
.venv\Scripts\python hunt_obs_autoclip.py --dry-run --verbose
.venv\Scripts\python hunt_obs_autoclip.py --obs-password 你的密码
```

先启动本工具，再启动 Hunt。日志出现 `authorized game` 即握手成功；进对局后出现 `match started`；击杀时出现 `>>> SAVE TRIGGER`，OBS 保存的文件路径会由 `ReplayBufferSaved` 事件打印出来。

参数：

| 参数 | 默认 | 说明 |
|---|---|---|
| `--events` | `HUNTER_KILLED,PLAYER_DOWNED` | 触发的事件 ID（逗号分隔），游戏注册了什么可通过 `--verbose` 日志看到 |
| `--coalesce-gap` | 15 | 连杀合并窗口（秒）：窗口内的多次击杀只触发一次保存 |
| `--min-interval` | 3 | 两次 SaveReplayBuffer 之间的最小间隔（秒） |
| `--obs-host/--obs-port` | 127.0.0.1:4455 | obs-websocket 地址 |

事件流水账写入 `events.jsonl`（时间、事件、对局 ID），可用于事后核对。

## 片段多长？

片段长度恒等于 OBS 回放缓冲区的时长设置 B——本工具只决定"何时按下保存"，不决定存多长。OBS 保存的是触发时刻往前 B 秒的内容。

保存时机 = 该片段计划中**最后一次**击杀后 `--coalesce-gap` 秒（默认 15，用于等待可能跟随的连杀）。设保存时刻为 T、最后一杀为 A：

- 片段 = [T − B, T]，总长恒为 B
- 击杀**之后**收录 T − A = 合并窗口（游戏原生建议只 +5 秒，多出的可事后剪）
- 击杀**之前**收录 B − (T − A)，**所以 B = 想要的击杀前秒数 + coalesce-gap**

| 想要的窗口 | 缓冲时长 B | coalesce-gap |
|---|---|---|
| 击杀前 20s ~ 后 15s（推荐，合并能力强） | 35 | 15（默认） |
| 击杀前 20s ~ 后 10s | 30 | 10 |
| 击杀前 20s ~ 后 5s（游戏原生建议窗口） | 25 | 5 |

连杀合并是链式的（每次与上一击间隔 ≤ coalesce-gap 即并入同一计划），锚点始终是最后一杀：B=35 时最后一杀之前 20 秒内的击杀都完整在片段里，更早的链式击杀会被截在片段前沿。`--min-interval` 节流若推迟了保存，击杀后部分相应变长、击杀前相应变短，总长仍是 B。

细节：OBS 实际从上一个关键帧起存，片段头部可能多 1~2 秒；回放缓冲区还有体积上限（默认 512MB），长缓冲配高码率时注意在录制设置里调大"最大回放缓冲区"。

## 已验证 / 已知未知

**已验证**（本仓库测试）：
- `tests/selftest.py`（22/22）：黄金协议帧逐帧回放——握手、实例创建、事件注册、对局开关、击杀事件字段（`HUNTER_KILLED / match-1896 / -20000 / +5000`）、配置查询、畸形帧拒绝、连杀合并与节流。
- `tests/e2e_pipe.py`（7/7）：真实管道服务端 + 真实管道客户端全链路，含 PID 反伪造校验（clientPID 必须等于管道对端进程）。

**已知未知**（需要进一次真实对局确认）：
1. 真实游戏流量与黄金帧的偏差——HuntOpenHighlights 的作者实测过游戏行为，但其黄金帧是合成样本；真实对局中 SDK 可能发送黄金帧未覆盖的请求（`openGroupSummaryRequest` 等，已按"静默接受"处理）。首次实测请开 `--dry-run --verbose` 观察。
2. 首次对局载入时机——若游戏在本工具启动前就已连接过 NVIDIA 管道（例如 NVIDIA App 曾占用），需要重启游戏让它重新发现管道。
3. 反作弊姿态属于"社区共识安全"（无注入/无内存读取），非 Crytek 官方许可；自用风险自评估。

## 设计说明

- 每条管道连接一个 `Session` 状态机（移植自 HuntOpenHighlights `HighlightsSession.cs`），拒绝加密 IPC，校验关联 ID 与 PID。
- `ClipPlanner`（移植自其 `HighlightPlanner.cs` 的规则）：15 秒窗口内的击杀合并为一个片段计划，避免一连杀 5 个人存 5 个几乎相同的片段；`min_interval` 防止 OBS 侧 buffer 尚未回填就再次保存。
- OBS 侧用 obsws-python：`SaveReplayBuffer` 触发保存，订阅 `ReplayBufferSaved` 事件拿到落盘路径。
- 不包含：录像引擎、采集、编码、UI——这些全部交给 OBS。

## 致谢与许可

- 本项目受 [Mlovo/HuntOpenHighlights](https://github.com/Mlovo/HuntOpenHighlights) 启发，协议定义模块（`protocol/*_pb2.py`）、黄金测试帧（`tests/*wire.json`）及 Session 状态机 / 连杀合并规则源自该项目（MIT 许可，见 [`LICENSE-HuntOpenHighlights.txt`](LICENSE-HuntOpenHighlights.txt)）。
- 本工具自身代码以 MIT 许可提供（见 [`LICENSE`](LICENSE)）。
- **未包含** NVIDIA 专有的 `MessageBus.dll`——若你的机器没装任何 NVIDIA 软件（游戏 SDK 需要它才会连管道），请参照上游 `HuntOpenHighlights` 仓库的 shim 安装说明自行获取；装了 NVIDIA App 的机器则按上文"前提"关闭其 Highlights 即可（游戏 SDK 由驱动自带）。

## 免责声明

本项目为非官方社区工具，与 Crytek GmbH、NVIDIA Corporation 或 OBS Project 无任何关联，亦未获得其认可或背书。"Hunt: Showdown"、"NVIDIA"、"GeForce Experience" 等名称均为其各自所有者的商标，在本文中仅作兼容性说明之用。本工具不注入游戏进程、不读写游戏内存、不修改任何游戏文件；使用者请自行评估使用风险。

