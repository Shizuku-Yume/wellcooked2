using System;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.Reflection;
using System.Text;
using HarmonyLib;
using UnityEngine;
using WellCooked.Protocol;

namespace WellCooked.Game
{
    /// <summary>
    /// 状态读取器：从运行中的厨房场景读取厨师 / 订单 / 资源状态，
    /// 按设计文档 §5.2 的 state_update schema 序列化为 JSON。
    ///
    /// 全部读取都在 Unity 主线程执行（由 Plugin.Update 按间隔节流调用）。
    /// 场景对象用 TTL 缓存降低 FindObjectsOfType 开销；私有字段一律反射读取。
    /// </summary>
    public sealed class StateReader
    {
        public Action<string> Log = delegate { };

        // ---- 缓存 TTL（秒） ----
        private const float ChefTtl = 1f;
        private const float ResourceTtl = 3f;
        private const float ScoreTtl = 2f;

        private PlayerControls[] _chefs = new PlayerControls[0];
        private float _chefsAt = -9999f;

        private ServerCookableContainer[] _containers = new ServerCookableContainer[0];
        private float _containersAt = -9999f;

        private ServerWorkstation[] _workstations = new ServerWorkstation[0];
        private float _workstationsAt = -9999f;

        private ServerPlateStackBase[] _plateStacks = new ServerPlateStackBase[0];
        private float _plateStacksAt = -9999f;

        private ScoreUIController _scoreUi;
        private float _scoreUiAt = -9999f;

        /// <summary>厨师位置采样（用于 moving/idle 判定）。</summary>
        private readonly Dictionary<int, ChefSample> _samples = new Dictionary<int, ChefSample>();

        private sealed class ChefSample
        {
            public float X;
            public float Z;
            public float Time;
        }

        /// <summary>订单控制器不是 MonoBehaviour，只能靠 patch 捕获实例。</summary>
        private static ServerOrderControllerBase _orders;

        private readonly HashSet<string> _loggedOnce = new HashSet<string>();

        // ================= 初始化 =================

        public void Init(Harmony harmony)
        {
            PatchOrderController(harmony);
        }

        private void PatchOrderController(Harmony harmony)
        {
            try
            {
                MethodInfo target = typeof(ServerOrderControllerBase).GetMethod("Update",
                    BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic,
                    null, Type.EmptyTypes, null);
                if (target == null)
                {
                    Log("[State] ServerOrderControllerBase.Update not found");
                    return;
                }
                MethodInfo postfix = typeof(StateReader).GetMethod("OrderPostfix",
                    BindingFlags.Static | BindingFlags.NonPublic);
                harmony.Patch(target, postfix: new HarmonyMethod(postfix));
                Log("[State] patched ServerOrderControllerBase.Update");
            }
            catch (Exception e)
            {
                Log("[State] patch failed: " + e.Message);
            }
        }

        private static void OrderPostfix(ServerOrderControllerBase __instance)
        {
            if (__instance != null) _orders = __instance;
        }

        // ================= 状态 JSON =================

        /// <summary>构造一帧完整的 state_update JSON（主线程调用）。</summary>
        public string BuildStateJson()
        {
            var b = new JsonBuilder(2048);
            b.Obj();
            b.Field("type", MsgType.StateUpdate);
            b.FieldInt("timestamp", DateTimeOffset.UtcNow.ToUnixTimeSeconds());
            b.FieldInt("game_time", GetGameTimeSeconds());
            b.FieldInt("score", GetScore());

            b.Key("chefs").Arr();
            List<string> chefs = BuildChefItems();
            for (int i = 0; i < chefs.Count; i++) b.Raw(chefs[i]);
            b.EndArr();

            b.Key("orders").Arr();
            List<string> orders = BuildOrderItems();
            for (int i = 0; i < orders.Count; i++) b.Raw(orders[i]);
            b.EndArr();

            b.Key("resources").Obj();
            List<KeyValuePair<string, string>> resources = BuildResourceItems();
            for (int i = 0; i < resources.Count; i++)
            {
                b.Key(resources[i].Key);
                b.Raw(resources[i].Value);
            }
            b.EndObj();

            b.EndObj();
            return b.ToString();
        }

        // ================= 计时 / 得分 =================

        private long GetGameTimeSeconds()
        {
            try
            {
                ServerOrderControllerBase orders = _orders;
                if (orders == null) return 0;
                object timer = ReadField(orders, "m_roundTimer");
                if (timer == null) return 0;
                object v = ReadProperty(timer, "TimeElapsed");
                if (v == null) return 0;
                return (long)Math.Round(Convert.ToDouble(v, CultureInfo.InvariantCulture));
            }
            catch (Exception e)
            {
                LogOnce("game_time", "[State] game_time failed: " + e.Message);
                return 0;
            }
        }

        private long GetScore()
        {
            try
            {
                float now = Time.unscaledTime;
                if (_scoreUi == null || now - _scoreUiAt > ScoreTtl)
                {
                    _scoreUi = UnityEngine.Object.FindObjectOfType<ScoreUIController>();
                    _scoreUiAt = now;
                }
                if (_scoreUi == null) return 0;

                object v = ReadField(_scoreUi, "m_totalScore");   // Int32, non-public
                if (v == null) return 0;
                return Convert.ToInt64(v, CultureInfo.InvariantCulture);
            }
            catch (Exception e)
            {
                LogOnce("score", "[State] score failed: " + e.Message);
                return 0;
            }
        }

        // ================= 厨师 =================

        private List<string> BuildChefItems()
        {
            var list = new List<string>();
            PlayerControls[] all = Cached<PlayerControls>(ref _chefs, ref _chefsAt, ChefTtl);
            float now = Time.unscaledTime;

            var byId = new List<KeyValuePair<int, PlayerControls>>();
            for (int i = 0; i < all.Length; i++)
            {
                PlayerControls c = all[i];
                if (c == null) continue;
                int id = ChefId(c);
                if (id >= 0) byId.Add(new KeyValuePair<int, PlayerControls>(id, c));
            }
            byId.Sort(delegate(KeyValuePair<int, PlayerControls> a, KeyValuePair<int, PlayerControls> c)
            {
                return a.Key.CompareTo(c.Key);
            });

            for (int i = 0; i < byId.Count; i++)
            {
                try
                {
                    list.Add(ChefJson(byId[i].Key, byId[i].Value, now));
                }
                catch (Exception e)
                {
                    LogOnce("chef" + byId[i].Key, "[State] chef read failed: " + e.Message);
                }
            }
            return list;
        }

        private string ChefJson(int id, PlayerControls controls, float now)
        {
            Vector3 p = controls.transform.position;

            string state = "idle";
            ChefSample prev;
            if (_samples.TryGetValue(id, out prev))
            {
                float dt = now - prev.Time;
                if (dt > 0.001f)
                {
                    float dx = p.x - prev.X;
                    float dz = p.z - prev.Z;
                    float speed = Mathf.Sqrt(dx * dx + dz * dz) / dt;
                    if (speed > 0.3f) state = "moving";
                }
            }
            _samples[id] = new ChefSample { X = p.x, Z = p.z, Time = now };

            string holding = ReadHolding(controls);

            var b = new JsonBuilder(160);
            b.Obj();
            b.FieldInt("id", id);
            b.Key("position").Obj().FieldNum("x", p.x).FieldNum("y", p.z).EndObj();
            if (holding == null) b.FieldNull("holding");
            else b.Field("holding", holding);
            b.Field("state", state);
            b.EndObj();
            return b.ToString();
        }

        private static readonly string[] CarrierFields = { "m_clientCarrier", "m_serverCarrier" };

        private string ReadHolding(PlayerControls controls)
        {
            for (int i = 0; i < CarrierFields.Length; i++)
            {
                object carrier = ReadField(controls, CarrierFields[i]);
                if (carrier == null) continue;
                object item = InvokeNoArg(carrier, "InspectCarriedItem");
                GameObject go = item as GameObject;
                if (go == null) continue;
                string n = Normalize(go.name);
                if (!string.IsNullOrEmpty(n)) return n;
            }
            return null;
        }

        private static int ChefId(PlayerControls controls)
        {
            if (controls == null) return -1;
            PlayerIDProvider p = ReadField(controls, "m_playerIDProvider") as PlayerIDProvider;
            return InputInjector.IdOf(p);
        }

        // ================= 订单 =================

        private List<string> BuildOrderItems()
        {
            var list = new List<string>();
            ServerOrderControllerBase orders = _orders;
            if (orders == null) return list;

            IEnumerable seq;
            try
            {
                seq = ReadField(orders, "m_activeOrders") as IEnumerable;
            }
            catch (Exception e)
            {
                LogOnce("orders", "[State] orders read failed: " + e.Message);
                return list;
            }
            if (seq == null) return list;

            int guard = 0;
            foreach (object order in seq)
            {
                if (order == null) continue;
                if (++guard > 64) break;   // 防御性上限
                try
                {
                    list.Add(OrderJson(order));
                }
                catch (Exception e)
                {
                    LogOnce("order_item", "[State] order read failed: " + e.Message);
                }
            }
            return list;
        }

        private string OrderJson(object order)
        {
            // OrderID.m_id (UInt32)
            long idNum = 0;
            object idObj = ReadField(order, "ID");
            if (idObj != null)
            {
                object mid = ReadField(idObj, "m_id");
                if (mid != null) idNum = Convert.ToInt64(mid, CultureInfo.InvariantCulture);
            }

            // RecipeListEntry(RecipeList+Entry).m_order -> OrderDefinitionNode (ScriptableObject)
            string recipe = null;
            UnityEngine.Object orderNode = null;
            object entry = ReadField(order, "RecipeListEntry");
            if (entry != null)
            {
                orderNode = ReadField(entry, "m_order") as UnityEngine.Object;
                if (orderNode != null) recipe = Normalize(orderNode.name);
            }

            // Remaining（剩余秒，Single）
            long remaining = 0;
            object rem = ReadField(order, "Remaining");
            if (rem != null)
            {
                double r = Convert.ToDouble(rem, CultureInfo.InvariantCulture);
                if (r > 0.0) remaining = (long)Math.Ceiling(r);
            }

            var b = new JsonBuilder(256);
            b.Obj();
            b.Field("id", "order_" + idNum.ToString("D3", CultureInfo.InvariantCulture));
            if (recipe == null) b.FieldNull("recipe");
            else b.Field("recipe", recipe);
            b.FieldInt("time_remaining", remaining);

            b.Key("ingredients").Arr();
            if (orderNode != null)
            {
                List<string> names = IngredientsOf(orderNode);
                for (int i = 0; i < names.Count; i++) b.Item(names[i]);
            }
            b.EndArr();

            b.EndObj();
            return b.ToString();
        }

        private List<string> IngredientsOf(UnityEngine.Object orderNode)
        {
            var names = new List<string>();
            AssembledDefinitionNode node = InvokeNoArg(orderNode, "Convert") as AssembledDefinitionNode;
            if (node != null) CollectNodeNames(node, names, 0);
            return names;
        }

        /// <summary>递归收集 Assembled 节点树里的食材名。</summary>
        private static void CollectNodeNames(AssembledDefinitionNode node, List<string> names, int depth)
        {
            if (node == null || depth > 6) return;

            // IngredientAssembledNode.m_ingriedientOrderNode（游戏源码 typo，同时容忍正确拼写）
            object ing = ReadField(node, "m_ingriedientOrderNode");
            if (ing == null) ing = ReadField(node, "m_ingredientOrderNode");
            if (ing != null)
            {
                UnityEngine.Object o = ing as UnityEngine.Object;
                if (o != null)
                {
                    string n = Normalize(o.name);
                    if (!string.IsNullOrEmpty(n)) names.Add(n);
                }
                return;
            }

            IEnumerable children = node as IEnumerable;
            if (children == null) return;
            foreach (object child in children) CollectNodeNames(child as AssembledDefinitionNode, names, depth + 1);
        }

        // ================= 资源 =================

        private List<KeyValuePair<string, string>> BuildResourceItems()
        {
            var items = new List<KeyValuePair<string, string>>();

            try { AddContainerItems(items); }
            catch (Exception e) { LogOnce("res_pot", "[State] pot read failed: " + e.Message); }

            try { AddWorkstationItems(items); }
            catch (Exception e) { LogOnce("res_station", "[State] workstation read failed: " + e.Message); }

            try { AddPlateStackItem(items); }
            catch (Exception e) { LogOnce("res_plate", "[State] plate stack read failed: " + e.Message); }

            return items;
        }

        private void AddContainerItems(List<KeyValuePair<string, string>> items)
        {
            ServerCookableContainer[] all = Cached<ServerCookableContainer>(ref _containers, ref _containersAt, ResourceTtl);
            List<ServerCookableContainer> sorted = SortByPosition(all);

            int potIdx = 0;
            int cookerIdx = 0;
            for (int i = 0; i < sorted.Count; i++)
            {
                ServerCookableContainer c = sorted[i];
                string key = IsNamed(c.gameObject, "pot") ? "pot_" + (potIdx++) : "cooker_" + (cookerIdx++);
                items.Add(new KeyValuePair<string, string>(key, ContainerJson(c)));
            }
        }

        private string ContainerJson(ServerCookableContainer container)
        {
            var names = new List<string>();
            try
            {
                ServerIngredientContainer ic = ReadField(container, "m_itemContainer") as ServerIngredientContainer;
                if (ic != null)
                {
                    AssembledDefinitionNode[] contents = ic.GetContents();
                    if (contents != null)
                        for (int i = 0; i < contents.Length; i++) CollectNodeNames(contents[i], names, 0);
                }
            }
            catch (Exception e)
            {
                LogOnce("res_contents", "[State] container contents failed: " + e.Message);
            }

            if (names.Count == 0)
            {
                var free = new JsonBuilder(64);
                free.Obj().Field("state", "free").EndObj();
                return free.ToString();
            }

            string state = "occupied";
            float timeLeft = -1f;
            try
            {
                ServerCookingHandler h = container.GetCookingHandler();
                if (h != null)
                {
                    if (h.IsBurning()) state = "burning";
                    else if (h.IsCooked()) state = "cooked";
                    else if (h.GetCookingProgress() > 0.001f)
                    {
                        state = "cooking";
                        float total = h.AccessCookingTime;
                        if (total > 0f) timeLeft = total * (1f - Mathf.Clamp01(h.GetCookingProgress()));
                    }
                }
            }
            catch (Exception e)
            {
                LogOnce("res_cook", "[State] cooking state failed: " + e.Message);
            }

            var b = new JsonBuilder(160);
            b.Obj();
            b.Field("state", state);
            b.Field("content", string.Join("+", names.ToArray()));
            if (timeLeft >= 0f) b.FieldNum("time_left", timeLeft);
            b.EndObj();
            return b.ToString();
        }

        private void AddWorkstationItems(List<KeyValuePair<string, string>> items)
        {
            ServerWorkstation[] all = Cached<ServerWorkstation>(ref _workstations, ref _workstationsAt, ResourceTtl);
            List<ServerWorkstation> sorted = SortByPosition(all);

            int boardIdx = 0;
            int wsIdx = 0;
            for (int i = 0; i < sorted.Count; i++)
            {
                ServerWorkstation w = sorted[i];
                bool board = IsNamed(w.gameObject, "chop") || IsNamed(w.gameObject, "cut");
                string key = board ? "cutting_board_" + (boardIdx++) : "workstation_" + (wsIdx++);
                items.Add(new KeyValuePair<string, string>(key, WorkstationJson(w)));
            }
        }

        private string WorkstationJson(ServerWorkstation station)
        {
            bool occupied = ReadField(station, "m_item") != null;

            var b = new JsonBuilder(128);
            b.Obj();
            b.Field("state", occupied ? "occupied" : "free");
            if (occupied)
            {
                int chef = InteracterChefId(station);
                if (chef >= 0) b.FieldInt("chef_id", chef);
            }
            b.EndObj();
            return b.ToString();
        }

        /// <summary>从 ServerWorkstation.m_interacters（non-public 嵌套类型）里找出操作厨师。</summary>
        private static int InteracterChefId(ServerWorkstation station)
        {
            IEnumerable seq = ReadField(station, "m_interacters") as IEnumerable;
            if (seq == null) return -1;
            foreach (object interacter in seq)
            {
                if (interacter == null) continue;
                GameObject go = ReadField(interacter, "m_object") as GameObject;
                if (go == null) continue;

                PlayerIDProvider p = go.GetComponent<PlayerIDProvider>();
                if (p == null) p = go.GetComponentInParent<PlayerIDProvider>();
                if (p == null) p = go.GetComponentInChildren<PlayerIDProvider>();

                int id = InputInjector.IdOf(p);
                if (id >= 0) return id;
            }
            return -1;
        }

        private void AddPlateStackItem(List<KeyValuePair<string, string>> items)
        {
            ServerPlateStackBase[] all = Cached<ServerPlateStackBase>(ref _plateStacks, ref _plateStacksAt, ResourceTtl);
            int count = 0;
            bool found = false;
            for (int i = 0; i < all.Length; i++)
            {
                ServerCleanPlateStack clean = all[i] as ServerCleanPlateStack;
                if (clean == null) continue;
                found = true;
                count += clean.GetSize();
            }
            if (!found && all.Length > 0 && all[0] != null) count = all[0].GetSize();

            var b = new JsonBuilder(64);
            b.Obj().FieldInt("count", count).EndObj();
            items.Add(new KeyValuePair<string, string>("plate_stack", b.ToString()));
        }

        // ================= 通用工具 =================

        private static T[] Cached<T>(ref T[] cache, ref float at, float ttl) where T : UnityEngine.Object
        {
            float now = Time.unscaledTime;
            if (cache == null || now - at > ttl)
            {
                cache = UnityEngine.Object.FindObjectsOfType<T>();
                at = now;
            }
            return cache;
        }

        private static List<T> SortByPosition<T>(T[] source) where T : Component
        {
            var list = new List<T>();
            if (source == null) return list;
            for (int i = 0; i < source.Length; i++)
                if (source[i] != null) list.Add(source[i]);
            list.Sort(delegate(T a, T b)
            {
                Vector3 pa = a.transform.position;
                Vector3 pb = b.transform.position;
                int c = pa.x.CompareTo(pb.x);
                return c != 0 ? c : pa.z.CompareTo(pb.z);
            });
            return list;
        }

        private static bool IsNamed(GameObject go, string token)
        {
            return go != null && go.name != null &&
                   go.name.IndexOf(token, StringComparison.OrdinalIgnoreCase) >= 0;
        }

        /// <summary>资源名规范化为 snake_case（去 (Clone)、驼峰拆词、去噪）。</summary>
        private static string Normalize(string raw)
        {
            if (string.IsNullOrEmpty(raw)) return null;
            string s = raw.Trim();

            int clone = s.IndexOf("(Clone)", StringComparison.OrdinalIgnoreCase);
            if (clone >= 0) s = s.Substring(0, clone).Trim();
            if (s.Length == 0) return null;

            var sb = new StringBuilder(s.Length + 4);
            for (int i = 0; i < s.Length; i++)
            {
                char c = s[i];
                if (char.IsUpper(c))
                {
                    if (i > 0 && (char.IsLower(s[i - 1]) || char.IsDigit(s[i - 1]))) sb.Append('_');
                    sb.Append(char.ToLowerInvariant(c));
                }
                else if (char.IsLetterOrDigit(c))
                {
                    sb.Append(char.ToLowerInvariant(c));
                }
                else
                {
                    sb.Append('_');
                }
            }

            string r = sb.ToString();
            while (r.IndexOf("__", StringComparison.Ordinal) >= 0) r = r.Replace("__", "_");
            r = r.Trim('_');
            return r.Length == 0 ? null : r;
        }

        private void LogOnce(string key, string message)
        {
            if (_loggedOnce.Add(key)) Log(message);
        }

        // ================= 反射缓存 =================

        private static readonly Dictionary<string, FieldInfo> _fields = new Dictionary<string, FieldInfo>();
        private static readonly Dictionary<string, PropertyInfo> _props = new Dictionary<string, PropertyInfo>();
        private static readonly Dictionary<string, MethodInfo> _methods = new Dictionary<string, MethodInfo>();

        private static FieldInfo FindFieldInfo(Type t, string name)
        {
            if (t == null) return null;
            string key = t.FullName + "." + name;
            FieldInfo f;
            if (_fields.TryGetValue(key, out f)) return f;
            for (Type cur = t; cur != null && f == null; cur = cur.BaseType)
                f = cur.GetField(name, BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.DeclaredOnly);
            _fields[key] = f;   // 未找到时缓存 null，避免每帧重扫
            return f;
        }

        private static object ReadField(object target, string name)
        {
            if (target == null) return null;
            try
            {
                FieldInfo f = FindFieldInfo(target.GetType(), name);
                return f == null ? null : f.GetValue(target);
            }
            catch
            {
                return null;
            }
        }

        private static object ReadProperty(object target, string name)
        {
            if (target == null) return null;
            try
            {
                Type t = target.GetType();
                string key = t.FullName + "." + name;
                PropertyInfo p;
                if (!_props.TryGetValue(key, out p))
                {
                    for (Type cur = t; cur != null && p == null; cur = cur.BaseType)
                        p = cur.GetProperty(name, BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.DeclaredOnly);
                    _props[key] = p;
                }
                return p == null ? null : p.GetValue(target, null);
            }
            catch
            {
                return null;
            }
        }

        private static object InvokeNoArg(object target, string name)
        {
            if (target == null) return null;
            Type t = target.GetType();
            string key = t.FullName + "." + name + "()";
            MethodInfo m;
            if (!_methods.TryGetValue(key, out m))
            {
                for (Type cur = t; cur != null && m == null; cur = cur.BaseType)
                    m = cur.GetMethod(name, BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic,
                        null, Type.EmptyTypes, null);
                _methods[key] = m;
            }
            if (m == null) return null;
            try
            {
                return m.Invoke(target, null);
            }
            catch
            {
                return null;
            }
        }
    }
}
