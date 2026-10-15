using System;
using BepInEx;
using BepInEx.Configuration;
using HarmonyLib;
using UnityEngine;
using WellCooked.Game;
using WellCooked.Net;
using WellCooked.Protocol;

namespace WellCooked
{
    /// <summary>
    /// 插件入口（BepInEx 5，Windows 主机侧）：
    /// - 控制注入：Harmony patch PlayerInputLookup 输入层（InputInjector）
    /// - 状态外传：按间隔读取场景并上报 state_update（StateReader）
    /// - 链路：TCP 行分帧，一行一条 JSON（TcpLink）；字节收发放后台线程，
    ///   入站 JSON 全部在主线程 Update 里用 JsonUtility 解析（JsonUtility 线程受限）。
    /// </summary>
    [BepInPlugin(PluginGuid, PluginName, PluginVersion)]
    public sealed class WellCookedPlugin : BaseUnityPlugin
    {
        public const string PluginGuid = "com.wellcooked.plugin";
        public const string PluginName = "WellCooked";
        public const string PluginVersion = "0.1.0";

        private static readonly string[] HostActions =
        {
            "move", "stop", "press", "release", "tap", "pickup", "dash", "interact"
        };

        // ---- 配置 ----
        private ConfigEntry<string> _netMode;
        private ConfigEntry<string> _netHost;
        private ConfigEntry<int> _netPort;
        private ConfigEntry<int> _netServerPort;
        private ConfigEntry<string> _netListenAddress;
        private ConfigEntry<int> _netHeartbeatMs;
        private ConfigEntry<int> _netHeartbeatTimeoutMs;
        private ConfigEntry<int> _netReconnectDelayMs;

        private ConfigEntry<bool> _stateEnabled;
        private ConfigEntry<int> _stateIntervalMsConfig;
        private ConfigEntry<bool> _stateDumpToLog;

        private ConfigEntry<string> _inputMode;
        private ConfigEntry<string> _inputAxisBasis;
        private ConfigEntry<bool> _inputInvertX;
        private ConfigEntry<bool> _inputInvertY;
        private ConfigEntry<float> _inputArriveThreshold;
        private ConfigEntry<int> _inputMoveTimeoutMs;

        // ---- 运行时组件 ----
        private Harmony _harmony;
        private TcpLink _link;
        private readonly InputInjector _input = new InputInjector();
        private readonly StateReader _state = new StateReader();

        private float _nextStateAt;
        private int _stateIntervalMs = 0;
        private bool _statePushEnabled = true;
        private bool _wasConnected;
        private bool _stateErrorLogged;

        private void Awake()
        {
            BindConfig();

            _harmony = new Harmony(PluginGuid);
            _input.Log = LogLine;
            _state.Log = LogLine;
            _input.Init(_harmony);
            _state.Init(_harmony);

            ApplyInputConfig();
            _statePushEnabled = _stateEnabled.Value;
            _stateIntervalMs = ClampInterval(_stateIntervalMsConfig.Value);

            StartLink();
            LogLine("[Plugin] WellCooked " + PluginVersion + " loaded (game=" + Application.productName + ")");
        }

        private void OnDestroy()
        {
            if (_link != null)
            {
                _link.Stop();
                _link = null;
            }
            _input.ClearAll();
            if (_harmony != null)
            {
                try { _harmony.UnpatchSelf(); } catch { }
                _harmony = null;
            }
            LogLine("[Plugin] unloaded");
        }

        private void Update()
        {
            PumpInbound();

            _input.Tick();

            bool connected = _link != null && _link.Connected;
            if (connected && !_wasConnected)
            {
                _link.Send(Msg.Hello("windows-host", PluginVersion, HostActions));
                _nextStateAt = 0f; // 连接建立后立即上报一次状态
                LogLine("[Plugin] host connected; hello sent");
            }
            if (!connected && _wasConnected)
            {
                // 断线立刻释放全部注入状态，避免角色方向卡死
                _input.ClearAll();
            }
            _wasConnected = connected;

            if (connected && _statePushEnabled)
            {
                float now = Time.realtimeSinceStartup;
                /* 0 = 每帧：这时 _nextStateAt 永远追不上 now，等价于不设闸门。 */
                if (_stateIntervalMs <= 0 || now >= _nextStateAt)
                {
                    _nextStateAt = now + _stateIntervalMs / 1000f;
                    SendState();
                }
            }
        }

        // ================= 入站 =================

        private void PumpInbound()
        {
            if (_link == null) return;
            string json;
            int budget = 32; // 每帧上限，避免大流量卡帧
            while (budget-- > 0 && _link.TryDequeueInbound(out json))
            {
                HandleInbound(json);
            }
        }

        private void HandleInbound(string json)
        {
            InboundMessage m;
            try
            {
                m = JsonUtility.FromJson<InboundMessage>(json);
            }
            catch (Exception e)
            {
                LogLine("[Plugin] malformed inbound message dropped: " + e.Message);
                return;
            }
            if (m == null || string.IsNullOrEmpty(m.type)) return;

            switch (m.type)
            {
                case MsgType.Heartbeat:
                    break; // 链路层保活，不回包
                case MsgType.Hello:
                    break; // 对端握手，仅记录
                case MsgType.Echo:
                    _link.Send(Msg.EchoReply(m.token));
                    break;
                case MsgType.Action:
                    HandleAction(m);
                    break;
                case MsgType.Config:
                    HandleConfig(m);
                    break;
                case MsgType.StateRequest:
                    SendState();
                    break;
                default:
                    LogLine("[Plugin] unknown message type: " + m.type);
                    break;
            }
        }

        private void HandleAction(InboundMessage m)
        {
            bool ok;
            string detail;
            if (!_input.InjectEnabled)
            {
                ok = false;
                detail = "injection disabled (input_mode=off)";
            }
            else
            {
                try
                {
                    ok = _input.TryHandleAction(m, out detail);
                }
                catch (Exception e)
                {
                    ok = false;
                    detail = "exception: " + e.Message;
                }
            }

            _link.Send(Msg.Ack(m.action, m.chef_id, ok, detail));
            if (!ok)
                LogLine("[Plugin] action rejected: " + m.action + " chef=" + m.chef_id + " (" + detail + ")");
        }

        private void HandleConfig(InboundMessage m)
        {
            if (!string.IsNullOrEmpty(m.input_mode))
            {
                bool inject = !string.Equals(m.input_mode, "off", StringComparison.OrdinalIgnoreCase);
                _input.SetInjectEnabled(inject);
                LogLine("[Plugin] config input_mode=" + m.input_mode);
            }

            if (m.state_interval_ms > 0)
            {
                _stateIntervalMs = ClampInterval(m.state_interval_ms);
                LogLine("[Plugin] config state_interval_ms=" + _stateIntervalMs);
            }

            if (!string.IsNullOrEmpty(m.strategy))
            {
                // 调度策略由 OS 核心侧执行，插件只做执行器，不解释 strategy
                LogLine("[Plugin] config strategy=" + m.strategy + " (informational)");
            }
        }

        // ================= 出站 =================

        private void SendState()
        {
            if (_link == null || !_link.Connected) return;
            try
            {
                string json = _state.BuildStateJson();
                _link.Send(json);
                if (_stateDumpToLog.Value) LogLine("[State] " + json);
                _stateErrorLogged = false;
            }
            catch (Exception e)
            {
                if (!_stateErrorLogged)
                {
                    _stateErrorLogged = true;
                    LogLine("[Plugin] state build failed: " + e);
                }
            }
        }

        // ================= 装配 =================

        private void BindConfig()
        {
            _netMode = Config.Bind("Network", "Mode", "client",
                "client=主动连接 OS 核心（WSL2 侧）；server=监听等待对端接入");
            _netHost = Config.Bind("Network", "Host", "127.0.0.1",
                "client 模式目标地址（WSL2 的 localhost 转发）");
            _netPort = Config.Bind("Network", "Port", 12345,
                "client 模式目标端口（Bridge 默认监听在这里）");
            _netServerPort = Config.Bind("Network", "ServerPort", 14455,
                "server 模式监听端口（Bridge 用 --link connect 时默认拨这里，见文档 §6.2.4）");
            _netListenAddress = Config.Bind("Network", "ListenAddress", "127.0.0.1",
                "server 模式监听地址（0.0.0.0 表示全部网卡）");
            _netHeartbeatMs = Config.Bind("Network", "HeartbeatMs", 2000,
                "链路层 heartbeat 发送间隔毫秒（0=禁用）");
            _netHeartbeatTimeoutMs = Config.Bind("Network", "HeartbeatTimeoutMs", 0,
                "超过该毫秒数未收到任何消息则断开重连（0=禁用）");
            _netReconnectDelayMs = Config.Bind("Network", "ReconnectDelayMs", 2000,
                "client 断线重连间隔毫秒（game-bridge spec：连接失败 2 秒后重试）");

            _stateEnabled = Config.Bind("State", "Enabled", true,
                "是否主动上报 state_update");
            _stateIntervalMsConfig = Config.Bind("State", "IntervalMs", 0,
                "state_update 上报间隔毫秒；0 = 每帧上报（默认，game-bridge spec 要求每帧），" +
                "正数则钳到 50~60000 用于限流");
            _stateDumpToLog = Config.Bind("State", "DumpToLog", false,
                "把 state_update 原文写入 BepInEx 日志（联调期可用）");

            _inputMode = Config.Bind("Input", "Mode", "inject",
                "inject=启用控制注入；off=只读观察（拒绝 action）");
            _inputAxisBasis = Config.Bind("Input", "AxisBasis", "camera",
                "move 向量基准：camera=相对当前相机；world=固定世界轴（联调校准用）");
            _inputInvertX = Config.Bind("Input", "InvertX", false, "反转 move 的 X 分量");
            _inputInvertY = Config.Bind("Input", "InvertY", false, "反转 move 的 Y 分量");
            _inputArriveThreshold = Config.Bind("Input", "ArriveThreshold", 0.2f,
                "move 到达判定距离阈值（世界单位）");
            _inputMoveTimeoutMs = Config.Bind("Input", "MoveTimeoutMs", 15000,
                "move 导航超时毫秒（超时自动停车）");
        }

        private void ApplyInputConfig()
        {
            bool cameraBasis = !string.Equals(_inputAxisBasis.Value, "world", StringComparison.OrdinalIgnoreCase);
            _input.Configure(cameraBasis, _inputInvertX.Value, _inputInvertY.Value,
                _inputArriveThreshold.Value, _inputMoveTimeoutMs.Value / 1000f);

            bool inject = !string.Equals(_inputMode.Value, "off", StringComparison.OrdinalIgnoreCase);
            _input.SetInjectEnabled(inject);
        }

        private void StartLink()
        {
            TcpLink.Mode mode = string.Equals(_netMode.Value, "server", StringComparison.OrdinalIgnoreCase)
                ? TcpLink.Mode.Server
                : TcpLink.Mode.Client;

            /* 两种模式各有自己的默认端口：client 连 Bridge 监听的 12345，server 监听文档
             * §6.2.4 说的 14455 —— Bridge 的 --link connect 默认就拨这里。共用一个端口会让
             * "相反拓扑"在默认配置下连不上，而那正是文档承诺能直接跑通的形状。 */
            int port = mode == TcpLink.Mode.Server ? _netServerPort.Value : _netPort.Value;

            _link = new TcpLink(
                mode,
                _netHost.Value,
                port,
                _netListenAddress.Value,
                _netHeartbeatMs.Value,
                _netHeartbeatTimeoutMs.Value,
                _netReconnectDelayMs.Value);
            _link.Log = LogLine;
            _link.Start();
        }

        private static int ClampInterval(int ms)
        {
            if (ms <= 0) return 0;   /* 0 保留原意：每帧上报。限流才会钳到下限。 */
            if (ms < 50) return 50;
            if (ms > 60000) return 60000;
            return ms;
        }

        private void LogLine(string message)
        {
            try { Logger.LogInfo(message); } catch { }
        }
    }
}
