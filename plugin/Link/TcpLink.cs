using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading;

namespace WellCooked
{
    /// <summary>
    /// TCP 链路（单后台线程）：分帧 = 一行一条 UTF-8 JSON，以 '\n' 结尾，单行上限 1MB。
    /// 字节级收发都在 worker 线程；入站投递到队列，由 Unity 主线程消费（任务 03）。
    ///
    /// 这份是从 `plugin/WellCooked.Plugin/Net/TcpLink.cs`（net46 参考实现）搬过来的，只改了三处
    /// **net35/游戏 Mono 上没有的类型**，其余逐字保留（分帧规则与上限是协议的一部分）：
    ///   - `ConcurrentQueue` → `Queue` + 自己的锁（net35 没有 System.Collections.Concurrent）
    ///   - `ManualResetEventSlim` → `ManualResetEvent`
    ///   - `DateTimeOffset.ToUnixTimeSeconds()` → 自己算（net35 的 DateTimeOffset 没有这个方法）
    /// </summary>
    public sealed class TcpLink
    {
        public enum Mode
        {
            Client,
            Server
        }

        /// <summary>一行 JSON 的字节上限，不计结尾的 '\n'。</summary>
        private const int MaxFrameBytes = 1024 * 1024;
        private const int MaxInboundQueue = 256;
        private const int MaxOutboundQueue = 32;

        /// <summary>日志回调（由上层注入，链路层自己不依赖 BepInEx）。</summary>
        public Action<string> Log = delegate { };

        private readonly Mode _mode;
        private readonly string _host;
        private readonly int _port;
        private readonly string _listenAddress;
        private readonly int _heartbeatMs;
        private readonly int _heartbeatTimeoutMs;
        private readonly int _reconnectDelayMs;

        private readonly Queue<string> _inbound = new Queue<string>();
        private long _droppedActions;   // 队列满时丢掉的 action 数（只涨不落，日志限频）
        private long _droppedPassive;   // 同上，丢的是心跳/echo 这类
        private readonly Queue<string> _outbound = new Queue<string>();
        private readonly object _inboundLock = new object();
        private readonly object _outboundLock = new object();
        private readonly ManualResetEvent _wake = new ManualResetEvent(false);

        private Thread _thread;
        private volatile bool _running;
        private volatile bool _connected;
        private int _attempts;   // 连续失败次数，进日志（任务清单要求"日志打印重试次数"）
        private TcpClient _client;
        private TcpListener _listener;
        private long _lastRxMs;

        public bool Connected
        {
            get { return _connected; }
        }

        public TcpLink(Mode mode, string host, int port, string listenAddress,
                       int heartbeatMs, int heartbeatTimeoutMs, int reconnectDelayMs)
        {
            _mode = mode;
            _host = host;
            _port = port;
            _listenAddress = listenAddress;
            _heartbeatMs = heartbeatMs;
            _heartbeatTimeoutMs = heartbeatTimeoutMs;
            _reconnectDelayMs = reconnectDelayMs;
        }

        public void Start()
        {
            if (_running)
            {
                return;
            }

            _running = true;
            _thread = new Thread(Worker);
            _thread.IsBackground = true;
            _thread.Name = "WellCooked.TcpLink";
            _thread.Start();
        }

        public void Stop()
        {
            if (!_running)
            {
                return;
            }

            _running = false;
            _wake.Set();
            try { if (_client != null) _client.Close(); } catch { }
            try { if (_listener != null) _listener.Stop(); } catch { }
            try { if (_thread != null) _thread.Join(1500); } catch { }
            _thread = null;
        }

        /// <summary>主线程取一条入站 JSON（无消息返回 false）。</summary>
        public bool TryDequeueInbound(out string json)
        {
            lock (_inboundLock)
            {
                if (_inbound.Count == 0)
                {
                    json = null;
                    return false;
                }

                json = _inbound.Dequeue();
                return true;
            }
        }

        /// <summary>发送一条 JSON（未连接时静默丢弃；队列满时丢最旧）。</summary>
        public void Send(string json)
        {
            if (string.IsNullOrEmpty(json) || !_connected)
            {
                return;
            }

            lock (_outboundLock)
            {
                while (_outbound.Count >= MaxOutboundQueue)
                {
                    _outbound.Dequeue();
                }

                _outbound.Enqueue(json);
            }
        }

        // ---------------- worker ----------------

        private void Worker()
        {
            while (_running)
            {
                try
                {
                    if (!Acquire())
                    {
                        continue;
                    }

                    /* 先清空出站队列，再发布"已连接"。
                     * 反过来的话（先 _connected = true 再清）主线程可能在中间这一瞬间
                     * 看到已连接、把 hello 和首帧状态入队，紧接着被这里清掉 —— 而
                     * LinkRunner 只在"断开→连接"这一跳发一次 hello，于是这条连接永远没有握手。 */
                    lock (_outboundLock)
                    {
                        _outbound.Clear(); // 断线期间的旧动作没有意义，别在重连后补发
                    }
                    _connected = true;
                    if (_attempts > 0)
                    {
                        Log("[Link] 第 " + _attempts + " 次重试连上了 " + DescribePeer());
                        _attempts = 0;
                    }

                    _lastRxMs = NowMs();
                    Log("[Link] connected (" + (_mode == Mode.Client ? "client" : "server") + " " + DescribePeer() + ")");
                    PumpLoop();
                }
                catch (Exception e)
                {
                    if (_running)
                    {
                        Log("[Link] link error: " + e.Message);
                    }
                }
                finally
                {
                    if (_connected)
                    {
                        _connected = false;
                        Log("[Link] disconnected");
                    }

                    CloseConnection();
                    if (_running && _mode == Mode.Client)
                    {
                        Sleep(_reconnectDelayMs);
                    }
                }
            }
        }

        /// <summary>
        /// 队列满时挑一条非 action 的消息丢掉；一条都没有就返回 false（调用方丢最旧的 action）。
        /// 判定靠字符串：入站消息只有 Bridge 这一个生产者，`{"type":"action"` 是它固定的写法
        /// （见 `bridge/src/protocol.c` 的 builder）。真判错也不会更糟 —— 退化成"丢最旧的 action
        /// 并计数告警"，也就是调用方那条路。
        /// </summary>
        private bool DropOldestNonAction()
        {
            int count = _inbound.Count;
            string victim = null;
            for (int i = 0; i < count; i++)
            {
                string item = _inbound.Dequeue();
                if (victim == null && item.IndexOf("\"type\":\"action\"", StringComparison.Ordinal) < 0)
                {
                    victim = item;
                }
                else
                {
                    _inbound.Enqueue(item);
                }
            }
            return victim != null;
        }

        private string DescribePeer()
        {
            return _mode == Mode.Client ? _host + ":" + _port : "listening " + _listenAddress + ":" + _port;
        }

        private bool Acquire()
        {
            return _mode == Mode.Client ? AcquireClient() : AcquireServer();
        }

        private bool AcquireClient()
        {
            var client = new TcpClient();
            _client = client;
            try
            {
                IAsyncResult ar = client.BeginConnect(_host, _port, null, null);
                if (!ar.AsyncWaitHandle.WaitOne(3000))
                {
                    throw new TimeoutException("connect timeout after 3000ms");
                }

                client.EndConnect(ar);
                client.Client.SetSocketOption(SocketOptionLevel.Tcp, SocketOptionName.NoDelay, true);
                client.Client.SendTimeout = 2000;
                client.Client.ReceiveTimeout = 0;
                return true;
            }
            catch (Exception e)
            {
                _attempts++;
                if (_running)
                {
                    Log("[Link] connect to " + _host + ":" + _port + " failed（第 " + _attempts + " 次，"
                        + _reconnectDelayMs + " ms 后重试）: " + e.Message);
                }

                try { client.Close(); } catch { }
                _client = null;
                // 等待统一由 Worker 的 finally 做：这里再睡一次会让实际间隔变成两倍
                return false;
            }
        }

        private bool AcquireServer()
        {
            if (_listener == null)
            {
                IPAddress addr;
                if (_listenAddress == "0.0.0.0")
                {
                    addr = IPAddress.Any;
                }
                else if (!IPAddress.TryParse(_listenAddress, out addr))
                {
                    addr = IPAddress.Loopback;
                }

                try
                {
                    _listener = new TcpListener(addr, _port);
                    _listener.Start(1);
                    Log("[Link] listening on " + addr + ":" + _port);
                }
                catch (Exception e)
                {
                    _attempts++;
                    Log("[Link] listen failed（第 " + _attempts + " 次）: " + e.Message);
                    Sleep(_reconnectDelayMs);   // server 模式没有 finally 的等待，这里自己等
                    return false;
                }
            }

            while (_running && !_listener.Pending())
            {
                Sleep(25);
            }

            if (!_running)
            {
                return false;
            }

            try
            {
                TcpClient client = _listener.AcceptTcpClient();
                client.Client.SetSocketOption(SocketOptionLevel.Tcp, SocketOptionName.NoDelay, true);
                client.Client.SendTimeout = 2000;
                client.Client.ReceiveTimeout = 0;
                _client = client;
                return true;
            }
            catch (Exception e)
            {
                if (_running)
                {
                    Log("[Link] accept failed: " + e.Message);
                }

                Sleep(_reconnectDelayMs);   // 同上：server 模式的重试节奏由这里给
                return false;
            }
        }

        private void PumpLoop()
        {
            Socket sock = _client.Client;
            var rx = new byte[65536];
            int rxLen = 0;
            long lastHeartbeat = NowMs();

            while (_running && _connected)
            {
                if (sock.Poll(25000, SelectMode.SelectRead))
                {
                    if (rxLen == rx.Length)
                    {
                        Grow(ref rx);
                    }

                    int n;
                    try
                    {
                        n = sock.Receive(rx, rxLen, rx.Length - rxLen, SocketFlags.None);
                    }
                    catch (SocketException se)
                    {
                        if (se.SocketErrorCode == SocketError.TimedOut)
                        {
                            n = 0;
                        }
                        else
                        {
                            throw;
                        }
                    }

                    if (n == 0 && sock.Poll(0, SelectMode.SelectRead) && sock.Available == 0)
                    {
                        throw new IOException("peer closed the connection");
                    }

                    if (n > 0)
                    {
                        rxLen += n;
                        _lastRxMs = NowMs();
                        rxLen = ExtractFrames(rx, rxLen);
                    }
                }

                long now = NowMs();

                if (_heartbeatMs > 0 && now - lastHeartbeat >= _heartbeatMs)
                {
                    lastHeartbeat = now;
                    SendHeartbeat(sock);
                }

                FlushOutbound(sock);

                if (_heartbeatTimeoutMs > 0 && now - _lastRxMs > _heartbeatTimeoutMs)
                {
                    throw new IOException("heartbeat timeout (" + _heartbeatTimeoutMs + "ms without any message)");
                }
            }
        }

        private static void Grow(ref byte[] rx)
        {
            int capacity = rx.Length * 2;
            // 上限要容得下"最大的一行"：1MB 载荷 + 结尾的 '\n'，否则卡在 1MB 的那条合法消息
            // 会因为缓冲区满而永远等不到换行符。
            if (capacity > MaxFrameBytes + 1)
            {
                capacity = MaxFrameBytes + 1;
            }

            if (capacity <= rx.Length)
            {
                throw new IOException("inbound line exceeds the 1MB limit");
            }

            var bigger = new byte[capacity];
            Buffer.BlockCopy(rx, 0, bigger, 0, rx.Length);
            rx = bigger;
        }

        /// <summary>从缓冲区抽取完整行入队，返回剩余字节数（已压缩到头部）。</summary>
        private int ExtractFrames(byte[] rx, int rxLen)
        {
            int off = 0;
            while (off < rxLen)
            {
                int nl = Array.IndexOf(rx, (byte)'\n', off, rxLen - off);
                if (nl < 0)
                {
                    // 没有换行符 = 这条消息还没收完，或者对端发了一条永远不会结束的行。
                    // 后者没法恢复（没有换行符就没有重新同步的位置），只能断开。
                    if (rxLen - off > MaxFrameBytes)
                    {
                        throw new IOException("inbound line exceeds the 1MB limit");
                    }

                    break;
                }

                int len = nl - off;
                if (len > MaxFrameBytes)
                {
                    throw new IOException("invalid line length: " + len);
                }

                string json = Encoding.UTF8.GetString(rx, off, len);
                lock (_inboundLock)
                {
                    _inbound.Enqueue(json);
                    while (_inbound.Count > MaxInboundQueue)
                    {
                        /* 满了先丢一条非 action 的消息：心跳/echo 之类丢了下一次还有，
                         * 而 action 是"要执行"的指令 —— 静默丢掉的话对端不知道，
                         * 既不 ack 也不重发（对端按帧重发，下一帧还会来，但日志里必须留痕）。 */
                        if (DropOldestNonAction())
                        {
                            _droppedPassive++;
                        }
                        else
                        {
                            _inbound.Dequeue();
                            _droppedActions++;
                            if (_droppedActions == 1 || _droppedActions % 100 == 0)
                            {
                                Log("[Link] 入站队列满（" + MaxInboundQueue + "），已丢掉 "
                                    + _droppedActions + " 条 action；对端按帧重发，下一帧会再来");
                            }
                        }
                    }
                }

                off = nl + 1;
            }

            if (off > 0)
            {
                Buffer.BlockCopy(rx, off, rx, 0, rxLen - off);
                rxLen -= off;
            }

            return rxLen;
        }

        /// <summary>
        /// 组帧：载荷后跟一个 '\n'。载荷里带裸换行符就没法分帧（对端只会把它读成两行），
        /// 返回 null 让调用方丢掉这条消息 —— 行分帧没有别的转义手段。
        /// </summary>
        private static byte[] EncodeFrame(byte[] payload)
        {
            if (Array.IndexOf(payload, (byte)'\n') >= 0)
            {
                return null;
            }

            var frame = new byte[payload.Length + 1];
            Buffer.BlockCopy(payload, 0, frame, 0, payload.Length);
            frame[payload.Length] = (byte)'\n';
            return frame;
        }

        private void FlushOutbound(Socket sock)
        {
            while (true)
            {
                string msg;
                lock (_outboundLock)
                {
                    if (_outbound.Count == 0)
                    {
                        return;
                    }

                    msg = _outbound.Dequeue();
                }

                byte[] payload = Encoding.UTF8.GetBytes(msg);
                if (payload.Length > MaxFrameBytes)
                {
                    continue;
                }

                byte[] frame = EncodeFrame(payload);
                if (frame == null)
                {
                    Log("[Link] outbound message contains a raw newline, dropped");
                    continue;
                }

                SendAll(sock, frame);
            }
        }

        private void SendHeartbeat(Socket sock)
        {
            byte[] payload = Encoding.UTF8.GetBytes(Msg.Heartbeat(UnixSeconds()));
            byte[] frame = EncodeFrame(payload);
            SendAll(sock, frame);
        }

        /// <summary>
        /// 循环发送直到整帧写完。
        ///
        /// `Socket.Send` 允许只发出去一部分（短写），原来只调一次、把没发出去的那截丢掉：
        /// 对端会看到半条 JSON 行（缺结尾换行），下一条消息的字节又接在残留后面 ——
        /// 分帧按换行切，一旦错位后面全错，而这条链路上没有重同步机制。
        /// </summary>
        private static void SendAll(Socket sock, byte[] frame)
        {
            int sent = 0;
            while (sent < frame.Length)
            {
                int n = sock.Send(frame, sent, frame.Length - sent, SocketFlags.None);
                if (n <= 0)
                {
                    throw new IOException("socket send returned " + n);
                }
                sent += n;
            }
        }

        /// <summary>Unix 秒（net35 的 DateTimeOffset 没有 ToUnixTimeSeconds）。</summary>
        public static long UnixSeconds()
        {
            return (long)(DateTime.UtcNow - new DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind.Utc)).TotalSeconds;
        }

        private void CloseConnection()
        {
            try { if (_client != null) _client.Close(); } catch { }
            _client = null;
        }

        private void Sleep(int ms)
        {
            if (ms <= 0)
            {
                return;
            }

            _wake.WaitOne(ms);
            _wake.Reset();
        }

        private static long NowMs()
        {
            return (long)(Stopwatch.GetTimestamp() * 1000.0 / Stopwatch.Frequency);
        }
    }
}
