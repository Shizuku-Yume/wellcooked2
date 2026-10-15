using System;
using System.Collections.Generic;
using System.Globalization;
using System.Reflection;
using HarmonyLib;
using UnityEngine;
using WellCooked.Protocol;

namespace WellCooked.Game
{
    /// <summary>
    /// 输入注入器。
    ///
    /// 原理：Harmony Postfix 挂到 PlayerInputLookup 的取键/取值方法上，
    /// 对 (逻辑键, 玩家) 返回我们持有的稳定假对象；调度器下发 action 时
    /// 改写假对象状态，游戏自身的输入管线照常读取，从而无感地驱动厨师。
    ///
    /// 线程模型：所有状态变更只在 Unity 主线程（Plugin.Update / Tick）执行。
    /// </summary>
    public sealed class InputInjector
    {
        public const int PlayerSlots = 11;   // PlayerInputLookup.Player: One..Eleven

        // LogicalButtonID / LogicalValueID 是 non-public 嵌套枚举，只能按数值使用
        private const int BtnPickup = 1;     // LogicalButtonID.PickupAndDrop
        private const int BtnDash = 2;       // LogicalButtonID.Dash
        private const int BtnInteract = 3;   // LogicalButtonID.WorkstationInteract
        private const int AxisX = 1;         // LogicalValueID.MovementX
        private const int AxisY = 2;         // LogicalValueID.MovementY

        private const float DefaultHoldSeconds = 0.12f;
        private const float ChefCacheTtl = 1f;

        private static InputInjector _active;

        /// <summary>日志回调（由 Plugin 注入）。</summary>
        public Action<string> Log = delegate { };

        // ---- 配置 ----
        private bool _injectEnabled = true;
        private bool _cameraBasis = true;
        private bool _invertX;
        private bool _invertY;
        private float _arriveThreshold = 0.2f;
        private float _moveTimeout = 15f;

        // ---- 假对象（按玩家槽位分组） ----
        private readonly Dictionary<int, FakeButton>[] _buttons = NewSlots<FakeButton>();
        private readonly Dictionary<int, FakeValue>[] _values = NewSlots<FakeValue>();

        // ---- 主线程待办 ----
        private readonly List<MoveState> _moves = new List<MoveState>();
        private readonly List<PendingRelease> _releases = new List<PendingRelease>();

        // ---- 厨师对象缓存 ----
        private readonly Dictionary<int, PlayerControls> _chefs = new Dictionary<int, PlayerControls>();
        private float _chefCacheAt = -9999f;

        private sealed class MoveState
        {
            public int Chef;
            public Vector3 Target;      // 世界坐标 (x, 0, z)
            public float Deadline;
            public PlayerControls Controls;
        }

        private sealed class PendingRelease
        {
            public int Chef;
            public int Button;
            public float ReleaseAt;
        }

        // ================= 初始化 =================

        public void Init(Harmony harmony)
        {
            _active = this;
            PatchLookup(harmony);
        }

        public void Configure(bool cameraBasis, bool invertX, bool invertY, float arriveThreshold, float moveTimeout)
        {
            _cameraBasis = cameraBasis;
            _invertX = invertX;
            _invertY = invertY;
            if (arriveThreshold > 0f) _arriveThreshold = arriveThreshold;
            if (moveTimeout > 0f) _moveTimeout = moveTimeout;
        }

        public bool InjectEnabled
        {
            get { return _injectEnabled; }
        }

        public void SetInjectEnabled(bool enabled)
        {
            _injectEnabled = enabled;
            if (!enabled) ClearAll();
        }

        /// <summary>清零全部假对象与待办（关闭注入 / 断线 / 插件卸载时调用）。</summary>
        public void ClearAll()
        {
            _moves.Clear();
            _releases.Clear();
            for (int p = 0; p < PlayerSlots; p++)
            {
                foreach (KeyValuePair<int, FakeButton> kv in _buttons[p]) kv.Value.Reset();
                foreach (KeyValuePair<int, FakeValue> kv in _values[p]) kv.Value.SetValue(0f);
            }
        }

        // ================= patch =================

        private void PatchLookup(Harmony harmony)
        {
            MethodInfo buttonPostfix = typeof(InputInjector).GetMethod("ButtonPostfix", BindingFlags.Static | BindingFlags.NonPublic);
            MethodInfo valuePostfix = typeof(InputInjector).GetMethod("ValuePostfix", BindingFlags.Static | BindingFlags.NonPublic);

            Type lookup = AccessTools.TypeByName("PlayerInputLookup");
            if (lookup == null)
            {
                Log("[Input] PlayerInputLookup not found; injection unavailable");
                return;
            }

            int patched = 0;
            foreach (MethodInfo m in lookup.GetMethods(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static))
            {
                ParameterInfo[] ps = m.GetParameters();
                if (ps.Length < 2) continue;
                if (ps[0].ParameterType.Name != "LogicalButtonID") continue;
                if (ps[1].ParameterType.Name != "Player") continue;   // 跳过 (id, PlayerGameInput) 变体

                if (m.ReturnType == typeof(ILogicalButton))
                {
                    harmony.Patch(m, postfix: new HarmonyMethod(buttonPostfix));
                    patched++;
                }
                else if (m.ReturnType == typeof(ILogicalValue))
                {
                    harmony.Patch(m, postfix: new HarmonyMethod(valuePostfix));
                    patched++;
                }
            }
            Log("[Input] patched " + patched + " PlayerInputLookup getters");
        }

        // 枚举参数不可静态引用，改用 object[] __args 拿到装箱值
        private static void ButtonPostfix(ref ILogicalButton __result, object[] __args)
        {
            InputInjector inst = _active;
            if (inst == null || !inst._injectEnabled) return;
            if (__args == null || __args.Length < 2) return;

            int id, player;
            if (!ToInt(__args[0], out id) || !ToInt(__args[1], out player)) return;
            if (player < 0 || player >= PlayerSlots) return;
            if (id != BtnPickup && id != BtnDash && id != BtnInteract) return;   // 其余（UI 等）透传

            __result = inst.FakeButtonFor(player, id);
        }

        private static void ValuePostfix(ref ILogicalValue __result, object[] __args)
        {
            InputInjector inst = _active;
            if (inst == null || !inst._injectEnabled) return;
            if (__args == null || __args.Length < 2) return;

            int id, player;
            if (!ToInt(__args[0], out id) || !ToInt(__args[1], out player)) return;
            if (player < 0 || player >= PlayerSlots) return;
            if (id != AxisX && id != AxisY) return;

            __result = inst.FakeValueFor(player, id);
        }

        private static bool ToInt(object boxed, out int value)
        {
            value = 0;
            if (boxed == null) return false;
            try
            {
                value = Convert.ToInt32(boxed, CultureInfo.InvariantCulture);
                return true;
            }
            catch
            {
                return false;
            }
        }

        private static Dictionary<int, T>[] NewSlots<T>()
        {
            var slots = new Dictionary<int, T>[PlayerSlots];
            for (int i = 0; i < slots.Length; i++) slots[i] = new Dictionary<int, T>();
            return slots;
        }

        private FakeButton FakeButtonFor(int player, int button)
        {
            if (player < 0 || player >= PlayerSlots) return null;
            Dictionary<int, FakeButton> map = _buttons[player];
            FakeButton b;
            if (!map.TryGetValue(button, out b))
            {
                b = new FakeButton();
                map[button] = b;
            }
            return b;
        }

        private FakeValue FakeValueFor(int player, int axis)
        {
            if (player < 0 || player >= PlayerSlots) return null;
            Dictionary<int, FakeValue> map = _values[player];
            FakeValue v;
            if (!map.TryGetValue(axis, out v))
            {
                v = new FakeValue();
                map[axis] = v;
            }
            return v;
        }

        // ================= 动作 =================

        /// <summary>处理一条 action 消息（主线程）。返回 false 时 detail 说明原因。</summary>
        public bool TryHandleAction(InboundMessage m, out string detail)
        {
            detail = null;
            if (m == null || string.IsNullOrEmpty(m.action))
            {
                detail = "missing action";
                return false;
            }

            string action = m.action.Trim().ToLowerInvariant();
            int chef = m.chef_id;
            if (chef < 0 || chef >= PlayerSlots)
            {
                detail = "chef_id out of range: " + chef;
                return false;
            }

            switch (action)
            {
                case "move":
                    if (m.target == null)
                    {
                        detail = "missing target";
                        return false;
                    }
                    if (!StartMove(chef, m.target.x, m.target.y, out detail)) return false;
                    return true;

                case "stop":
                    StopMove(chef);
                    return true;

                case "press":
                case "release":
                {
                    int btn;
                    if (!TryParseButton(m.button, out btn))
                    {
                        detail = "unknown button: " + m.button;
                        return false;
                    }
                    CancelRelease(chef, btn);
                    SetDown(chef, btn, action == "press");
                    return true;
                }

                case "tap":
                {
                    int btn;
                    if (!TryParseButton(m.button, out btn))
                    {
                        detail = "unknown button: " + m.button;
                        return false;
                    }
                    ScheduleTap(chef, btn, m.hold_ms > 0 ? m.hold_ms / 1000f : DefaultHoldSeconds);
                    return true;
                }

                // 语义捷径：pickup/dash/interact 等价于对应按键的 tap
                case "pickup":
                    ScheduleTap(chef, BtnPickup, DefaultHoldSeconds);
                    return true;

                case "dash":
                    ScheduleTap(chef, BtnDash, DefaultHoldSeconds);
                    return true;

                case "interact":
                    ScheduleTap(chef, BtnInteract, DefaultHoldSeconds);
                    return true;

                default:
                    detail = "unknown action: " + action;
                    return false;
            }
        }

        private static bool TryParseButton(string name, out int button)
        {
            button = 0;
            if (string.IsNullOrEmpty(name)) return false;
            switch (name.Trim().ToLowerInvariant())
            {
                case "pickup":
                case "pick_up":
                case "pickupanddrop":
                case "pickup_and_drop":
                case "pickupdrop":
                case "1":
                    button = BtnPickup;
                    return true;
                case "dash":
                case "2":
                    button = BtnDash;
                    return true;
                case "interact":
                case "workstation":
                case "workstationinteract":
                case "3":
                    button = BtnInteract;
                    return true;
                default:
                    return false;
            }
        }

        private bool StartMove(int chef, float x, float y, out string detail)
        {
            detail = null;
            if (float.IsNaN(x) || float.IsNaN(y) || float.IsInfinity(x) || float.IsInfinity(y))
            {
                detail = "invalid target";
                return false;
            }

            PlayerControls controls = FindChef(chef, true);
            if (controls == null)
            {
                detail = "chef " + chef + " not in level";
                return false;
            }

            for (int i = _moves.Count - 1; i >= 0; i--)
                if (_moves[i].Chef == chef) _moves.RemoveAt(i);

            _moves.Add(new MoveState
            {
                Chef = chef,
                Target = new Vector3(x, 0f, y),
                Deadline = Time.unscaledTime + _moveTimeout,
                Controls = controls
            });
            return true;
        }

        private void StopMove(int chef)
        {
            for (int i = _moves.Count - 1; i >= 0; i--)
                if (_moves[i].Chef == chef) _moves.RemoveAt(i);
            SetAxis(chef, 0f, 0f);
        }

        private void ScheduleTap(int chef, int button, float holdSeconds)
        {
            CancelRelease(chef, button);
            SetDown(chef, button, true);
            _releases.Add(new PendingRelease
            {
                Chef = chef,
                Button = button,
                ReleaseAt = Time.unscaledTime + Mathf.Max(0.02f, holdSeconds)
            });
        }

        private void CancelRelease(int chef, int button)
        {
            for (int i = _releases.Count - 1; i >= 0; i--)
                if (_releases[i].Chef == chef && _releases[i].Button == button) _releases.RemoveAt(i);
        }

        private void SetDown(int chef, int button, bool down)
        {
            FakeButton b = FakeButtonFor(chef, button);
            if (b != null) b.SetDown(down);
        }

        private void SetAxis(int chef, float x, float y)
        {
            FakeValue vx = FakeValueFor(chef, AxisX);
            FakeValue vy = FakeValueFor(chef, AxisY);
            if (vx != null) vx.SetValue(x);
            if (vy != null) vy.SetValue(y);
        }

        // ================= 每帧 =================

        /// <summary>Unity 主线程每帧调用：自动释放按键、推进 move 导航。</summary>
        public void Tick()
        {
            float now = Time.unscaledTime;

            for (int i = _releases.Count - 1; i >= 0; i--)
            {
                PendingRelease r = _releases[i];
                if (now < r.ReleaseAt) continue;
                SetDown(r.Chef, r.Button, false);
                _releases.RemoveAt(i);
            }

            for (int i = _moves.Count - 1; i >= 0; i--)
            {
                MoveState mv = _moves[i];
                if (mv.Controls == null) mv.Controls = FindChef(mv.Chef, true);

                if (mv.Controls == null || now > mv.Deadline)
                {
                    SetAxis(mv.Chef, 0f, 0f);
                    _moves.RemoveAt(i);
                    continue;
                }

                Vector3 p = mv.Controls.transform.position;
                float dx = mv.Target.x - p.x;
                float dz = mv.Target.z - p.z;
                float dist = Mathf.Sqrt(dx * dx + dz * dz);

                if (dist <= _arriveThreshold)
                {
                    SetAxis(mv.Chef, 0f, 0f);
                    _moves.RemoveAt(i);
                    continue;
                }

                float ix, iy;
                WorldToInput(dx / dist, dz / dist, out ix, out iy);
                SetAxis(mv.Chef, ix, iy);
            }
        }

        /// <summary>世界方向（单位向量, xz 平面）→ 游戏输入轴值。</summary>
        private void WorldToInput(float wx, float wz, out float ix, out float iy)
        {
            if (_cameraBasis)
            {
                Camera cam = Camera.main;
                float yaw = cam != null ? cam.transform.eulerAngles.y : 0f;
                float rad = -yaw * Mathf.Deg2Rad;
                float cs = Mathf.Cos(rad);
                float sn = Mathf.Sin(rad);
                ix = wx * cs + wz * sn;
                iy = -wx * sn + wz * cs;
            }
            else
            {
                ix = wx;
                iy = wz;
            }
            if (_invertX) ix = -ix;
            if (_invertY) iy = -iy;
        }

        // ================= 厨师对象 =================

        private PlayerControls FindChef(int chefId, bool force)
        {
            float now = Time.unscaledTime;
            if (force || now - _chefCacheAt > ChefCacheTtl)
            {
                _chefs.Clear();
                PlayerControls[] all = UnityEngine.Object.FindObjectsOfType<PlayerControls>();
                for (int i = 0; i < all.Length; i++)
                {
                    int id = ChefIdOf(all[i]);
                    if (id >= 0 && !_chefs.ContainsKey(id)) _chefs[id] = all[i];
                }
                _chefCacheAt = now;
            }
            PlayerControls c;
            return _chefs.TryGetValue(chefId, out c) ? c : null;
        }

        private static FieldInfo _fPlayerIdProvider;

        private static int ChefIdOf(PlayerControls controls)
        {
            if (controls == null) return -1;
            if (_fPlayerIdProvider == null)
                _fPlayerIdProvider = typeof(PlayerControls).GetField("m_playerIDProvider",
                    BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
            if (_fPlayerIdProvider == null) return -1;

            var provider = _fPlayerIdProvider.GetValue(controls) as PlayerIDProvider;
            return IdOf(provider);
        }

        private static MethodInfo _mGetId;

        internal static int IdOf(PlayerIDProvider provider)
        {
            if (provider == null) return -1;
            if (_mGetId == null)
            {
                for (Type t = typeof(PlayerIDProvider); t != null && _mGetId == null; t = t.BaseType)
                    _mGetId = t.GetMethod("GetID", BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic,
                        null, Type.EmptyTypes, null);
            }
            if (_mGetId == null) return -1;
            try
            {
                // GetID() 返回不可见类型 Player（枚举），装箱后转 int
                object raw = _mGetId.Invoke(provider, null);
                return Convert.ToInt32(raw, CultureInfo.InvariantCulture);
            }
            catch
            {
                return -1;
            }
        }

        // ================= 假对象 =================

        /// <summary>ILogicalButton 假实现：三态语义由 down/pressClaimed/releaseClaimed 表达。</summary>
        public sealed class FakeButton : ILogicalButton
        {
            private bool _down;
            private bool _pressClaimed = true;     // 初始均已认领，避免开机即产生虚假事件
            private bool _releaseClaimed = true;
            private float _downAt;

            public bool IsDown()
            {
                return _down;
            }

            public bool JustPressed()
            {
                return _down && !_pressClaimed;
            }

            public bool JustReleased()
            {
                return !_down && !_releaseClaimed;
            }

            public bool HasUnclaimedPressEvent()
            {
                return _down && !_pressClaimed;
            }

            public void ClaimPressEvent()
            {
                _pressClaimed = true;
            }

            public bool HasUnclaimedReleaseEvent()
            {
                return !_down && !_releaseClaimed;
            }

            public void ClaimReleaseEvent()
            {
                _releaseClaimed = true;
            }

            public float GetHeldTimeLength()
            {
                return _down ? Time.unscaledTime - _downAt : 0f;
            }

            /// <summary>ILogicalElement：把自身作为根节点注册进逻辑树（游戏调试可视化用）。</summary>
            public void GetLogicTreeData(out AcyclicGraph<ILogicalElement, LogicalLinkInfo> graph,
                                         out AcyclicGraph<ILogicalElement, LogicalLinkInfo>.Node node)
            {
                graph = new AcyclicGraph<ILogicalElement, LogicalLinkInfo>(this);
                node = graph.GetNode(this);
            }

            public void SetDown(bool value)
            {
                if (value == _down) return;
                _down = value;
                if (value)
                {
                    _pressClaimed = false;
                    _releaseClaimed = true;
                    _downAt = Time.unscaledTime;
                }
                else
                {
                    _releaseClaimed = false;
                    _pressClaimed = true;
                }
            }

            /// <summary>清零但不产生事件。</summary>
            public void Reset()
            {
                _down = false;
                _pressClaimed = true;
                _releaseClaimed = true;
            }
        }

        /// <summary>ILogicalValue 假实现（单轴）。</summary>
        public sealed class FakeValue : ILogicalValue
        {
            private float _value;

            public float GetValue()
            {
                return _value;
            }

            /// <summary>ILogicalElement：把自身作为根节点注册进逻辑树（游戏调试可视化用）。</summary>
            public void GetLogicTreeData(out AcyclicGraph<ILogicalElement, LogicalLinkInfo> graph,
                                         out AcyclicGraph<ILogicalElement, LogicalLinkInfo>.Node node)
            {
                graph = new AcyclicGraph<ILogicalElement, LogicalLinkInfo>(this);
                node = graph.GetNode(this);
            }

            public void SetValue(float value)
            {
                _value = value;
            }
        }
    }
}
