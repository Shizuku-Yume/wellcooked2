# 跨端协议：KitchenOS Bridge ↔ WellCooked 插件

这是两端**唯一**的契约。字段名、消息类型名以本文档和 `schema/` 为准，两端都不许私自改名。

改动顺序：先改 `schema/` 与本文档，再同步两端实现 ——
C 侧 `bridge/include/kitchenos/protocol.h` + `bridge/src/protocol.c`，
插件侧 `plugin/WellCooked.Plugin/Protocol/Messages.cs`。

分帧的契约就是本文档第 1 节，两端实现分别是插件侧
`plugin/WellCooked.Plugin/Net/TcpLink.cs` 与 C 侧 `bridge/src/framing.c`。注意这两端是同一轮
写成的，所以"两边一致"本身不算独立证据：能证伪它的是 `plugin/tools/conformance/`（拿插件
自己的源码编译出一个对端，跑跨实现检查）与那次实机运行。JSON 子集（对象/数组/字符串/数字/
布尔/null）由 `bridge/src/json.c` 处理。

## 1. 传输与分帧

- TCP，默认端口 **14455**，两端同一个号，谁在听由模式决定。Bridge 默认 `--link listen`：
  绑 `127.0.0.1:14455` 等插件拨入；加 `--link connect` 时 Bridge 反过来去连插件开的服务端，
  插件默认监听 `127.0.0.1:14455`。方向由模式表达，端口号不承担这个责任。
- 绑定地址默认 **loopback**。链路没有认证，绑 `0.0.0.0` 等于把"谁能当插件"交给同网段的
  任何人；跨机演示时显式传 `--host 0.0.0.0`。
- 两端都可配置：Bridge 用 `--port` / `--host`；插件侧下面列出的 `Network.*` 是 **net46 参考实现**
  （`plugin/WellCooked.Plugin/`）的配置键，**net35 演示插件不读它们** —— 它只读
  `BepInEx/wellcooked-takeover.txt` 里的 `bridge=` / `state_ms=`（见 `plugin/README.md`）。
  （`decisions.md: 2026-10-15 - Bridge 端口可配置，默认 14455`）
  两种拓扑跑同一套协议。
- 一帧 = **一行**：UTF-8 JSON 字节，后面跟一个 **`\n`（0x0A）**。换行符是分隔符，不属于载荷。
- 单行载荷上限 **1048576 字节（1 MiB）**，与 `TcpLink.MaxFrameBytes` 一致（不含结尾的 `\n`）。
- 上限只对"行"生效：没有 `\n` 时收到超过 1 MiB 字节，说明这一行永远不会结束 → **分帧错误**。
  已经用 `\n` 收尾但内容超过 1 MiB 同样是**分帧错误**。这表示流已经失步（没有换行符就没有
  重新同步的位置），正确反应是断开重连并从连接开头重新对齐，不能靠猜下一行的位置恢复。
- 空行（紧跟一个 `\n`）在分帧层是合法帧（空载荷）。空载荷过不了 JSON 解析，按下面"解析失败"处理。
- 一个 `read` 里可能带多行，一行也可能跨多次 `read`：接收侧必须缓冲，不能假定一次读一行。
- 单行超限时插件侧 `TcpLink.FlushOutbound` 直接丢该消息不发送；因此正常对端不会产生超限行。
- 载荷里的裸 `\n` 不可表示（读出来就是两行），两侧的编码器都必须拒收这种消息而不是发出去。

## 2. 消息总览

| type | 方向 | C 侧能解析 | C 侧能构造 |
|---|---|---|---|
| `state_update` | 插件 → Bridge | 是 | 否（插件上报） |
| `heartbeat` | 双向 | 是 | 是 |
| `hello` | 双向 | 是 | 是 |
| `echo` | 双向 | 是 | 否 |
| `echo_reply` | 双向 | 是 | 是 |
| `ack` | 双向 | 是 | 是 |
| `action` | Bridge → 插件 | 是（联调/测试用） | 是 |
| `config` | Bridge → 插件 | 是 | 否（本任务范围外） |
| `state_request` | Bridge → 插件 | 仅识别类型名 | 否（本任务范围外） |

`schema/` 目录里每个消息一个 JSON Schema（draft 2020-12）：
`state_update`、`action`、`heartbeat`、`config`、`ack`、`hello`、`echo`、`echo_reply`。

## 3. 字段

约定：标"必填"的字段缺失时，C 侧解析返回 `PROTO_ERR_MISSING` 并丢弃整条消息；
非必填字段缺失（或显式为 `null`）时取默认值（数字 0、字符串 `""`）。`schema/` 里对发送端
确实会发 `null` 的字段显式标成 `["string","null"]`（`holding`、`content`、`recipe`）；
其余非必填的标量字段两端实现同样按"缺省"处理 `null`，schema 还没逐个标注，属待补，
不是拒绝一条消息的理由。
字段存在但 JSON 类型不对（例如 `"chef_id": "0"`）一律 `PROTO_ERR_RANGE`：那是发送端的 bug，
要进日志。所有消息都必须带 `type`。

### 3.1 `state_update`（插件 → Bridge）

一帧世界状态。Bridge 每收到一条就当作一帧处理（帧屏障以它划界）。

| 字段 | 类型 | 必填 | C 侧字段 | 说明 |
|---|---|---|---|---|
| `type` | string | 是 | — | 固定 `"state_update"` |
| `timestamp` | integer | 否 | `timestamp` | Unix 秒 |
| `game_time` | number | 否 | `game_time` | 游戏内已进行秒数 |
| `game_time_ms` | integer | 是 | `game_time_ms` | 游戏内已进行毫秒数（暂停不走；缺失当错误被拒） |
| `score` | number | 否 | `score` | 当前分数 |
| `chefs` | array | 否 | `chefs[]` | 最多 16 个，见下 |
| `orders` | array | 否 | `orders[]` | 最多 32 张，见下 |
| `resources` | object | 否 | `resources[]` | 最多 48 个键，见下 |

`chefs[]`：

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `id` | integer | 是 | 厨师编号，同时也是输入槽编号；没有 `id` 的条目会被单独丢弃并计数 |
| `position` | object `{x,y}` | 否 | 世界坐标（映射游戏 x/z 平面） |
| `holding` | string \| null | 否 | 手上的东西；`null` 表示空手，C 侧存成 `""`（`proto_chef.holding`） |
| `state` | string | 否 | 取自游戏，例如 `moving` / `idle` |
| `facing` | number | 否 | 厨师朝向（弧度） |

`orders[]`：

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `id` | string | 否 | 例如 `order_001` |
| `recipe` | string | 否 | 例如 `salad` |
| `time_remaining` | number | 否 | 剩余秒数 |
| `ingredients` | string[] | 否 | 已放入的原料，最多 8 个 |

`resources` 的**键是动态的**：`pot_0`、`pot_1`、`cutting_board_0`、`plate_stack`、
`dirty_plates`、`sink_0`。
将来游戏新增 `pot_7`、`sink_2` 之类也必须能收，所以 C 侧按前缀分类后保留**原始键名**，
认不出的键归 `PROTO_RES_UNKNOWN`，绝不因为"键不认识"而拒绝整条消息。

| 键形式 | C 侧 kind | index |
|---|---|---|
| `pot_<N>` | `PROTO_RES_POT` | N |
| `cutting_board_<N>` | `PROTO_RES_CUTTING_BOARD` | N |
| `plate_stack` | `PROTO_RES_PLATE_STACK` | -1 |
| `dirty_plates`、`sink_<N>` 等其它 | `PROTO_RES_UNKNOWN` | -1 |

每个键的值是一个对象：

| 字段 | 类型 | 必填 | C 侧字段 | 说明 |
|---|---|---|---|---|
| `state` | string | 否 | `state` | `free` / `occupied` … |
| `content` | string \| null | 否 | `content` | 锅里在煮什么 |
| `time_left` | number | 否 | `time_left` + `has_time_left` | 剩余烹饪时间 |
| `chef_id` | integer | 否 | `chef_id` + `has_chef_id` | 占用该工位的厨师 |
| `count` | integer | 否 | `count` + `has_count` | 盘子数量（`plate_stack`）；`0` 与"没这个字段"不同，靠 `has_count` 区分 |
| `progress` | number | 否 | `progress` + `has_progress` | 烹饪/切菜/洗盘进度（0..1） |

样例见 `overcooked_os_course_project.md` §5.2；`bridge/tests/test_protocol.c` 逐字段核对了那份样例，
并额外覆盖了样例里没有的 `pot_7` 与未知键。

### 3.2 `heartbeat`（双向）

| 字段 | 类型 | 必填 |
|---|---|---|
| `type` | string | 是（`"heartbeat"`） |
| `timestamp` | integer | 是（Unix 秒） |

C 侧构造：`{"type":"heartbeat","timestamp":1234567890}`。

发送间隔与超时都是各端自己的配置，两边默认值不一样：

| 端 | 配置 | 默认 |
|---|---|---|
| 插件 | `Network.HeartbeatMs` | **2000 ms**（`Plugin.cs`） |
| 插件 | `Network.HeartbeatTimeoutMs` | **0 = 关闭该判定**；开着时按"**任何**消息都没来"算 |
| Bridge | `--heartbeat-interval-ms` | 1000 ms |
| Bridge | `--heartbeat-timeout-ms` | **5000 ms，且只看心跳**（`state_update` 一直流也不算活着） |

两侧默认余量 **3 秒**：插件把 `HeartbeatMs` 配到 ≥5000，就会一边正常上报状态、一边被
Bridge 判为失联（断链、清空注入、3 秒后重连）。要改这个配对，先看
`decisions.md`: 2026-10-15 - 链路存活只看心跳。`--heartbeat-timeout-ms 0` 可关掉 Bridge 侧判定。

### 3.3 `hello`（双向）

用来在连接建立后说明身份。

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `type` | string | 是（`"hello"`） | |
| `instance` | string | 是 | Bridge 侧固定 `"linux-bridge"`（`PROTO_BRIDGE_INSTANCE`） |
| `version` | string | 是 | 协议版本，见 §6 |
| `actions` | string[] | 否 | 本端支持的动作名，最多 32 个 |

C 侧构造样例：
`{"type":"hello","instance":"linux-bridge","version":"0.1.0","actions":["move","stop","interact"]}`

### 3.4 `echo` / `echo_reply`

联调第一步：一端发什么 token，另一端原样回。

| 消息 | 字段 | 类型 | 必填 |
|---|---|---|---|
| `echo` | `type` | string | 是（`"echo"`） |
| `echo` | `token` | string | 是 |
| `echo_reply` | `type` | string | 是（`"echo_reply"`） |
| `echo_reply` | `token` | string | 是（照抄收到的 token） |

### 3.5 `ack`（双向）

对一条 `action` 的执行回执。**同一条动作可以回两次**（2026-10-16，任务 04 实测后定）：

1. **收到就回一次**：`ok=true` 表示指令已登记并开始执行 —— 上层据此知道消息没丢；
2. **有结局再回一次**：`move` 到达时 `ok=true`（`detail` 写"到达 (x,y)，用时 N 秒"）、
   15 秒没走到时 `ok=false`（`detail` 写还差多少米）；没实现的动作名当场回 `ok=false`。

第二次回执是可选的（没有结局就不回，例如按键类动作没有"到达"这回事），但**超时必须回一次**，
否则上层会永远等一条不会来的结果。

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `type` | string | 是（`"ack"`） | |
| `action` | string | 是 | 被回执的动作名 |
| `chef_id` | integer | 是 | 哪个厨师 |
| `ok` | boolean | 是 | 必须是 JSON 布尔，不接受 `"true"` 字符串 |
| `seq` | integer | 否 | `path` 的规划序号；插件在 `path` 的两次 ack 里都带回，其它动作不带 |
| `detail` | string | 否 | 失败原因等，可读文本 |

C 侧构造样例：
`{"type":"ack","action":"move","chef_id":1,"ok":true,"detail":"ok"}`；
`path` 的结局 ack：`{"type":"ack","action":"path","chef_id":1,"ok":true,"seq":7,"detail":"path 到达终点 …"}`

### 3.6 `action`（Bridge → 插件）

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `type` | string | 是（`"action"`） | |
| `chef_id` | integer | 是 | 目标厨师 |
| `action` | string | 是 | `move` / `stop` / `interact` / `pickup` / `dash` / `path` / `face` / `work` |
| `target` | object `{x,y}` | `move` 必填，`face` 可选 | 目标坐标 |
| `target_object` | string | `interact`/`work` 必填，`face` 可选 | 例如 `cutting_board_0`；`work` 按住加工键直到下一条动作 |
| `waypoints` | array | `path` 必填 | 1..8 个相邻格中心坐标 `{x,y}` |
| `seq` | integer | `path` 必填 | 规划序号，同 seq 只追加路点 |
| `button` | string | 否 | 附加按键，插件透传给游戏 |
| `hold_ms` | number | 否 | 长按时长 |
`path` 的 `seq` 是规划序号：同 `seq` 的新消息只把新路点接在队尾，插件不重置已走过的路点；
`face` 同时给了 `target` 与 `target_object` 时以 `target_object` 为准（桥只下发其中一个）。
`stop`、`pickup`、`dash` 都是**只有名字**的动作（不带任何附加字段）：`stop` 是空转，
某个厨师的输入槽缺失或超时时 Bridge 就对他下发
`{"type":"action","chef_id":N,"action":"stop"}`；`pickup`（取/放厨师脚下的东西）与 `dash`
（朝当前朝向冲刺）来自输入槽的 `kos_action_kind`，插件 `InputInjector` 本来就实现了它们
（它一共八个动作），2026-10-15 起允许被调度下发。插件的 `press`/`release`/`tap` 仍然只在
插件内部可用 —— 需要按帧精确调度原始按键时再扩这里。

C 侧构造样例（前两条与 §5.2 的两条控制指令一致）：

- `{"type":"action","chef_id":0,"action":"move","target":{"x":12,"y":6}}`
- `{"type":"action","chef_id":1,"action":"interact","target_object":"cutting_board_0"}`
- `{"type":"action","chef_id":0,"action":"pickup"}`
- `{"type":"action","chef_id":1,"action":"dash"}`
- `{"type":"action","chef_id":0,"action":"path","seq":1,"waypoints":[{"x":12,"y":6},{"x":12,"y":7}]}`
- `{"type":"action","chef_id":0,"action":"face","target":{"x":13,"y":6}}`
- `{"type":"action","chef_id":1,"action":"work","target_object":"cutting_board_0"}`

### 3.7 `config`（Bridge → 插件）

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `type` | string | 是（`"config"`） | |
| `strategy` | string | 是 | 例如 `EDF` |
| `order_list` | array | 否 | 每项 `{id, recipe, arrival_time, deadline}` |

### 3.8 `state_request`（Bridge → 插件）

只保证类型名与插件一致（`state_request`）。字段（`state_interval_ms`、`input_mode`）由插件
`InboundMessage` 定义，C 侧本任务只识别类型名，不做字段解析。

## 4. 错误处理约定

| 情况 | 处理 | 谁负责记日志 |
|---|---|---|
| 一行超过 1 MiB（有 `\n` 收尾或没有 `\n` 都算） | 分帧错误：断开重连，不尝试重新对齐 | Bridge（`logs/bridge.log`） |
| 行内不是合法 JSON（含空行） | 丢弃这一条消息，**保持连接** | Bridge |
| JSON 合法但字段缺失/类型不对 | 丢弃这一条消息（数组里的坏条目单独丢弃并计数） | Bridge |
| 数组超过 §5 的上限 | 保留前 N 条，其余丢弃并计数，消息仍然可用 | Bridge |
| 字符串超过 C 侧字段长度 | 截断并计数 | Bridge |
| 心跳超时（Bridge 只看心跳；插件侧默认关闭该判定，开着时看"任何消息"） | 判定插件断开，暂停下发，重连后继续 | Bridge |
| TCP 断开 | 等待重连（客户端模式按间隔重试） | Bridge |

C 侧错误码：`bridge/include/kitchenos/json.h` 的 `JSON_ERR_*`、
`bridge/include/kitchenos/framing.h` 的 `FRAMING_ERR_*`、
`bridge/include/kitchenos/protocol.h` 的 `PROTO_ERR_*`。解析失败带上下文（`json_error` 里有
字节偏移、行列和一句话说明），够写一条能定位的日志。

## 5. 上限（C 侧固定布局）

| 名称 | 值 | 超出时 |
|---|---|---|
| 单行载荷 | 1048576 字节 | 分帧错误，断开 |
| `chefs` | 16 | 丢弃多出的条目并计数 |
| `orders` | 32 | 同上 |
| 每张订单的 `ingredients` | 8 | 同上 |
| `resources` 键 | 48 | 同上 |
| `hello.actions` | 32 | 同上 |
| 字符串字段 | 64 字节（`PROTO_STR_MAX`），资源键/原料名 32 字节（`PROTO_KEY_MAX`） | 截断并计数 |
| JSON 嵌套深度 | 64 | 解析错误，丢弃该消息 |

计数通过 `proto_diag`（`dropped_items` / `truncated_strings`）返回，Bridge 把它写进日志。

## 6. 版本与兼容

- 当前版本 `0.1.0`（`bridge/include/kitchenos/protocol.h` 的 `PROTO_VERSION`，
  同时是插件 `WellCooked.Plugin` 的版本基准）。
- 加法兼容：新增可选字段、新增 `resources` 键、新增消息类型 —— 老实现忽略即可，不必改版本。
- 破坏兼容：改字段名、改字段类型、改动作名、改分帧 —— 必须同时改两端并提升版本号。
- 共享内存的结构版本号与协议版本是两件事，见 `bridge/include/kitchenos/shm.h`。
