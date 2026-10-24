using System;
using System.Collections.Generic;
using System.Globalization;
using System.Text;

namespace WellCooked
{
    // 跨端协议的消息层（任务 03 从 plugin/WellCooked.Plugin/Protocol/Messages.cs 搬过来）。
    // 那份是旧参考工程（也是 net35）的实现，这里按本工程的写法落在同一个 `WellCooked` 命名空间下，
    // 逐字保留字段名与转义规则 —— 两端必须与 protocol/protocol.md 和 bridge/src/protocol.c 对齐。

    /// <summary>2D 向量（映射游戏世界 x/z 平面）。</summary>
    [Serializable]
    public class Vec2
    {
        public float x;
        public float y;
    }

    /// <summary>
    /// 入站消息的统一包装类（JsonUtility 需要具体类型；未知 JSON 字段会被自动忽略）。
    /// 覆盖: heartbeat / echo / action / config / state_request / ack
    /// </summary>
    [Serializable]
    public class InboundMessage
    {
        public string type;
        public double timestamp;
        public int chef_id;
        public string action;
        public string button;
        public int hold_ms;
        public Vec2 target;
        public string target_object;
        public string token;
        public int state_interval_ms;
        public string input_mode;
        public string strategy;
    }

    /// <summary>
    /// 出站 JSON 构造器：`resources` 是动态键对象，JsonUtility 表达不了，所以手写流式构造。
    /// </summary>
    public sealed class JsonBuilder
    {
        private readonly StringBuilder _sb;
        private bool _needComma;

        public JsonBuilder(int capacity = 512)
        {
            _sb = new StringBuilder(capacity);
        }

        public JsonBuilder Obj()
        {
            Prep();
            _sb.Append('{');
            _needComma = false;
            return this;
        }

        public JsonBuilder EndObj()
        {
            _sb.Append('}');
            _needComma = true;
            return this;
        }

        public JsonBuilder Arr()
        {
            Prep();
            _sb.Append('[');
            _needComma = false;
            return this;
        }

        public JsonBuilder EndArr()
        {
            _sb.Append(']');
            _needComma = true;
            return this;
        }

        /// <summary>对象成员名。</summary>
        public JsonBuilder Key(string name)
        {
            Prep();
            Escape(name);
            _sb.Append(':');
            _needComma = false;
            return this;
        }

        public JsonBuilder Str(string value)
        {
            Prep();
            Escape(value);
            return this;
        }

        public JsonBuilder Num(double value)
        {
            Prep();
            _sb.Append(value.ToString("0.###", CultureInfo.InvariantCulture));
            return this;
        }

        public JsonBuilder Int(long value)
        {
            Prep();
            _sb.Append(value.ToString(CultureInfo.InvariantCulture));
            return this;
        }

        public JsonBuilder Bool(bool value)
        {
            Prep();
            _sb.Append(value ? "true" : "false");
            return this;
        }

        public JsonBuilder Null()
        {
            Prep();
            _sb.Append("null");
            return this;
        }

        /// <summary>直接写入一段已序列化的 JSON。</summary>
        public JsonBuilder Raw(string json)
        {
            Prep();
            _sb.Append(json);
            return this;
        }

        public JsonBuilder Field(string k, string v) { return Key(k).Str(v); }
        public JsonBuilder FieldNum(string k, double v) { return Key(k).Num(v); }
        public JsonBuilder FieldInt(string k, long v) { return Key(k).Int(v); }
        public JsonBuilder FieldBool(string k, bool v) { return Key(k).Bool(v); }
        public JsonBuilder FieldNull(string k) { return Key(k).Null(); }

        public JsonBuilder Item(string v) { return Str(v); }
        public JsonBuilder ItemNum(double v) { return Num(v); }

        public override string ToString()
        {
            return _sb.ToString();
        }

        private void Prep()
        {
            if (_needComma)
            {
                _sb.Append(',');
            }

            _needComma = true;
        }

        private void Escape(string s)
        {
            _sb.Append('"');
            if (!string.IsNullOrEmpty(s))
            {
                foreach (char c in s)
                {
                    switch (c)
                    {
                        case '"': _sb.Append("\\\""); break;
                        case '\\': _sb.Append("\\\\"); break;
                        case '\n': _sb.Append("\\n"); break;
                        case '\r': _sb.Append("\\r"); break;
                        case '\t': _sb.Append("\\t"); break;
                        default:
                            if (c < 0x20)
                            {
                                _sb.Append("\\u").Append(((int)c).ToString("x4", CultureInfo.InvariantCulture));
                            }
                            else
                            {
                                _sb.Append(c);
                            }

                            break;
                    }
                }
            }

            _sb.Append('"');
        }
    }

    /// <summary>协议中使用的消息类型名。</summary>
    public static class MsgType
    {
        public const string StateUpdate = "state_update";
        public const string Action = "action";
        public const string Config = "config";
        public const string Heartbeat = "heartbeat";
        public const string StateRequest = "state_request";
        public const string Echo = "echo";
        public const string EchoReply = "echo_reply";
        public const string Hello = "hello";
        public const string Ack = "ack";
    }

    /// <summary>常用出站消息构造。</summary>
    public static class Msg
    {
        public static string Heartbeat(long unixSeconds)
        {
            return new JsonBuilder(96)
                .Obj().Field("type", MsgType.Heartbeat).FieldInt("timestamp", unixSeconds).EndObj()
                .ToString();
        }

        public static string Hello(string instance, string version, List<string> actions)
        {
            var b = new JsonBuilder(256);
            b.Obj().Field("type", MsgType.Hello).Field("instance", instance).Field("version", version);
            b.Key("actions").Arr();
            for (int i = 0; i < actions.Count; i++)
            {
                b.Item(actions[i]);
            }

            b.EndArr().EndObj();
            return b.ToString();
        }

        public static string Ack(string action, int chefId, bool ok, string detail)
        {
            return new JsonBuilder(256)
                .Obj()
                .Field("type", MsgType.Ack)
                .Field("action", action ?? "")
                .FieldInt("chef_id", chefId)
                .FieldBool("ok", ok)
                .Field("detail", detail ?? "")
                .EndObj()
                .ToString();
        }

        public static string EchoReply(string token)
        {
            return new JsonBuilder(160)
                .Obj().Field("type", MsgType.EchoReply).Field("token", token ?? "").EndObj()
                .ToString();
        }
    }
}
