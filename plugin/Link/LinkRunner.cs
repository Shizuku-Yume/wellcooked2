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

        /// <summary>协议 §5 的单条消息上限。</summary>
        private const int MaxStateBytes = 4096;

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
            "move", "stop", "press", "release", "tap", "pickup", "dash", "interact"
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
                _link.Send(Msg.Hello(Instance, "0.1.0", HostActions));
                _nextStateAt = 0f; // 连上先立刻报一帧，别等下一个间隔
                _log.LogInfo("link: 已连上 Bridge，" + _host + ":" + _port + "，hello 已发");
            }
            else if (!connected && _wasConnected)
            {
                _log.LogInfo("link: 与 Bridge 断开，后台每 2 秒重试");
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
                    // 协议约定单条 <4KB（`protocol/protocol.md` §5）。超了照样发（对端按
                    // 容量截断，丢一条会让帧屏障少一帧），但要说出来，别让人以为没这回事。
                    if (!_oversizeLogged)
                    {
                        _oversizeLogged = true;
                        _log.LogWarning("link: state_update " + bytes + " 字节，超过 4 KB 约定（已发）");
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
                    HandleAction(m);
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

        private static void HandleAction(InboundMessage m)
        {
            var handler = ActionHandler;
            if (handler == null)
            {
                _log.LogWarning("link: 收到 action " + m.action + "（厨师位 " + m.chef_id + "），但还没有动作执行器");
                _link.Send(Msg.Ack(m.action, m.chef_id, false, "动作执行器未接入（任务 04）"));
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

            _link.Send(Msg.Ack(m.action, m.chef_id, error == null, error ?? "ok"));
        }

        private static void LogLine(string message)
        {
            _log.LogInfo(message);
        }
    }
}
