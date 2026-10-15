/*
 * PluginPeer.cs -- KitchenOS 桥的跨实现一致性对端。
 *
 * 它要解决的问题：tools/fake_plugin.py 是对线格式的**重新实现**，
 * 所以只要两套实现错得一样，它就会和 C 桥一致。
 * 能证伪这一点的只有插件自己的代码。于是本程序与定义契约的
 * 那两个插件源文件一起编译，但从不修改它们：
 * 
 *
 *   plugin/WellCooked.Plugin/Protocol/Messages.cs   JsonBuilder, Msg, MsgType, InboundMessage
 *   plugin/WellCooked.Plugin/Net/TcpLink.cs         4 字节大端分帧 + TCP 链路
 *
 * 它发到线上的每个字节都由插件自己的构造器产生
 * （Msg.Hello / Msg.Ack / Msg.Heartbeat，state_update 用 JsonBuilder 按
 * StateReader.cs 发出的精确形状搭建）。它读回来的每一样东西都经反射绑定到
 * 插件自己的 InboundMessage 字段：Messages.cs 里改了字段名或类型，
 * 这个对端接受的形状就跟着变，不需要改本文件。
 *
 * 它做不到什么，以及为什么不装作做到了：
 *
 *   Plugin.cs 用 UnityEngine.JsonUtility 解析入站 JSON，那需要 Unity 引擎，
 *   无法链接进控制台程序。所以本对端自己解析 JSON 语法，
 *   并通过反射对 InboundMessage 强制插件的**字段契约**：
 *   未知的字段名、
 *   JSON 类型不对、插件的 C# 类型装不下的值，都算失败；
 *   而「JSON 本身解析正确、只有 JsonUtility 才会犯的怪癖」
 *   不在本对端的覆盖范围内。
 *
 * 分两个阶段，因为它们对 socket 的要求不同：
 *
 *   阶段 1「分帧探针」——裸 socket。TcpLink 独占自己的 socket，
 *     每次 Socket.Send 都交出一整帧，所以它造不出 C 解码器必须扛住的两种情况：
 *     一帧拆成两次写、两帧并在一次写里。
 *     这些帧打向**桥**，而桥一旦分帧失步就会断开链路，
 *     所以「扛住了」这件事是能观察到的。
 *
 *   阶段 2「会话」——插件自己的 TcpLink 跑客户端模式，用插件的
 *     默认配置（Network.Mode=client、HeartbeatMs=2000、
 *     HeartbeatTimeoutMs=0、ReconnectDelayMs=3000）：先 hello，再以
 *     60 Hz 发 state_update，并把桥回的每条 action 解码、回 ack。
 *
 * 退出码：0 全部检查通过，1 契约分歧或前置条件不满足，
 * 2 参数错误。
 */

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Net.Sockets;
using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using WellCooked.Net;
using WellCooked.Protocol;

namespace WellCooked.Conformance
{
    internal static class Program
    {
        private static int Main(string[] argv)
        {
            /* 本对端打印的证据既给人看也给 grep 看，所以把编码钉死，
             * 而不是继承管道/控制台碰巧用的那个代码页
             * （下面的 echo token 里故意放了一个非 ASCII
             * 字符）。 */
            try { Console.OutputEncoding = new UTF8Encoding(false); }
            catch (Exception) { }

            Options opt;
            string argError;
            if (!Options.TryParse(argv, out opt, out argError))
            {
                Console.Error.WriteLine("PluginPeer: " + argError);
                Console.Error.WriteLine(Options.Usage);
                return 2;
            }

            var peer = new Peer(opt);
            return peer.Run();
        }
    }

    internal sealed class Options
    {
        public string Host = "127.0.0.1";
        public int Port = 12345;              /* the plugin's Network.Port default */
        public double Seconds = 3.0;
        public int Hz = 60;
        public bool RawPhase = true;
        public string[] RequireKinds = new string[] { "move", "stop" };
        public string SourceRoot;

        public const string Usage =
            "用法: PluginPeer [--host H] [--port P] [--seconds S] [--hz H]\n" +
            "                  [--require-kinds move,stop] [--no-raw-phase]\n" +
            "                  [--source-root DIR]\n" +
            "\n" +
            "对着一个正在跑的 kitchenos-bridge，扮演插件那一侧的线格式。\n" +
            "--require-kinds 指定必须观察到的 action 种类：桥自己会回空转的\n" +
            "\"stop\"，而 \"move\" 需要有一个 Chef Worker（或者\n" +
            "core/build/kitchenos-worker-stub）先往 slot 0 里发布。\n";

        public static bool TryParse(string[] argv, out Options opt, out string error)
        {
            opt = new Options();
            error = null;

            for (int i = 0; i < argv.Length; i++)
            {
                string a = argv[i];
                string value = null;
                int eq = a.IndexOf('=');
                if (a.StartsWith("--", StringComparison.Ordinal) && eq > 0)
                {
                    value = a.Substring(eq + 1);
                    a = a.Substring(0, eq);
                }

                switch (a)
                {
                    case "--host":
                        if (!Take(argv, ref i, ref value, ref error)) return false;
                        opt.Host = value;
                        break;
                    case "--port":
                        if (!Take(argv, ref i, ref value, ref error)) return false;
                        {
                            int port;
                            if (!int.TryParse(value, NumberStyles.Integer, CultureInfo.InvariantCulture, out port) ||
                                port < 1 || port > 65535)
                            {
                                error = "--port wants an integer in 1..65535";
                                return false;
                            }
                            opt.Port = port;
                        }
                        break;
                    case "--seconds":
                        if (!Take(argv, ref i, ref value, ref error)) return false;
                        {
                            double seconds;
                            if (!double.TryParse(value, NumberStyles.Float, CultureInfo.InvariantCulture, out seconds) ||
                                seconds <= 0.0 || seconds > 600.0)
                            {
                                error = "--seconds wants a number in (0, 600]";
                                return false;
                            }
                            opt.Seconds = seconds;
                        }
                        break;
                    case "--hz":
                        if (!Take(argv, ref i, ref value, ref error)) return false;
                        {
                            int hz;
                            if (!int.TryParse(value, NumberStyles.Integer, CultureInfo.InvariantCulture, out hz) ||
                                hz < 1 || hz > 1000)
                            {
                                error = "--hz wants an integer in 1..1000";
                                return false;
                            }
                            opt.Hz = hz;
                        }
                        break;
                    case "--require-kinds":
                        if (!Take(argv, ref i, ref value, ref error)) return false;
                        opt.RequireKinds = value.Split(',');
                        for (int k = 0; k < opt.RequireKinds.Length; k++)
                            opt.RequireKinds[k] = opt.RequireKinds[k].Trim().ToLowerInvariant();
                        break;
                    case "--no-raw-phase":
                        if (value != null) { error = "--no-raw-phase takes no value"; return false; }
                        opt.RawPhase = false;
                        break;
                    case "--source-root":
                        if (!Take(argv, ref i, ref value, ref error)) return false;
                        opt.SourceRoot = value;
                        break;
                    default:
                        error = "unknown argument '" + argv[i] + "'";
                        return false;
                }
            }
            return true;
        }

        private static bool Take(string[] argv, ref int i, ref string value, ref string error)
        {
            if (value != null) return true;              /* --key=value already split */
            if (i + 1 >= argv.Length)
            {
                error = argv[i] + " needs a value";
                return false;
            }
            value = argv[++i];
            return true;
        }
    }

    /* ==================================================================== */
    /* 对端                                                                 */
    /* ==================================================================== */

    internal sealed class Peer
    {
        /* 帧标记。state_update 带 game_time；挑一些真游戏不可能产生的值，
         * 就能把桥日志里的行钉到写出去的那一帧上——
         * 拆分写和粘包写是否被正确重组，靠的就是这个，
         * 而不是「没崩就算过」。
         * run-conformance.ps1 断言的就是同一组数。 */
        private const long SplitMarkerGameTime = 4242;
        private const long CoalescedMarkerGameTime = 4243;
        private const long CapProbeMarkerGameTime = 4244;

        /* C 侧 PROTO_KEY_MAX 是 32；过长的键必须被截断并计数，
         * 而不是被拒绝。41 个字符，小写，形状合 schema。 */
        private const string LongResourceKey = "resource_key_longer_than_the_c_side_field";
        private const int CapProbeResources = 50;    /* PROTO_MAX_RESOURCES is 48: expect dropped=2 */
        private const int CapProbeDropped = CapProbeResources - 48;
        private const int CapProbeTruncated = 1;

        /* 插件自己的握手取值，抄自 Plugin.cs：桥必须
         * 看到和真游戏一模一样的 hello。 */
        private const string PluginVersion = "0.1.0";
        private static readonly string[] HostActions =
        {
            "move", "stop", "press", "release", "tap", "pickup", "dash", "interact"
        };

        private readonly Options _opt;
        private readonly List<string> _failures = new List<string>();
        private readonly Dictionary<string, int> _failureCounts = new Dictionary<string, int>(StringComparer.Ordinal);
        private readonly Dictionary<string, int> _rxTypes = new Dictionary<string, int>(StringComparer.Ordinal);
        private readonly Dictionary<string, int> _actionKinds = new Dictionary<string, int>(StringComparer.Ordinal);
        private readonly List<string> _knownChefs = new List<string>();
        private readonly Dictionary<string, FieldInfo> _inboundFields = new Dictionary<string, FieldInfo>(StringComparer.Ordinal);
        private readonly Dictionary<string, string> _msgTypeConstants = new Dictionary<string, string>(StringComparer.Ordinal);

        private int _actionsSeen;
        private int _actionsFailed;
        private int _acksSent;
        private int _malformedFrames;
        private TcpLink _ackLink;   /* set only while phase 2 owns a live TcpLink */
        private double _moveXMin = double.MaxValue;
        private double _moveXMax = double.MinValue;
        private double _moveYMin = double.MaxValue;
        private double _moveYMax = double.MinValue;
        private bool _sawMoveTarget;

        public Peer(Options opt)
        {
            _opt = opt;
            FieldInfo[] fields = typeof(InboundMessage).GetFields(BindingFlags.Public | BindingFlags.Instance);
            for (int i = 0; i < fields.Length; i++) _inboundFields[fields[i].Name] = fields[i];

            FieldInfo[] consts = typeof(MsgType).GetFields(BindingFlags.Public | BindingFlags.Static);
            for (int i = 0; i < consts.Length; i++)
            {
                if (!consts[i].IsLiteral) continue;
                string v = consts[i].GetRawConstantValue() as string;
                if (v != null) _msgTypeConstants[v] = consts[i].Name;
            }

            /* 本对端在 state_update 里上报的厨师，就是
             * 插件 InputInjector 唯一会接受的 action 目标（否则 TryHandleAction 会报
             * "chef_id out of range"），所以入站 action 就是拿这个集合校验的。
             * */
            for (int i = 0; i < StateFrames.ChefIds.Length; i++)
                _knownChefs.Add(StateFrames.ChefIds[i].ToString(CultureInfo.InvariantCulture));
        }

        /* ================================================================ */

        public int Run()
        {
            Banner();

            if (_opt.RawPhase) RunFramingProbes();
            RunLinkSession();

            return Summary();
        }

        private void Banner()
        {
            Say("=== KitchenOS 一致性对端（插件自己的协议代码）===");
            Say("对端       : {0}", Environment.Version + " / " + Path.GetFileName(CurrentExecutable()));
            Say("编译来源   : Protocol/Messages.cs + Net/TcpLink.cs，未改动");
            string root = _opt.SourceRoot;
            if (root == null)
            {
                string dir = Path.GetDirectoryName(CurrentExecutable());
                root = Path.GetFullPath(Path.Combine(dir, Path.Combine("..", Path.Combine("..", Path.Combine("..", "..")))));
            }
            Say("源码根目录 : {0}", root);
            SaySource(root, Path.Combine("plugin", Path.Combine("WellCooked.Plugin", Path.Combine("Protocol", "Messages.cs"))));
            SaySource(root, Path.Combine("plugin", Path.Combine("WellCooked.Plugin", Path.Combine("Net", "TcpLink.cs"))));
            Say("目标       : {0}:{1}  （TcpLink.Mode.Client，插件默认 Network.Mode=client）", _opt.Host, _opt.Port);
            Say("本对端能构造的消息 : {0}", Join(new string[] { MsgType.Hello, MsgType.StateUpdate, MsgType.Heartbeat, MsgType.Ack, MsgType.Echo }));
            Say("本对端接受的消息   : {0}", Join(MsgTypeNames()));
            Say("");
        }

        private static string CurrentExecutable()
        {
            return Assembly.GetExecutingAssembly().Location;
        }

        private void SaySource(string root, string relative)
        {
            string path = Path.Combine(root, relative);
            try
            {
                using (FileStream fs = File.OpenRead(path))
                using (SHA256 sha = SHA256.Create())
                {
                    byte[] digest = sha.ComputeHash(fs);
                    var sb = new StringBuilder(digest.Length * 2);
                    for (int i = 0; i < digest.Length; i++) sb.Append(digest[i].ToString("x2", CultureInfo.InvariantCulture));
                    Say("  {0}  sha256={1}", relative.Replace('\\', '/'), sb.ToString());
                    return;
                }
            }
            catch (Exception e)
            {
                Say("  {0}  sha256=不可用 ({1})", relative.Replace('\\', '/'), e.Message);
            }
        }

        /* ============================= 阶段 1 ============================== */

        private void RunFramingProbes()
        {
            Say("--- 阶段 1：C 解码器分帧探针（裸 socket 写入）---");
            Say("  真插件是被 TCP 分段与合段带到这些状态的；");
            Say("  探针把它们变成确定性的。");
            Say("");

            ProbeSocket sock = null;
            try
            {
                sock = new ProbeSocket(_opt.Host, _opt.Port);
                Say("  已连接（裸 socket）。故意跳过握手：这里不发 hello。");
                Say("");

                /* （A）一帧两次写。桥必须先把前半段缓存住，
                 * 等长度前缀说明整帧到齐了才解析。 */
                string splitJson = StateFrames.BuildAtGameTime(SplitMarkerGameTime);
                byte[] splitFrame = Framer.Encode(splitJson);
                int firstHalf = 7;
                Say("  [split]  state_update game_time={0} 载荷 {1} 字节，整帧 {2} 字节",
                    SplitMarkerGameTime, Encoding.UTF8.GetByteCount(splitJson), splitFrame.Length);
                Say("           长度前缀 : {0}", Hex(splitFrame, 0, 4));
                Say("           第 1/2 次写: {0} 字节  {1}", firstHalf, Hex(splitFrame, 0, firstHalf));
                sock.Write(splitFrame, 0, firstHalf);
                Thread.Sleep(150);
                Say("           第 2/2 次写: {0} 字节  {1} ... （{2} ms 之后）",
                    splitFrame.Length - firstHalf, Hex(splitFrame, firstHalf, 8), 150);
                sock.Write(splitFrame, firstHalf, splitFrame.Length - firstHalf);
                Say("           桥日志应有: game_time={0} 被解析，且没有 parse_error", SplitMarkerGameTime);
                Say("");

                /* （B）一整帧两帧塞进一次写。 */
                string helloJson = Msg.Hello("windows-host", PluginVersion, HostActions);
                string coalescedJson = StateFrames.BuildAtGameTime(CoalescedMarkerGameTime);
                byte[] helloFrame = Framer.Encode(helloJson);
                byte[] coalescedFrame = Framer.Encode(coalescedJson);
                var both = new byte[helloFrame.Length + coalescedFrame.Length];
                Buffer.BlockCopy(helloFrame, 0, both, 0, helloFrame.Length);
                Buffer.BlockCopy(coalescedFrame, 0, both, helloFrame.Length, coalescedFrame.Length);
                Say("  [coalesce] hello（{0} B）+ state_update game_time={1}（{2} B）一次写入 {3} B",
                    helloFrame.Length, CoalescedMarkerGameTime, coalescedFrame.Length, both.Length);
                sock.Write(both, 0, both.Length);
                Say("           桥日志应有: 一次读里既有 plugin_hello 又有 game_time={0}", CoalescedMarkerGameTime);
                Say("");

                /* （C）一条心跳，和 TcpLink.PumpLoop 发的完全一样：
                 * 桥只凭心跳判断插件是否活着。 */
                string hbJson = Msg.Heartbeat(UnixNow());
                byte[] hbFrame = Framer.Encode(hbJson);
                sock.Write(hbFrame, 0, hbFrame.Length);
                Say("  [heartbeat] {0}（{1} B）", hbJson, hbFrame.Length);
                Say("");

                /* （D）容量上限。不是插件会产生的输出；它让桥的
                 * dropped=/truncated= 计数可以被证伪，而不是永远 0。 */
                string capJson = StateFrames.BuildCapProbe(LongResourceKey, CapProbeResources, CapProbeMarkerGameTime);
                byte[] capFrame = Framer.Encode(capJson);
                sock.Write(capFrame, 0, capFrame.Length);
                Say("  [caps]   state_update game_time={0} resources={1}（有一个键长 {2} 个字符）",
                    CapProbeMarkerGameTime, CapProbeResources, LongResourceKey.Length);
                Say("           桥日志应有: game_time={0} resources=48 dropped={1} truncated={2}",
                    CapProbeMarkerGameTime, CapProbeDropped, CapProbeTruncated);
                Say("");

                /* （E）对齐往返：echo 用插件自己的 JsonBuilder 构造，
                 * 桥用同一个 token 回。一个在（A）到（D）上丢了同步的 C
                 * 解码器早就该把链路断掉了；而一个把字符串转义搞坏的桥，
                 * 回过来的 token 也会不一样。
                 * */
                string token = "kos-\"verifier\"-\\1-\u9505";
                string echoJson = new JsonBuilder(160)
                    .Obj().Field("type", MsgType.Echo).Field("token", token).EndObj().ToString();
                byte[] echoFrame = Framer.Encode(echoJson);
                sock.Write(echoFrame, 0, echoFrame.Length);
                Say("  [align]  echo token={0} -> 等待 echo_reply", Quote(token));

                bool aligned = false;
                long deadline = NowMs() + 3000;
                while (NowMs() < deadline)
                {
                    string json;
                    if (!sock.TryReadFrame((int)Math.Max(1, deadline - NowMs()), out json)) break;
                    string type = Decode(json, "phase1");
                    if (type == MsgType.EchoReply)
                    {
                        JsonValue v = null;
                        try { v = Json.Parse(json); }
                        catch (JsonParseException) { }
                        JsonValue back = v == null ? null : v.Member("token");
                        string got = back == null ? null : back.Text;
                        if (got == token)
                        {
                            aligned = true;
                            Say("           echo_reply 的 token 逐字节一致（{0} 字节 UTF-8）", Encoding.UTF8.GetByteCount(token));
                        }
                        else
                        {
                            Fail("阶段1: echo 的 token 被改坏了：发出 {0}，收到 {1}", Quote(token), Quote(got));
                        }
                        break;
                    }
                }
                if (!aligned && _failures.Count == 0)
                {
                    Fail("阶段1: 3000 ms 内没等到 echo_reply；要么对面不是 C 桥，要么链路已经断了");
                }
                Say("           经历拆分帧 + 粘包帧 + 50 键帧之后链路仍然对齐: {0}", aligned ? "是" : "否");
                Say("");

                sock.Quiet(300);
                Say("  阶段 1 结束：写出 {0} 字节，读回 {1} 帧，干净关闭", sock.BytesSent, sock.FramesRead);
            }
            catch (Exception e)
            {
                Fail("阶段1: {0}: {1}", e.GetType().Name, e.Message);
            }
            finally
            {
                if (sock != null) sock.Dispose();
                Say("");
            }
        }

        /* ============================= 阶段 2 ============================== */

        private void RunLinkSession()
        {
            Say("--- 阶段 2：插件 TcpLink 客户端会话（{0} Hz 的 state_update，跑 {1} 秒）---",
                _opt.Hz, _opt.Seconds.ToString(CultureInfo.InvariantCulture));

            /* Plugin.StartLink()：插件的 Network.* 默认配置。 */
            var link = new TcpLink(
                TcpLink.Mode.Client,
                _opt.Host,
                _opt.Port,
                "127.0.0.1",   /* ListenAddress: unused in client mode */
                2000,          /* HeartbeatMs */
                0,             /* HeartbeatTimeoutMs */
                3000);         /* ReconnectDelayMs */
            link.Log = delegate(string message) { Say("  [tcplink] {0}", message); };
            link.Start();

            long connectDeadline = NowMs() + 5000;
            while (!link.Connected && NowMs() < connectDeadline) Thread.Sleep(20);
            if (!link.Connected)
            {
                Fail("阶段2: TcpLink 在 5000 ms 内连不上 {0}:{1}（桥在监听吗？）", _opt.Host, _opt.Port);
                link.Stop();
                Say("");
                return;
            }

            /* Plugin.Update() 在 Connected 翻转后的第一帧发 hello。
             * TcpLink 在置上 _connected 之后立刻清空发送队列，
             * 所以同一瞬间入队的 hello 可能被丢掉；插件靠「晚一帧再反应」
             * 从来没有踩到这个竞态。这里照做。 */
            Thread.Sleep(100);
            link.Send(Msg.Hello("windows-host", PluginVersion, HostActions));
            Say("  [tx] hello {0}", Msg.Hello("windows-host", PluginVersion, HostActions));
            Say("  [tx] 插件自己的心跳每 2000 ms 一次，由 TcpLink.PumpLoop 发出");
            Say("");

            long frames = (long)Math.Round(_opt.Seconds * _opt.Hz);
            var sw = Stopwatch.StartNew();
            long bytesSent = 0;
            long behind = 0;
            _ackLink = link;

            for (long i = 0; i < frames; i++)
            {
                double gameTime = (double)i / _opt.Hz;
                string json = StateFrames.Build(gameTime);
                bytesSent += Encoding.UTF8.GetByteCount(json);
                link.Send(json);
                if (i == 0) Say("  [tx] first state_update ({0} B): {1}", Encoding.UTF8.GetByteCount(json), json);
                else if (i % (long)(_opt.Hz) == 0) Say("  [tx] state_update #{0} ({1} B, game_time={2})", i + 1, Encoding.UTF8.GetByteCount(json), StateFrames.GameTime(gameTime));

                DrainInbound(link);

                double due = (i + 1) * 1000.0 / _opt.Hz;
                int wait = (int)(due - sw.Elapsed.TotalMilliseconds);
                if (wait > 0) Thread.Sleep(wait);
                else if (wait < -250) behind++;
            }

            Say("  [tx] 发出 {0} 个 state_update 帧，{1} 字节，用时 {2} ms（节拍落后超过 250 ms 共 {3} 次）",
                frames, bytesSent, (long)sw.Elapsed.TotalMilliseconds, behind);

            /* 桥每收到一个 state_update 就开一帧，并在 --frame-timeout-ms 内回答；
             * 继续把入站读干净，免得最后几帧的 action 被 close() 带走。
             * */
            long drainUntil = NowMs() + 700;
            while (NowMs() < drainUntil)
            {
                DrainInbound(link);
                Thread.Sleep(10);
            }
            DrainInbound(link);
            _ackLink = null;
            link.Stop();
            Say("  [tcplink] 已停止");
            Say("");
        }

        private void DrainInbound(TcpLink link)
        {
            string json;
            int budget = 512;
            while (budget-- > 0 && link.TryDequeueInbound(out json))
            {
                Decode(json, "phase2");
            }
        }

        /* ============================== 解码 ============================== */

        /// <summary>
        /// 解析一个入站帧，并拿插件自己的 InboundMessage 字段校验它。
        /// 返回消息类型（不可用时返回空串）。
        /// </summary>
        private string Decode(string json, string where)
        {
            JsonValue root;
            try
            {
                root = Json.Parse(json);
            }
            catch (JsonParseException e)
            {
                _malformedFrames++;
                Fail("{0}: 入站帧不是插件的读取器能用的 JSON：{1}\n      原始字节: {2}",
                    where, e.Message, Truncate(json, 400));
                return "";
            }

            string bindError;
            Bound bound = Bind(root, out bindError);
            if (bound == null)
            {
                _malformedFrames++;
                Fail("{0}: 入站帧装不进 InboundMessage：{1}\n      原始字节: {2}", where, bindError, Truncate(json, 400));
                return "";
            }

            string type = bound.Message.type;
            Count(_rxTypes, type == null ? "(null)" : type);

            if (type == null || type.Length == 0)
            {
                Fail("{0}: 入站帧没有 type", where);
                return "";
            }
            if (!_msgTypeConstants.ContainsKey(type))
            {
                Fail("{0}: 入站 type {1} 不在 MsgType 的名字里；Plugin.HandleInbound 会打日志 'unknown message type' 并忽略它",
                    where, Quote(type));
                return type;
            }

            if (type == MsgType.Action) CheckAction(root, bound, where);
            else if (type == MsgType.Heartbeat) CheckHeartbeat(bound, where);
            else if (type == MsgType.EchoReply) { /* phase 1 verifies the token */ }
            else Say("  [rx] {0} (handled, not asserted)", type);

            return type;
        }

        private void CheckHeartbeat(Bound bound, string where)
        {
            if (bound.Message.timestamp <= 0.0)
                Fail("{0}: 心跳带的 timestamp={1}；protocol/README §3.2 要求 Unix 秒",
                    where, bound.Message.timestamp.ToString("R", CultureInfo.InvariantCulture));
        }

        private void CheckAction(JsonValue root, Bound bound, string where)
        {
            InboundMessage m = bound.Message;
            _actionsSeen++;

            string kind = m.action == null ? "" : m.action.Trim().ToLowerInvariant();
            Count(_actionKinds, kind.Length == 0 ? "(empty)" : kind);

            /* 先打印解出来的 action：这是对端自己对 C 侧字节的读法，
             * 读的时候用的是插件 InboundMessage 的字段类型。 */
            Say("  [rx] action #{0} chef_id={1} action={2}{3}",
                _actionsSeen, m.chef_id, Quote(kind),
                m.target == null ? "" : " target=(" + F(m.target.x) + ", " + F(m.target.y) + ")");

            if (kind.Length == 0)
            {
                _actionsFailed++;
                Fail("{0}: action 帧没有 action 名（InputInjector 会说 'missing action'）\n      raw: {1}", where, Truncate(root.Text, 400));
                return;
            }

            bool ok = true;
            if (Array.IndexOf(HostActions, kind) < 0)
            {
                ok = false;
                Fail("{0}: action {1} 不在插件声明支持的 action 里（{2}）；InputInjector 会回 'unknown action'",
                    where, Quote(kind), Join(HostActions));
            }
            if (_knownChefs.IndexOf(m.chef_id.ToString(CultureInfo.InvariantCulture)) < 0)
            {
                ok = false;
                Fail("{0}: action 指向 chef_id={1}，但本对端在 state_update 里只上报过厨师 {2}；插件 InputInjector 会回 'chef_id out of range'",
                    where, m.chef_id, Join(_knownChefs.ToArray()));
            }

            if (kind == "move")
            {
                JsonValue target = root.Member("target");
                if (m.target == null || target == null || target.Kind != JsonKind.Object)
                {
                    ok = false;
                    Fail("{0}: move 没有 target 对象（InputInjector 会说 'missing target'）", where);
                }
                else
                {
                    JsonValue x = target.Member("x");
                    JsonValue y = target.Member("y");
                    if (x == null || x.Kind != JsonKind.Number || y == null || y.Kind != JsonKind.Number)
                    {
                        ok = false;
                        Fail("{0}: move 的 target 必须带数值型 x 和 y（protocol/README §3.6）", where);
                    }
                    else if (IsNotFinite(x.Number) || IsNotFinite(y.Number))
                    {
                        ok = false;
                        Fail("{0}: move 的 target 不是有限值：x={1} y={2}", where, x.Number.ToString("R", CultureInfo.InvariantCulture), y.Number.ToString("R", CultureInfo.InvariantCulture));
                    }
                    else
                    {
                        _sawMoveTarget = true;
                        if (x.Number < _moveXMin) _moveXMin = x.Number;
                        if (x.Number > _moveXMax) _moveXMax = x.Number;
                        if (y.Number < _moveYMin) _moveYMin = y.Number;
                        if (y.Number > _moveYMax) _moveYMax = y.Number;
                    }
                }
            }
            else if (kind == "interact" && m.target_object == null)
            {
                /* protocol/README §3.6 说 interact 的 target_object 是必填，
                 * 但 InputInjector 处理 interact 就是点一下按钮，
                 * 从不读 target_object。C 桥总是会带上它，所以这说的是
                 * 插件有多宽容，不是线格式失败。 */
                Say("  [note] interact 帧没带 target_object：文档说它必填，InputInjector 不读它");
            }

            if (!ok) _actionsFailed++;

            /* 插件对每条 action 都回 ack，不管接受与否
             * （Plugin.HandleAction -> _link.Send(Msg.Ack(...))），于是桥的
             * protocol_parse_ack 这条路径也被插件自己的构造器覆盖到了。
             * 阶段 1 没有 TcpLink，故意不回 ack。 */
            if (_ackLink != null)
            {
                _ackLink.Send(Msg.Ack(m.action, m.chef_id, ok, ok ? "conformance-peer" : "rejected by conformance peer"));
                _acksSent++;
            }
        }

        /* ============================== 绑定 ============================== */
        private sealed class Bound
        {
            public InboundMessage Message;
            public readonly List<string> Fields = new List<string>();
        }

        private Bound Bind(JsonValue root, out string error)
        {
            error = null;
            if (root.Kind != JsonKind.Object)
            {
                error = "top level is " + root.KindName() + ", not an object";
                return null;
            }

            var bound = new Bound();
            bound.Message = new InboundMessage();

            for (int i = 0; i < root.Members.Count; i++)
            {
                string name = root.Members[i].Key;
                JsonValue value = root.Members[i].Value;
                FieldInfo f;
                if (!_inboundFields.TryGetValue(name, out f))
                {
                    error = "field " + Quote(name) + " does not exist on WellCooked.Protocol.InboundMessage (JsonUtility would silently ignore it)";
                    return null;
                }
                string fieldError = null;
                if (!Assign(bound.Message, f, value, name, ref fieldError))
                {
                    error = fieldError;
                    return null;
                }
                bound.Fields.Add(name);
            }
            return bound;
        }

        private static bool Assign(object target, FieldInfo f, JsonValue value, string name, ref string error)
        {
            if (value.IsNull) return true;      /* JsonUtility leaves the field at its default */
            Type t = f.FieldType;

            if (t == typeof(string))
            {
                if (value.Kind != JsonKind.String) { error = FieldTypeError(name, "string", value); return false; }
                f.SetValue(target, value.Text);
                return true;
            }
            if (t == typeof(int) || t == typeof(long))
            {
                long iv;
                if (value.Kind != JsonKind.Number || !TryIntegral(value.Number, out iv))
                {
                    error = FieldTypeError(name, t.Name, value) + " (the plugin's field is an integer)";
                    return false;
                }
                if (t == typeof(int))
                {
                    if (iv < int.MinValue || iv > int.MaxValue) { error = "field " + Quote(name) + " is outside Int32"; return false; }
                    f.SetValue(target, (int)iv);
                }
                else f.SetValue(target, iv);
                return true;
            }
            if (t == typeof(double))
            {
                if (value.Kind != JsonKind.Number) { error = FieldTypeError(name, "number", value); return false; }
                f.SetValue(target, value.Number);
                return true;
            }
            if (t == typeof(float))
            {
                if (value.Kind != JsonKind.Number) { error = FieldTypeError(name, "number", value); return false; }
                f.SetValue(target, (float)value.Number);
                return true;
            }
            if (t == typeof(bool))
            {
                if (value.Kind != JsonKind.Bool) { error = FieldTypeError(name, "bool", value); return false; }
                f.SetValue(target, value.Bool);
                return true;
            }
            if (value.Kind == JsonKind.Object && t.IsClass)
            {
                /* 一层嵌套就够覆盖 InboundMessage.target（Vec2），遍历始终由
                 * 插件的类型驱动，而不是由字段名驱动。 */
                object nested;
                try { nested = Activator.CreateInstance(t); }
                catch (Exception e) { error = "cannot instantiate " + t.FullName + " for field " + Quote(name) + ": " + e.Message; return false; }

                for (int i = 0; i < value.Members.Count; i++)
                {
                    string nname = value.Members[i].Key;
                    FieldInfo nf = t.GetField(nname, BindingFlags.Public | BindingFlags.Instance);
                    if (nf == null)
                    {
                        error = "field " + Quote(name + "." + nname) + " does not exist on " + t.FullName + " (JsonUtility would silently ignore it)";
                        return false;
                    }
                    string nestedError = null;
                    if (!Assign(nested, nf, value.Members[i].Value, name + "." + nname, ref nestedError)) { error = nestedError; return false; }
                }
                f.SetValue(target, nested);
                return true;
            }

            error = "field " + Quote(name) + " holds " + value.KindName() + ", which the plugin's C# type " + t.Name + " cannot take";
            return false;
        }

        private static string FieldTypeError(string name, string want, JsonValue got)
        {
            return "field " + Quote(name) + " must be a JSON " + want + ", got " + got.KindName();
        }

        private static bool TryIntegral(double d, out long value)
        {
            value = 0;
            if (IsNotFinite(d)) return false;
            if (d < -9.007199254740992E15 || d > 9.007199254740992E15) return false;
            if (Math.Floor(d) != d) return false;
            value = (long)d;
            return true;
        }

        private static bool IsNotFinite(double d)
        {
            return double.IsNaN(d) || double.IsInfinity(d);
        }

        /* ============================== 判定 ============================== */

        private int Summary()
        {
            Say("--- 汇总 ---");
            Say("  按类型分的入站帧   : {0}", MapText(_rxTypes));
            Say("  解出的 action      : {0}（{1} 项检查未过）", _actionsSeen, _actionsFailed);
            Say("  观察到的 action 种类: {0}", MapText(_actionKinds));
            if (_sawMoveTarget)
                Say("  move target 范围   : x {0}..{1}  y {2}..{3}（worker stub 让 x 在 -4..4 扫动，y = slot）",
                    F(_moveXMin), F(_moveXMax), F(_moveYMin), F(_moveYMax));
            Say("  畸变入站帧         : {0}", _malformedFrames);
            Say("  发出的 ack(Msg.Ack): {0}", _acksSent);

            for (int i = 0; i < _opt.RequireKinds.Length; i++)
            {
                string kind = _opt.RequireKinds[i];
                if (kind.Length == 0) continue;
                int n;
                if (!_actionKinds.TryGetValue(kind, out n) || n == 0)
                {
                    Fail("从未观察到 {0} 类型的 action。桥自己会回空转的 'stop'；'move' 需要有 Chef Worker 往 slot 0 里发布（core/build/kitchenos-worker-stub），两者都要求桥正在派发",
                        Quote(kind));
                }
            }

            Say("");
            if (_failures.Count == 0)
            {
                Say("VERDICT: PASS —— 本对端用插件自己的代码构造的每一帧，");
                Say("         都被 C 桥解析了（见 logs/conformance-bridge.log）；桥回的每一帧，");
                Say("         都是经 InboundMessage 解出来的。");
                return 0;
            }

            Say("VERDICT: FAIL —— {0} 处分歧：", _failures.Count);
            for (int i = 0; i < _failures.Count; i++)
            {
                int n;
                _failureCounts.TryGetValue(_failures[i], out n);
                Say("  {0}. {1}{2}", i + 1, _failures[i], n > 1 ? "  （x" + n.ToString(CultureInfo.InvariantCulture) + " 次）" : "");
            }
            return 1;
        }

        /// <summary>
        /// 记录一处分歧。相同的消息会合并成一条并带出现次数：
        /// 否则 60 Hz 的流量会把结论埋在几百条一模一样的行下面。
        /// 
        /// </summary>
        private void Fail(string format, params object[] args)
        {
            string message = string.Format(CultureInfo.InvariantCulture, format, args);
            int n;
            if (_failureCounts.TryGetValue(message, out n))
            {
                _failureCounts[message] = n + 1;
                return;
            }
            _failureCounts[message] = 1;
            _failures.Add(message);
            Say("  [FAIL] {0}", message);
        }

        /* ============================ 辅助函数 ============================ */

        private string[] MsgTypeNames()
        {
            FieldInfo[] fields = typeof(MsgType).GetFields(BindingFlags.Public | BindingFlags.Static);
            var names = new List<string>();
            for (int i = 0; i < fields.Length; i++)
            {
                if (!fields[i].IsLiteral) continue;
                string v = fields[i].GetRawConstantValue() as string;
                if (v != null) names.Add(v);
            }
            return names.ToArray();
        }

        public static long UnixNow()
        {
            return (long)(DateTime.UtcNow - new DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind.Utc)).TotalSeconds;
        }

        public static long NowMs()
        {
            return (long)(Stopwatch.GetTimestamp() * 1000.0 / Stopwatch.Frequency);
        }

        private static void Count(Dictionary<string, int> map, string key)
        {
            int n;
            map.TryGetValue(key, out n);
            map[key] = n + 1;
        }

        private static string MapText(Dictionary<string, int> map)
        {
            if (map.Count == 0) return "(none)";
            var keys = new List<string>(map.Keys);
            keys.Sort(StringComparer.Ordinal);
            var sb = new StringBuilder();
            for (int i = 0; i < keys.Count; i++)
            {
                if (i > 0) sb.Append(", ");
                sb.Append(keys[i]).Append('=').Append(map[keys[i]].ToString(CultureInfo.InvariantCulture));
            }
            return sb.ToString();
        }

        private static string Join(string[] items)
        {
            var sb = new StringBuilder();
            for (int i = 0; i < items.Length; i++)
            {
                if (i > 0) sb.Append(',');
                sb.Append(items[i]);
            }
            return sb.ToString();
        }

        private static string F(double v)
        {
            return v.ToString("0.###", CultureInfo.InvariantCulture);
        }

        private static string Quote(string s)
        {
            if (s == null) return "(null)";
            return "\"" + s + "\"";
        }

        private static string Truncate(string s, int max)
        {
            if (s == null) return "(null)";
            return s.Length <= max ? s : s.Substring(0, max) + "...";
        }

        private static string Hex(byte[] data, int offset, int count)
        {
            var sb = new StringBuilder(count * 3);
            for (int i = 0; i < count && offset + i < data.Length; i++)
            {
                if (i > 0) sb.Append(' ');
                sb.Append(data[offset + i].ToString("x2", CultureInfo.InvariantCulture));
            }
            return sb.ToString();
        }

        private static void Say(string line)
        {
            Console.WriteLine(line);
        }

        private static void Say(string format, params object[] args)
        {
            Console.WriteLine(string.Format(CultureInfo.InvariantCulture, format, args));
        }
    }

    /* ==================================================================== */
    /* 分帧：发送端，故意手写                                            */
    /* ==================================================================== */

    internal static class Framer
    {
        /// <summary>
        /// 4 字节大端长度前缀 + UTF-8 JSON，与 TcpLink.FlushOutbound 写出的
        /// 字节完全一致。之所以手写，是因为 TcpLink 每次都给 Socket.Send 交一整帧：
        /// 它造不出 C 解码器必须扛住的拆分写与粘包写。
        /// 只有阶段 1 用它。
        /// </summary>
        public static byte[] Encode(string json)
        {
            byte[] payload = Encoding.UTF8.GetBytes(json);
            var frame = new byte[4 + payload.Length];
            frame[0] = (byte)(payload.Length >> 24);
            frame[1] = (byte)(payload.Length >> 16);
            frame[2] = (byte)(payload.Length >> 8);
            frame[3] = (byte)payload.Length;
            Buffer.BlockCopy(payload, 0, frame, 4, payload.Length);
            return frame;
        }
    }

    /// <summary>阶段 1 专用的裸 socket。</summary>
    internal sealed class ProbeSocket : IDisposable
    {
        private const int MaxFrameBytes = 1024 * 1024;

        private readonly TcpClient _client;
        private readonly NetworkStream _stream;
        private readonly Socket _socket;
        private readonly byte[] _rx = new byte[65536];
        private int _rxLen;

        public long BytesSent;
        public long FramesRead;

        public ProbeSocket(string host, int port)
        {
            _client = new TcpClient();
            IAsyncResult ar = _client.BeginConnect(host, port, null, null);
            if (!ar.AsyncWaitHandle.WaitOne(5000)) throw new IOException("connect to " + host + ":" + port + " timed out after 5000 ms");
            _client.EndConnect(ar);
            _client.NoDelay = true;
            _socket = _client.Client;
            _stream = _client.GetStream();
        }

        public void Write(byte[] data, int offset, int count)
        {
            _stream.Write(data, offset, count);
            _stream.Flush();
            BytesSent += count;
        }

        public bool TryReadFrame(int timeoutMs, out string json)
        {
            json = null;
            long deadline = Peer.NowMs() + timeoutMs;
            while (true)
            {
                if (Extract(out json)) { FramesRead++; return true; }
                int left = (int)(deadline - Peer.NowMs());
                if (left <= 0) return false;
                if (!_socket.Poll(left * 1000, SelectMode.SelectRead)) return false;
                int n = _socket.Receive(_rx, _rxLen, _rx.Length - _rxLen, SocketFlags.None);
                if (n <= 0) throw new IOException("the bridge closed the connection");
                _rxLen += n;
            }
        }

        /// <summary>先静默排空一段时间，这样 close() 发的是 FIN 而不是 RST。</summary>
        public bool Quiet(int quietMs)
        {
            long deadline = Peer.NowMs() + quietMs;
            while (true)
            {
                int left = (int)(deadline - Peer.NowMs());
                if (left <= 0) return true;
                if (!_socket.Poll(left * 1000, SelectMode.SelectRead)) return true;
                if (_rxLen == _rx.Length) return false;   /* unread data: close would RST */
                int n = _socket.Receive(_rx, _rxLen, _rx.Length - _rxLen, SocketFlags.None);
                if (n <= 0) return true;                  /* bridge closed first */
                _rxLen += n;
                deadline = Peer.NowMs() + quietMs;
            }
        }

        private bool Extract(out string json)
        {
            json = null;
            if (_rxLen < 4) return false;
            int len = (_rx[0] << 24) | (_rx[1] << 16) | (_rx[2] << 8) | _rx[3];
            if (len < 0 || len > MaxFrameBytes) throw new IOException("invalid frame length from the bridge: " + len);
            if (_rxLen - 4 < len) return false;
            json = Encoding.UTF8.GetString(_rx, 4, len);
            Buffer.BlockCopy(_rx, 4 + len, _rx, 0, _rxLen - 4 - len);
            _rxLen -= 4 + len;
            return true;
        }

        public void Dispose()
        {
            try { _stream.Close(); } catch { }
            try { _client.Close(); } catch { }
        }
    }

    /* ==================================================================== */
    /* 出站 state_update，形状与 StateReader.cs 发出的完全一致            */
    /* ==================================================================== */

    internal static class StateFrames
    {
        /// <summary>本对端上报的厨师号；也是入站 action 允许指名的号。</summary>
        public static readonly int[] ChefIds = new int[] { 0, 1 };

        /// <summary>game_time 从这里起步，让整条流看起来像打到一半的厨房。</summary>
        public const long GameTimeBase = 4000;

        public static long GameTime(double elapsedSeconds)
        {
            return (long)elapsedSeconds + GameTimeBase;
        }

        /// <summary>
        /// 用插件的 JsonBuilder 搭出来的 state_update，结构完全照
        /// StateReader.BuildStateJson：顶层 {type,timestamp,game_time,
        /// score,chefs[],orders[],resources{}}，每个 chef/order/resource 的值
        /// 都由各自的嵌套 JsonBuilder 产出，再用 Raw() 拼进来。
        ///
        /// 内容是写实的：两个有坐标的厨师，其中一个手里拿着东西；
        /// 两张订单（其中一张的菜谱解析不出来，StateReader 遇到这种情况
        /// 就发 JSON null）；资源覆盖 StateReader 能产生的全部五种形状——
        /// 带 time_left 的锅、空闲的灶、
        /// 被占用的具名工位（cutting_board_0）、plate_stack 计数，
        /// 外加一个 C 侧归不了类的键。
        /// </summary>
        public static string Build(double elapsedSeconds)
        {
            return BuildCore(GameTime(elapsedSeconds), 40 + (long)elapsedSeconds * 10);
        }

        /// <summary>
        /// 同一个帧，但把 game_time 钉成确定的值，好让桥日志
        /// 能对上是哪一个探针产生的（见 Peer 的标记常量）。
        /// </summary>
        public static string BuildAtGameTime(long gameTime)
        {
            return BuildCore(gameTime, 0);
        }

        private static string BuildCore(long gameTime, long score)
        {
            var b = new JsonBuilder(2048);
            b.Obj();
            b.Field("type", MsgType.StateUpdate);
            b.FieldInt("timestamp", Peer.UnixNow());
            b.FieldInt("game_time", gameTime);
            b.FieldInt("score", score);

            b.Key("chefs").Arr();
            b.Raw(ChefJson(ChefIds[0], ChefX(gameTime), 3.0f, "tomato", "moving"));
            b.Raw(ChefJson(ChefIds[1], -2.0f, 1.25f, null, "idle"));
            b.EndArr();

            b.Key("orders").Arr();
            b.Raw(OrderJson(1, "salad", 42 - (long)gameTime, new string[] { "tomato", "lettuce" }));
            b.Raw(OrderJson(2, null, 12, new string[] { "onion" }));
            b.EndArr();

            b.Key("resources").Obj();
            b.Key("pot_0");
            b.Raw(PotJson(gameTime));
            b.Key("cooker_0");
            b.Raw(FreeJson());
            b.Key("cutting_board_0");
            b.Raw(StationJson(0));
            b.Key("plate_stack");
            b.Raw(PlateStackJson(7));
            b.Key("sink_2");
            b.Raw(FreeJson());
            b.EndObj();

            b.EndObj();
            return b.ToString();
        }

        /// <summary>
        /// 一帧专门顶 C 侧写在文档里的上限：`resources` 有 50 个键
        /// （PROTO_MAX_RESOURCES 是 48），第一个键还长过
        /// PROTO_KEY_MAX。插件不会产生这种东西；它在这里是为了让桥的
        /// dropped= 与 truncated= 计数可以被证伪，
        /// 而不是永远是 0。
        /// </summary>
        public static string BuildCapProbe(string longKey, int resourceCount, long marker)
        {
            var b = new JsonBuilder(4096);
            b.Obj();
            b.Field("type", MsgType.StateUpdate);
            b.FieldInt("timestamp", Peer.UnixNow());
            b.FieldInt("game_time", marker);
            for (int i = 0; i < resourceCount; i++)
            {
                if (i == 0) { b.Key("resources").Obj(); b.Key(longKey); }
                else b.Key("probe_" + i.ToString("D2", CultureInfo.InvariantCulture));
                b.Raw(FreeJson());
            }
            b.EndObj();
            b.EndObj();
            return b.ToString();
        }

        private static float ChefX(double gameTime)
        {
            /* 一条缓慢扫过的轨迹，像厨师穿过厨房的样子。 */
            double phase = gameTime % 6.0;
            return (float)(-4.0 + (phase / 6.0) * 8.0);
        }

        private static string ChefJson(int id, float x, float z, string holding, string state)
        {
            var b = new JsonBuilder(160);
            b.Obj();
            b.FieldInt("id", id);
            b.Key("position").Obj().FieldNum("x", x).FieldNum("y", z).EndObj();
            if (holding == null) b.FieldNull("holding");
            else b.Field("holding", holding);
            b.Field("state", state);
            b.EndObj();
            return b.ToString();
        }

        private static string OrderJson(int number, string recipe, long timeRemaining, string[] ingredients)
        {
            var b = new JsonBuilder(256);
            b.Obj();
            b.Field("id", "order_" + number.ToString("D3", CultureInfo.InvariantCulture));
            if (recipe == null) b.FieldNull("recipe");
            else b.Field("recipe", recipe);
            b.FieldInt("time_remaining", timeRemaining);
            b.Key("ingredients").Arr();
            for (int i = 0; i < ingredients.Length; i++) b.Item(ingredients[i]);
            b.EndArr();
            b.EndObj();
            return b.ToString();
        }

        private static string FreeJson()
        {
            return new JsonBuilder(64).Obj().Field("state", "free").EndObj().ToString();
        }

        private static string PotJson(double gameTime)
        {
            var b = new JsonBuilder(160);
            b.Obj();
            b.Field("state", "cooking");
            b.Field("content", "onion+tomato");
            b.FieldNum("time_left", 2.4 - (gameTime % 2.0) * 0.5);
            b.EndObj();
            return b.ToString();
        }

        private static string StationJson(int chefId)
        {
            var b = new JsonBuilder(128);
            b.Obj();
            b.Field("state", "occupied");
            b.FieldInt("chef_id", chefId);
            b.EndObj();
            return b.ToString();
        }

        private static string PlateStackJson(int count)
        {
            var b = new JsonBuilder(64);
            b.Obj().FieldInt("count", count).EndObj();
            return b.ToString();
        }
    }

    /* ==================================================================== */
    /* JSON 读取器                                                         */
    /* ==================================================================== */

    internal enum JsonKind { Null, Bool, Number, String, Array, Object }

    internal sealed class JsonParseException : Exception
    {
        public JsonParseException(string message) : base(message) { }
    }

    internal sealed class JsonValue
    {
        public JsonKind Kind;
        public bool Bool;
        public double Number;
        public string Text;
        public List<JsonValue> Items;
        public List<KeyValuePair<string, JsonValue>> Members;

        public bool IsNull { get { return Kind == JsonKind.Null; } }

        public JsonValue Member(string name)
        {
            if (Members == null) return null;
            for (int i = 0; i < Members.Count; i++)
            {
                if (string.Equals(Members[i].Key, name, StringComparison.Ordinal)) return Members[i].Value;
            }
            return null;
        }

        public string KindName()
        {
            switch (Kind)
            {
                case JsonKind.Null: return "null";
                case JsonKind.Bool: return "boolean";
                case JsonKind.Number: return "number";
                case JsonKind.String: return "string";
                case JsonKind.Array: return "array";
                default: return "object";
            }
        }
    }

    /// <summary>
    /// 针对协议用到的子集写的严格 RFC 8259 读取器。JsonUtility 只属于 Unity
    /// （见文件头），所以语法在这里读，字段契约由对 InboundMessage 的反射
    /// 来强制：未知键、JSON 类型不对、
    /// 非整数的整数，一律报出来，而不是硬转。
    /// </summary>
    internal static class Json
    {
        public static JsonValue Parse(string text)
        {
            if (text == null) throw new JsonParseException("null input");
            int i = 0;
            JsonValue v = ParseValue(text, ref i, 0);
            SkipWs(text, ref i);
            if (i != text.Length)
                throw new JsonParseException("trailing data at offset " + i + " (more than one JSON value in one frame)");
            return v;
        }

        private static JsonValue ParseValue(string s, ref int i, int depth)
        {
            if (depth > 64) throw new JsonParseException("nesting deeper than 64 at offset " + i);
            SkipWs(s, ref i);
            if (i >= s.Length) throw new JsonParseException("unexpected end of input at offset " + i);

            char c = s[i];
            if (c == '{') return ParseObject(s, ref i, depth);
            if (c == '[') return ParseArray(s, ref i, depth);
            if (c == '"')
            {
                var v = new JsonValue();
                v.Kind = JsonKind.String;
                v.Text = ParseString(s, ref i);
                return v;
            }
            if (c == 't') { Expect(s, ref i, "true"); var v = new JsonValue(); v.Kind = JsonKind.Bool; v.Bool = true; return v; }
            if (c == 'f') { Expect(s, ref i, "false"); var v = new JsonValue(); v.Kind = JsonKind.Bool; v.Bool = false; return v; }
            if (c == 'n') { Expect(s, ref i, "null"); var v = new JsonValue(); v.Kind = JsonKind.Null; return v; }
            return ParseNumber(s, ref i);
        }

        private static JsonValue ParseObject(string s, ref int i, int depth)
        {
            var v = new JsonValue();
            v.Kind = JsonKind.Object;
            v.Members = new List<KeyValuePair<string, JsonValue>>();
            i++;                                  /* '{' */
            SkipWs(s, ref i);
            if (i < s.Length && s[i] == '}') { i++; return v; }
            while (true)
            {
                SkipWs(s, ref i);
                if (i >= s.Length || s[i] != '"') throw new JsonParseException("expected a member name at offset " + i);
                string name = ParseString(s, ref i);
                SkipWs(s, ref i);
                if (i >= s.Length || s[i] != ':') throw new JsonParseException("expected ':' after member " + name + " at offset " + i);
                i++;
                JsonValue value = ParseValue(s, ref i, depth + 1);
                v.Members.Add(new KeyValuePair<string, JsonValue>(name, value));
                SkipWs(s, ref i);
                if (i < s.Length && s[i] == ',') { i++; continue; }
                if (i < s.Length && s[i] == '}') { i++; return v; }
                throw new JsonParseException("expected ',' or '}' at offset " + i);
            }
        }

        private static JsonValue ParseArray(string s, ref int i, int depth)
        {
            var v = new JsonValue();
            v.Kind = JsonKind.Array;
            v.Items = new List<JsonValue>();
            i++;                                  /* '[' */
            SkipWs(s, ref i);
            if (i < s.Length && s[i] == ']') { i++; return v; }
            while (true)
            {
                v.Items.Add(ParseValue(s, ref i, depth + 1));
                SkipWs(s, ref i);
                if (i < s.Length && s[i] == ',') { i++; continue; }
                if (i < s.Length && s[i] == ']') { i++; return v; }
                throw new JsonParseException("expected ',' or ']' at offset " + i);
            }
        }

        private static string ParseString(string s, ref int i)
        {
            int start = i;
            i++;                                  /* opening quote */
            var sb = new StringBuilder();
            while (true)
            {
                if (i >= s.Length) throw new JsonParseException("unterminated string starting at offset " + start);
                char c = s[i++];
                if (c == '"') return sb.ToString();
                if (c != '\\') { sb.Append(c); continue; }

                if (i >= s.Length) throw new JsonParseException("unterminated escape at offset " + (i - 1));
                char e = s[i++];
                switch (e)
                {
                    case '"': sb.Append('"'); break;
                    case '\\': sb.Append('\\'); break;
                    case '/': sb.Append('/'); break;
                    case 'b': sb.Append('\b'); break;
                    case 'f': sb.Append('\f'); break;
                    case 'n': sb.Append('\n'); break;
                    case 'r': sb.Append('\r'); break;
                    case 't': sb.Append('\t'); break;
                    case 'u':
                        if (i + 4 > s.Length) throw new JsonParseException("truncated \\u escape at offset " + (i - 2));
                        int code = 0;
                        for (int k = 0; k < 4; k++)
                        {
                            int h = HexDigit(s[i + k]);
                            if (h < 0) throw new JsonParseException("bad \\u escape (" + s.Substring(i, 4) + ") at offset " + (i - 2));
                            code = code * 16 + h;
                        }
                        i += 4;
                        sb.Append((char)code);
                        break;
                    default:
                        throw new JsonParseException("unknown escape \\" + e + " at offset " + (i - 1));
                }
            }
        }

        private static JsonValue ParseNumber(string s, ref int i)
        {
            int start = i;
            if (i < s.Length && s[i] == '-') i++;
            if (i >= s.Length || !IsDigit(s[i])) throw new JsonParseException("invalid number at offset " + start);
            if (s[i] == '0') i++;
            else while (i < s.Length && IsDigit(s[i])) i++;
            if (i < s.Length && s[i] == '.')
            {
                i++;
                if (i >= s.Length || !IsDigit(s[i])) throw new JsonParseException("invalid fraction at offset " + i);
                while (i < s.Length && IsDigit(s[i])) i++;
            }
            if (i < s.Length && (s[i] == 'e' || s[i] == 'E'))
            {
                i++;
                if (i < s.Length && (s[i] == '+' || s[i] == '-')) i++;
                if (i >= s.Length || !IsDigit(s[i])) throw new JsonParseException("invalid exponent at offset " + i);
                while (i < s.Length && IsDigit(s[i])) i++;
            }

            string token = s.Substring(start, i - start);
            double d;
            try
            {
                d = double.Parse(token, NumberStyles.Float, CultureInfo.InvariantCulture);
            }
            catch (OverflowException)
            {
                throw new JsonParseException("number " + token + " at offset " + start + " is out of double range");
            }
            var v = new JsonValue();
            v.Kind = JsonKind.Number;
            v.Number = d;
            return v;
        }

        private static void Expect(string s, ref int i, string literal)
        {
            if (i + literal.Length > s.Length || string.CompareOrdinal(s, i, literal, 0, literal.Length) != 0)
                throw new JsonParseException("expected " + literal + " at offset " + i);
            i += literal.Length;
        }

        private static void SkipWs(string s, ref int i)
        {
            while (i < s.Length)
            {
                char c = s[i];
                if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { i++; continue; }
                break;
            }
        }

        private static bool IsDigit(char c) { return c >= '0' && c <= '9'; }

        private static int HexDigit(char c)
        {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        }
    }
}
