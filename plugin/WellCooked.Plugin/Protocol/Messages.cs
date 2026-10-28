using System;
using System.Collections.Generic;
using System.Globalization;
using System.Text;

namespace WellCooked.Protocol
{
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
        // JsonUtility（游戏内 2017.4 运行时）解析不了嵌套对象，move 的坐标额外给一份扁平字段。
        public bool has_target_xy;
        public float target_x;
        public float target_y;
        public string target_object;
        public string token;
        public int state_interval_ms;
        public string input_mode;
        public string strategy;
    }

    /// <summary>
    /// 出站 JSON 构造器。
    /// 出站消息必须精确对齐设计文档 §5.2 的 schema（含 resources 动态键对象），
    /// JsonUtility 无法表达，因此手写流式构造。
    /// </summary>
    public sealed class JsonBuilder
    {
        private readonly StringBuilder _sb;
        private bool _needComma;

        public JsonBuilder(int capacity = 512)
        {
            _sb = new StringBuilder(capacity);
        }

        // ---- 结构 ----

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

        // ---- 值 ----

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

        // ---- 便捷字段 ----

        public JsonBuilder Field(string k, string v) { return Key(k).Str(v); }
        public JsonBuilder FieldNum(string k, double v) { return Key(k).Num(v); }
        public JsonBuilder FieldInt(string k, long v) { return Key(k).Int(v); }
        public JsonBuilder FieldBool(string k, bool v) { return Key(k).Bool(v); }
        public JsonBuilder FieldNull(string k) { return Key(k).Null(); }

        /// <summary>数组元素（字符串）。</summary>
        public JsonBuilder Item(string v) { return Str(v); }

        /// <summary>数组元素（数字）。</summary>
        public JsonBuilder ItemNum(double v) { return Num(v); }

        public override string ToString()
        {
            return _sb.ToString();
        }

        // ---- 内部 ----

        private void Prep()
        {
            if (_needComma) _sb.Append(',');
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
                            if (c < 0x20) _sb.Append("\\u").Append(((int)c).ToString("x4", CultureInfo.InvariantCulture));
                            else _sb.Append(c);
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

        public static string Hello(string instance, string version, IEnumerable<string> actions)
        {
            var b = new JsonBuilder(256);
            b.Obj().Field("type", MsgType.Hello).Field("instance", instance).Field("version", version);
            b.Key("actions").Arr();
            foreach (string a in actions) b.Item(a);
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
