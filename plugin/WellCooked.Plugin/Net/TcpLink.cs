using System;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading;

namespace WellCooked.Net
{
    /// <summary>
    /// TCP 链路（单后台线程）：
    /// 帧格式 = 4 字节大端（网络序）长度前缀 + UTF-8 JSON，单帧上限 1MB。
    /// 字节级收发都在 worker 线程；入站投递到队列由 Unity 主线程消费。
    /// </summary>
    public sealed class TcpLink
    {
        public enum Mode
        {
            Client,
            Server
        }

        private const int MaxFrameBytes = 1024 * 1024;
        private const int MaxInboundQueue = 64;
        private const int MaxOutboundQueue = 32;

        /// <summary>日志回调（由 Plugin 注入，避免 Net 层依赖 BepInEx）。</summary>
        public Action<string> Log = delegate { };

        private readonly Mode _mode;
        private readonly string _host;
        private readonly int _port;
        private readonly string _listenAddress;
        private readonly int _heartbeatMs;
        private readonly int _heartbeatTimeoutMs;
        private readonly int _reconnectDelayMs;

        private readonly ConcurrentQueue<string> _inbound = new ConcurrentQueue<string>();
        private readonly ConcurrentQueue<string> _outbound = new ConcurrentQueue<string>();
        private readonly ManualResetEventSlim _wake = new ManualResetEventSlim(false);

        private Thread _thread;
        private volatile bool _running;
        private volatile bool _connected;
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
            if (_running) return;
            _running = true;
            _thread = new Thread(Worker) { IsBackground = true, Name = "WellCooked.TcpLink" };
            _thread.Start();
        }

        public void Stop()
        {
            if (!_running) return;
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
            return _inbound.TryDequeue(out json);
        }

        /// <summary>发送一条 JSON（未连接时静默丢弃；队列满时丢最旧）。</summary>
        public void Send(string json)
        {
            if (string.IsNullOrEmpty(json) || !_connected) return;
            while (_outbound.Count >= MaxOutboundQueue)
            {
                string dropped;
                _outbound.TryDequeue(out dropped);
            }
            _outbound.Enqueue(json);
        }

        // ---------------- worker ----------------

        private void Worker()
        {
            while (_running)
            {
                try
                {
                    if (!Acquire()) continue;
                    _connected = true;
                    string stale;
                    while (_outbound.TryDequeue(out stale)) { }
                    _lastRxMs = NowMs();
                    Log("[TcpLink] connected (" + _mode + " " + DescribePeer() + ")");
                    PumpLoop();
                }
                catch (Exception e)
                {
                    if (_running) Log("[TcpLink] link error: " + e.Message);
                }
                finally
                {
                    if (_connected)
                    {
                        _connected = false;
                        Log("[TcpLink] disconnected");
                    }
                    CloseConnection();
                    if (_running && _mode == Mode.Client) Sleep(_reconnectDelayMs);
                }
            }
        }

        private string DescribePeer()
        {
            if (_mode == Mode.Client) return _host + ":" + _port;
            return "listening " + _listenAddress + ":" + _port;
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
                    throw new TimeoutException("connect timeout after 3000ms");
                client.EndConnect(ar);
                client.Client.SetSocketOption(SocketOptionLevel.Tcp, SocketOptionName.NoDelay, true);
                client.Client.SendTimeout = 2000;
                client.Client.ReceiveTimeout = 0;
                return true;
            }
            catch (Exception e)
            {
                if (_running) Log("[TcpLink] connect to " + _host + ":" + _port + " failed: " + e.Message);
                try { client.Close(); } catch { }
                _client = null;
                Sleep(_reconnectDelayMs);
                return false;
            }
        }

        private bool AcquireServer()
        {
            if (_listener == null)
            {
                IPAddress addr;
                if (_listenAddress == "0.0.0.0") addr = IPAddress.Any;
                else if (!IPAddress.TryParse(_listenAddress, out addr)) addr = IPAddress.Loopback;
                try
                {
                    _listener = new TcpListener(addr, _port);
                    _listener.Start(1);
                    Log("[TcpLink] listening on " + addr + ":" + _port);
                }
                catch (Exception e)
                {
                    Log("[TcpLink] listen failed: " + e.Message);
                    Sleep(_reconnectDelayMs);
                    return false;
                }
            }

            while (_running && !_listener.Pending())
            {
                Sleep(25);
            }
            if (!_running) return false;

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
                if (_running) Log("[TcpLink] accept failed: " + e.Message);
                Sleep(_reconnectDelayMs);
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
                    if (rxLen == rx.Length) Grow(ref rx);
                    int n;
                    try
                    {
                        n = sock.Receive(rx, rxLen, rx.Length - rxLen, SocketFlags.None);
                    }
                    catch (SocketException se)
                    {
                        if (se.SocketErrorCode == SocketError.TimedOut) n = 0;
                        else throw;
                    }
                    if (n == 0 && sock.Poll(0, SelectMode.SelectRead) && sock.Available == 0)
                    {
                        // 对端已关闭
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
            if (capacity > MaxFrameBytes + 4) capacity = MaxFrameBytes + 4;
            if (capacity <= rx.Length) throw new IOException("inbound frame exceeds 1MB limit");
            var bigger = new byte[capacity];
            Buffer.BlockCopy(rx, 0, bigger, 0, rx.Length);
            rx = bigger;
        }

        /// <summary>从缓冲区抽取完整帧入队，返回剩余字节数（已压缩到头部）。</summary>
        private int ExtractFrames(byte[] rx, int rxLen)
        {
            int off = 0;
            while (rxLen - off >= 4)
            {
                int len = (rx[off] << 24) | (rx[off + 1] << 16) | (rx[off + 2] << 8) | rx[off + 3];
                if (len < 0 || len > MaxFrameBytes)
                    throw new IOException("invalid frame length: " + len);
                if (rxLen - off - 4 < len) break;
                string json = Encoding.UTF8.GetString(rx, off + 4, len);
                _inbound.Enqueue(json);
                while (_inbound.Count > MaxInboundQueue)
                {
                    string dropped;
                    _inbound.TryDequeue(out dropped);
                }
                off += 4 + len;
            }
            if (off > 0)
            {
                Buffer.BlockCopy(rx, off, rx, 0, rxLen - off);
                rxLen -= off;
            }
            return rxLen;
        }

        private void FlushOutbound(Socket sock)
        {
            string msg;
            while (_outbound.TryDequeue(out msg))
            {
                byte[] payload = Encoding.UTF8.GetBytes(msg);
                if (payload.Length > MaxFrameBytes) continue;
                var frame = new byte[4 + payload.Length];
                frame[0] = (byte)(payload.Length >> 24);
                frame[1] = (byte)(payload.Length >> 16);
                frame[2] = (byte)(payload.Length >> 8);
                frame[3] = (byte)payload.Length;
                Buffer.BlockCopy(payload, 0, frame, 4, payload.Length);
                sock.Send(frame, 0, frame.Length, SocketFlags.None);
            }
        }

        private void SendHeartbeat(Socket sock)
        {
            string json = Msg_Heartbeat();
            byte[] payload = Encoding.UTF8.GetBytes(json);
            var frame = new byte[4 + payload.Length];
            frame[0] = (byte)(payload.Length >> 24);
            frame[1] = (byte)(payload.Length >> 16);
            frame[2] = (byte)(payload.Length >> 8);
            frame[3] = (byte)payload.Length;
            Buffer.BlockCopy(payload, 0, frame, 4, payload.Length);
            sock.Send(frame, 0, frame.Length, SocketFlags.None);
        }

        private static string Msg_Heartbeat()
        {
            long unix = DateTimeOffset.UtcNow.ToUnixTimeSeconds();
            return Protocol.Msg.Heartbeat(unix);
        }

        private void CloseConnection()
        {
            try { if (_client != null) _client.Close(); } catch { }
            _client = null;
        }

        private void Sleep(int ms)
        {
            if (ms <= 0) return;
            _wake.Wait(ms);
            _wake.Reset();
        }

        private static long NowMs()
        {
            return (long)(Stopwatch.GetTimestamp() * 1000.0 / Stopwatch.Frequency);
        }
    }
}
