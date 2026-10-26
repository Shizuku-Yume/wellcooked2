using System;
using System.Collections.Generic;
using System.IO;
using BepInEx;
using BepInEx.Logging;
using UnityEngine;

namespace WellCooked
{
    /// <summary>
    /// 插件与 Bridge 的链路运行时（任务 03）：按帧上报世界状态、收心跳与动作、断线自己重连。
    ///
    /// 边界（`specs/game-bridge.md` 的「How it is built」）：这里只做**搬运**——
    /// 状态从游戏读出来编码成 `state_update` 发走，收到的 `action` 交给执行器（任务 04），
    /// 自己不做任何调度决策。
    ///
    /// 配置走 `BepInEx/wellcooked-takeover.txt`（与接管/工序同一个文件，5 秒内生效）：
    ///   `bridge=127.0.0.1:14455`   Bridge 地址；写 `bridge=off` 关掉链路（默认开）
    ///   `state_ms=0`               上报间隔毫秒；0 = 每帧（协议与 spec 要求每帧）
    /// 心跳间隔/超时/重连间隔用参考实现的默认值（2000ms / 关 / 2000ms），见 `protocol/protocol.md` §3.2。
    /// </summary>
    public static class LinkRunner
    {
        private static ManualLogSource _log;
        private static TcpLink _link;
        private static string _configPath;

        private static bool _enabled = true;
        private static string _host = "127.0.0.1";
        private static int _port = 14455;
        private static int _stateMs;
        private static int _configuredStateMs;
        private static string _linkHost;
        private static int _linkPort;
        private static bool _sizeLogged;

        private static float _nextStateAt;
        private static float _nextConfigCheck;
        private static bool _wasConnected;
        private static bool _malformedLogged;
        private static bool _oversizeLogged;

        /// <summary>
        /// 状态帧的**软预算**（不是协议上限）：超了只在日志里警告一次，仍然发送。
        /// 递归 `contents[]` 上线后满帧约 7–9 KB，所以从 4 KB 提到 16 KB；
        /// 协议的单条硬上限是 1 MiB（`TcpLink.MaxFrameBytes`），两边不是一个数。
        /// </summary>
        private const int MaxStateBytes = 16384;

        /// <summary>桥侧靠这个认人：机器名 + 插件角色（写死一个字面量时两台机器看起来一样）。</summary>
        private static readonly string Instance = SafeMachineName() + "-plugin";

        private static string SafeMachineName()
        {
            try
            {
                var name = Environment.MachineName;
                return string.IsNullOrEmpty(name) ? "unknown" : name;
            }
            catch (Exception)
            {
                return "unknown";
            }
        }

        private static readonly List<string> HostActions = new List<string>
        {
            "move", "stop", "press", "release", "tap", "pickup", "dash", "interact",
            "path", "face", "work"
        };

        /// <summary>
        /// 动作执行器（任务 04 装上来）。返回 null 表示做成了，非 null 是失败原因，会回进 `ack`。
        /// 没装时回 `ok=false`，不假装执行过。
        /// </summary>
        public static Func<InboundMessage, string> ActionHandler;

        public static bool Connected
        {
            get { return _link != null && _link.Connected; }
        }

        public static void Install(ManualLogSource log)
        {
            _log = log;
            _configPath = Path.Combine(Paths.BepInExRootPath, "wellcooked-takeover.txt");
            ReadConfig();
            ApplyConfig();
        }

        public static void Tick(float now)
        {
            if (now >= _nextConfigCheck)
            {
                _nextConfigCheck = now + 5f;
                ReadConfig();
                ApplyConfig();
            }

            if (_link == null)
            {
                return;
            }

            PumpInbound();

            bool connected = _link.Connected;
            if (connected && !_wasConnected)
            {
                _link.Send(Msg.Hello(Instance, "0.3.0", HostActions));
                _nextStateAt = 0f; // 连上先立刻报一帧，别等下一个间隔
                _log.LogInfo("link: 已连上 Bridge，" + _host + ":" + _port + "，hello 已发");
            }
            else if (!connected && _wasConnected)
            {
                _log.LogInfo("link: 与 Bridge 断开，后台每 2 秒重试");
                ActionDriver.ReleaseAll(); // 没人在下动作了，把玩家位交还本地驱动
            }

            _wasConnected = connected;

            if (connected && (_stateMs <= 0 || now >= _nextStateAt))
            {
                _nextStateAt = now + _stateMs / 1000f;
                long started = DateTime.UtcNow.Ticks;
                string frame = StateFrame.Build(now);
                int bytes = System.Text.Encoding.UTF8.GetByteCount(frame);
                if (!_sizeLogged)
                {
                    _sizeLogged = true;
                    _log.LogInfo("link: 首帧 state_update " + bytes + " 字节，构建 "
                                 + ((DateTime.UtcNow.Ticks - started) / 10000.0).ToString("0.0") + " ms");
                }
                else if (bytes > MaxStateBytes)
                {
                    // 软预算（16 KB）：超了照样发。丢一条会让帧屏障少一帧，不能丢；
                    // 但对端按自己的固定布局截断，所以还是要说出来，别让人以为没这回事。
                    if (!_oversizeLogged)
                    {
                        _oversizeLogged = true;
                        _log.LogWarning("link: state_update " + bytes + " 字节，超过 "
                                        + MaxStateBytes + " 字节软预算（已发）");
                    }
                }

                _link.Send(frame);
            }
        }

        public static void Stop()
        {
            if (_link != null)
            {
                _link.Stop();
                _link = null;
            }

            _wasConnected = false;
        }

        // ---------------- 配置 ----------------

        private static void ReadConfig()
        {
            if (_configPath == null || !File.Exists(_configPath))
            {
                return;
            }

            try
            {
                foreach (var raw in File.ReadAllLines(_configPath))
                {
                    var line = raw.Trim();
                    if (line.Length == 0 || line.StartsWith("#"))
                    {
                        continue;
                    }

                    int equals = line.IndexOf('=');
                    if (equals <= 0)
                    {
                        continue;
                    }

                    string key = line.Substring(0, equals).Trim().ToLowerInvariant();
                    string value = line.Substring(equals + 1).Trim();
                    if (key == "bridge")
                    {
                        if (value.Equals("off", StringComparison.OrdinalIgnoreCase)
                            || value.Equals("false", StringComparison.OrdinalIgnoreCase)
                            || value == "0")
                        {
                            _enabled = false;
                            continue;
                        }

                        _enabled = true;
                        int colon = value.LastIndexOf(':');
                        if (colon > 0)
                        {
                            _host = value.Substring(0, colon);
                            int parsed;
                            if (int.TryParse(value.Substring(colon + 1), out parsed) && parsed > 0 && parsed < 65536)
                            {
                                _port = parsed;
                            }
                        }
                        else if (value.Length > 0)
                        {
                            _host = value;
                        }
                    }
                    else if (key == "state_ms")
                    {
                        int parsed;
                        if (int.TryParse(value, out parsed) && parsed >= 0)
                        {
                            _configuredStateMs = parsed;
                        }
                    }
                }
            }
            catch (Exception e)
            {
                _log.LogWarning("link: 读配置失败 " + e.Message);
            }
        }

        private static void ApplyConfig()
        {
            if (!_enabled)
            {
                if (_link != null)
                {
                    Stop();
                    _log.LogInfo("link: 配置里关掉了（bridge=off）");
                }

                return;
            }

            if (_link != null && _stateMs != _configuredStateMs)
            {
                _stateMs = _configuredStateMs;
            }

            if (_link != null && (_host != _linkHost || _port != _linkPort))
            {
                // 改了地址就重建：否则新的 bridge= 要等插件重启才生效，而"改文件 5 秒内生效"
                // 是这套配置的约定。
                _log.LogInfo("link: 地址改成 " + _host + ":" + _port + "，重连");
                Stop();
            }

            if (_link != null)
            {
                return;
            }

            _linkHost = _host;
            _linkPort = _port;

            _stateMs = _configuredStateMs;
            _link = new TcpLink(TcpLink.Mode.Client, _host, _port, _host, 2000, 0, 2000);
            _link.Log = LogLine;
            _link.Start();
        }

        // ---------------- 入站 ----------------

        private static void PumpInbound()
        {
            string json;
            int budget = 32; // 每帧上限，避免大流量卡帧
            while (budget-- > 0 && _link.TryDequeueInbound(out json))
            {
                HandleInbound(json);
            }
        }

        private static void HandleInbound(string json)
        {
            InboundMessage m;
            try
            {
                m = JsonUtility.FromJson<InboundMessage>(json);
            }
            catch (Exception e)
            {
                if (!_malformedLogged)
                {
                    _malformedLogged = true;
                    _log.LogWarning("link: 收到解析不了的消息，丢弃（只报一次）: " + e.Message);
                }

                return;
            }

            if (m == null || string.IsNullOrEmpty(m.type))
            {
                return;
            }

            switch (m.type)
            {
                case MsgType.Heartbeat:
                    break; // 链路层保活，不回包
                case MsgType.Hello:
                    _log.LogInfo("link: 对端 hello");
                    break;
                case MsgType.Echo:
                    _link.Send(Msg.EchoReply(m.token));
                    break;
                case MsgType.StateRequest:
                    _link.Send(StateFrame.Build(Time.realtimeSinceStartup));
                    break;
                case MsgType.Config:
                    ApplyRemoteConfig(m);
                    break;
                case MsgType.Action:
                    Diag.Trace("link: 收到 action 原文 " + json);
                    HandleAction(m, json);
                    break;
                default:
                    _log.LogWarning("link: 不认识的消息类型 " + m.type);
                    break;
            }
        }

        private static void ApplyRemoteConfig(InboundMessage m)
        {
            if (m.state_interval_ms > 0 && m.state_interval_ms != _stateMs)
            {
                _stateMs = m.state_interval_ms;
                _log.LogInfo("link: Bridge 把上报间隔改成 " + _stateMs + " ms");
            }

            if (!string.IsNullOrEmpty(m.input_mode))
            {
                _log.LogInfo("link: Bridge 下发 input_mode=" + m.input_mode);
            }
        }

        public static void HandleAction(InboundMessage m, string json)
        {
            // Unity 的 JsonUtility 没把嵌套的 target 对象填进来（实测：move 到了这里 target 是 null，
            // 于是每一条 move 都被拒）。参考实现（net46 那份）同样用它，但那条路从来没被真机跑过
            // —— bridge/05 的实机只跑过 stop/idle。这里对 move 自己从原文里取一次，别再指望它。
            if (m.target == null && !string.IsNullOrEmpty(json) && (m.action == "move" || m.action == "face"))
            {
                Vec2 parsed;
                if (TryReadTarget(json, out parsed))
                {
                    m.target = parsed;
                }
            }

            if (m.action == "path" && !string.IsNullOrEmpty(json))
            {
                long seq;
                if (TryReadSeq(json, out seq))
                {
                    m.seq = seq;
                }
            }

            if ((m.waypoints == null || m.waypoints.Length == 0) && !string.IsNullOrEmpty(json) && m.action == "path")
            {
                Vec2[] wps;
                if (TryReadWaypoints(json, out wps))
                {
                    m.waypoints = wps;
                }
            }

            var handler = ActionHandler;
            if (handler == null)
            {
                _log.LogWarning("link: 收到 action " + m.action + "（厨师位 " + m.chef_id + "），但还没有动作执行器");
                if (_link != null)
                {
                    _link.Send(Msg.Ack(m.action, m.chef_id, false, "动作执行器未接入（任务 04）",
                                       ActionSeq(m.action, m.seq)));
                }
                return;
            }

            string error;
            try
            {
                error = handler(m);
            }
            catch (Exception e)
            {
                error = "异常: " + e.Message;
            }

            if (_link != null)
            {
                _link.Send(Msg.Ack(m.action, m.chef_id, error == null, error ?? "ok",
                                   ActionSeq(m.action, m.seq)));
            }
            _log.LogInfo("ack: action=" + m.action + " chef=" + m.chef_id
                         + (m.action == "path" ? " seq=" + m.seq : "")
                         + " ok=" + (error == null) + " detail=" + (error ?? "ok"));
        }

        /// <summary>`seq` 只对 path 有意义；其它动作的 ack 保持旧形状（不带 seq）。</summary>
        private static long ActionSeq(string action, long seq)
        {
            return action == "path" ? seq : -1;
        }

        /// <summary>
        /// 从原文里取 `"target":{"x":…,"y":…}`。只在 JsonUtility 没填出来时用，
        /// 所以够用就行：找到 target 键，再找它后面第一个 x 与 y 的数字。
        /// </summary>
        private static bool TryReadTarget(string json, out Vec2 target)
        {
            target = null;
            int at = json.IndexOf("\"target\"", StringComparison.Ordinal);
            if (at < 0)
            {
                return false;
            }

            float x, y;
            if (!TryReadNumber(json, "\"x\"", at, out x) || !TryReadNumber(json, "\"y\"", at, out y))
            {
                return false;
            }

            target = new Vec2();
            target.x = x;
            target.y = y;
            return true;
        }

        private static bool TryReadNumber(string json, string key, int from, out float value)
        {
            value = 0f;
            int at = json.IndexOf(key, from, StringComparison.Ordinal);
            if (at < 0)
            {
                return false;
            }

            int colon = json.IndexOf(':', at + key.Length);
            if (colon < 0)
            {
                return false;
            }

            int start = colon + 1;
            while (start < json.Length && (json[start] == ' ' || json[start] == '\t'))
            {
                start++;
            }

            int end = start;
            while (end < json.Length
                   && (char.IsDigit(json[end]) || json[end] == '-' || json[end] == '+' || json[end] == '.'
                       || json[end] == 'e' || json[end] == 'E'))
            {
                end++;
            }

            if (end == start)
            {
                return false;
            }

            return float.TryParse(json.Substring(start, end - start),
                                  System.Globalization.NumberStyles.Float,
                                  System.Globalization.CultureInfo.InvariantCulture, out value);
        }

        private static bool TryReadWaypoints(string json, out Vec2[] waypoints)
        {
            waypoints = null;
            int at = json.IndexOf("\"waypoints\"", StringComparison.Ordinal);
            if (at < 0) return false;
            int open = json.IndexOf('[', at);
            if (open < 0) return false;
            int close = json.IndexOf(']', open);
            if (close < 0) return false;

            var list = new List<Vec2>();
            int cur = open;
            while (cur < close && list.Count < 8)
            {
                int objStart = json.IndexOf('{', cur);
                if (objStart < 0 || objStart >= close) break;
                int objEnd = json.IndexOf('}', objStart);
                if (objEnd < 0 || objEnd > close) break;

                float x, y;
                if (TryReadNumber(json, "\"x\"", objStart, out x) && TryReadNumber(json, "\"y\"", objStart, out y))
                {
                    Vec2 v = new Vec2();
                    v.x = x;
                    v.y = y;
                    list.Add(v);
                }
                cur = objEnd + 1;
            }

            if (list.Count == 0) return false;
            waypoints = list.ToArray();
            return true;
        }

        private static bool TryReadSeq(string json, out long seq)
        {
            seq = 0L;
            int at = json.IndexOf("\"seq\"", StringComparison.Ordinal);
            if (at < 0)
            {
                return false;
            }
            int colon = json.IndexOf(':', at + 5);
            if (colon < 0)
            {
                return false;
            }
            int start = colon + 1;
            while (start < json.Length && (json[start] == ' ' || json[start] == '\t'))
            {
                start++;
            }
            int end = start;
            if (end < json.Length && json[end] == '-')
            {
                end++;
            }
            while (end < json.Length && char.IsDigit(json[end]))
            {
                end++;
            }
            if (end == start || (end == start + 1 && json[start] == '-'))
            {
                return false;
            }
            return long.TryParse(json.Substring(start, end - start),
                                 System.Globalization.NumberStyles.Integer,
                                 System.Globalization.CultureInfo.InvariantCulture, out seq);
        }

        /// <summary>
        /// 回一条动作的结局（执行器在到达/超时/失败时调）。第一次 ack 是"已登记"，
        /// 这一次是"结局"—— 上层据此不必靠猜（见 `ActionDriver` 的说明）。
        /// </summary>
        public static void ReportAction(string action, int chefId, bool ok, string detail,
                                        long seq = -1)
        {
            if (_link != null)
            {
                _link.Send(Msg.Ack(action, chefId, ok, detail, seq));
            }
            _log.LogInfo("ack: action=" + action + " chef=" + chefId
                         + (seq >= 0 ? " seq=" + seq : "")
                         + " ok=" + ok + " detail=" + detail);
        }

        private static void LogLine(string message)
        {
            _log.LogInfo(message);
        }
    }
}
