using System;
using System.Collections.Generic;
using System.Reflection;
using BepInEx.Logging;
using HarmonyLib;
using InControl;
using Team17.Online;
using UnityEngine;

namespace WellCooked
{
    /// <summary>
    /// 进入关卡后自动接管（任务 game-bridge/07）。
    ///
    /// 用户自己在菜单里导航，插件做两件事：
    /// 1. 在关卡加载前把空着的玩家位用虚拟手柄占上（用户占着的那位不动），这样关卡里有多名厨师；
    /// 2. 一旦场景里出现本地厨师控制器，就开始驱动我们那几名厨师。
    ///
    /// 第一条策略是**最小策略**：每名厨师沿一条直线来回走，并周期性按一次交互键。
    /// 它只用来证明"接管 + 输入链路"通了；订单级/工序级调度算法由后续任务接进同一个驱动点。
    /// </summary>
    public static class Takeover
    {
        private const int TargetPlayers = 2;
        private const float CheckInterval = 0.5f;
        private const float WalkHalfPeriod = 5f;
        private const float ActionPeriod = 3f;
        private const float ActionLength = 0.2f;

        private static ManualLogSource _log;
        private static readonly List<Owned> OwnedPads = new List<Owned>();
        private static readonly List<VirtualPad> PendingPads = new List<VirtualPad>();
        private static float _nextCheck;
        private static bool _driving;
        private static float _drivingSince;
        private static float _nextDriveLog;
        private static float _nextConfigCheck;
        private static float _nextFrontendPress;
        private static bool _frontendHeld;

        /// <summary>`trace=true` 时每帧把喂出去的方向与厨师位置打进日志（卡住时看抖动从哪来）。</summary>
        private static bool _trace;
        private static int _arrivals;
        private static int _retargets;

        private static readonly List<int> Slots = new List<int> { 0, 1 };
        private static readonly List<PlayerInputLookup.Player> LogicalPlayers = new List<PlayerInputLookup.Player>();
        private static bool _frontendPress;

        private sealed class Owned
        {
            public string ChefName = "?";
            public int Slot;
            public int PadNumber;
            public VirtualPad Pad;
        }

        /// <summary>
        /// 参数放在游戏目录的 `BepInEx/wellcooked-takeover.txt`，改文件不用重新编译：
        /// `slots=1,2` 要占的玩家位（1 基，默认 1,2）；`frontend_press=true` 让虚拟手柄在前端也按 A
        /// （占位后加入/确认要用，但会抢菜单导航，默认关）。
        /// </summary>
        private static void ReadConfig()
        {
            var now = Time.realtimeSinceStartup;
            if (now < _nextConfigCheck)
            {
                return;
            }

            _nextConfigCheck = now + 5f;
            try
            {
                string path = System.IO.Path.Combine(BepInEx.Paths.BepInExRootPath, "wellcooked-takeover.txt");
                if (!System.IO.File.Exists(path))
                {
                    return;
                }

                foreach (var line in System.IO.File.ReadAllLines(path))
                {
                    var trimmed = line.Trim();
                    if (trimmed.Length == 0 || trimmed.StartsWith("#"))
                    {
                        continue;
                    }

                    int equals = trimmed.IndexOf('=');
                    if (equals <= 0)
                    {
                        continue;
                    }

                    var key = trimmed.Substring(0, equals).Trim().ToLowerInvariant();
                    var value = trimmed.Substring(equals + 1).Trim();
                    if (key == "slots")
                    {
                        var parsed = new List<int>();
                        foreach (var part in value.Split(','))
                        {
                            int slot;
                            if (int.TryParse(part.Trim(), out slot) && slot >= 1 && slot <= 4)
                            {
                                parsed.Add(slot - 1); // 配置里写 1 基，内部用 0 基
                            }
                        }

                        if (parsed.Count > 0
                            && string.Join(",", parsed.ConvertAll(x => x.ToString()).ToArray())
                               != string.Join(",", Slots.ConvertAll(x => x.ToString()).ToArray()))
                        {
                            Slots.Clear();
                            Slots.AddRange(parsed);
                            _log.LogInfo("takeover: 占位改成 " + string.Join(",", Slots.ConvertAll(x => (x + 1).ToString()).ToArray()));
                        }
                    }
                    else if (key == "trace")
                    {
                        _trace = value == "true" || value == "1";
                    }
                    else if (key == "frontend_press")
                    {
                        bool press = value == "true" || value == "1" || value == "yes";
                        if (press != _frontendPress)
                        {
                            _frontendPress = press;
                            _log.LogInfo("takeover: 前端按键 " + (press ? "开" : "关"));
                        }
                    }
                }
            }
            catch (Exception e)
            {
                _log.LogWarning("takeover: 读配置失败: " + e.Message);
            }
        }

        public static void Install(ManualLogSource log)
        {
            _log = log;

            var update = AccessTools.Method(typeof(PlayerInputLookup), "Update", Type.EmptyTypes);
            if (update == null)
            {
                _log.LogError("takeover: 找不到 PlayerInputLookup.Update，接管检查挂不上");
                return;
            }

            var postfix = new HarmonyMethod(typeof(Takeover).GetMethod(
                nameof(Tick), BindingFlags.Static | BindingFlags.NonPublic));
            new Harmony("wellcooked.takeover").Patch(update, postfix: postfix);
            _log.LogInfo("takeover: 每帧检查已挂上，占位 " + string.Join(",", Slots.ConvertAll(x => (x + 1).ToString()).ToArray()));
        }

        private static void Tick()
        {
            var now = Time.realtimeSinceStartup;
            Commands.Tick();
            WorldState.Tick(now);
            if (now < _nextCheck)
            {
                return;
            }

            _nextCheck = now + CheckInterval;
            try
            {
                Step(now);
            }
            catch (Exception e)
            {
                _log.LogError("takeover: " + e);
            }
        }

        private static void Step(float now)
        {
            if (!InputManager.IsSetup || !PlayerInputLookup.IsAwake())
            {
                return;
            }

            ReadConfig();

            // 设备一直挂；但**前端里不抢着占位**：游戏的"按 A 加入"靠 GetEngagementPad
            // 找一个还没占位、刚按下的 pad（反编译证据 StartScreenFlow.CheckForEngagement），
            // 我们提前占满会让标题画面永远等不到，卡死在那儿。
            bool inLevel = UnityEngine.Object.FindObjectsOfType<PlayerControls>().Length > 0;
            EnsurePads();
            ResolvePendingPads(inLevel);

            // 关卡开始的判定：场景里出现本地厨师控制器。前端没有它，所以不会误触发。
            var chefs = UnityEngine.Object.FindObjectsOfType<PlayerControls>();
            if (chefs.Length > 0 && !_driving)
            {
                StartDriving(chefs.Length, now);
            }
            else if (chefs.Length == 0 && _driving)
            {
                StopDriving();
            }

            if (_driving)
            {
                Drive(now);
            }
            else if (_frontendPress)
            {
                PressForJoin(now);
            }
        }

        /// <summary>
        /// 前端里替我们占的位按 A（加入/确认要用）。默认关：它会抢菜单导航。
        /// </summary>
        private static void PressForJoin(float now)
        {
            if (now < _nextFrontendPress)
            {
                return;
            }

            // 必须"按下→松开"交替：游戏只认按下这个边沿，一直按着的话只有第一次算数，
            // 第一次又常常发生在标题画面就绪之前，于是画面永远停在标题页。
            _nextFrontendPress = now + 2f;
            _frontendHeld = !_frontendHeld;
            int count = 0;
            foreach (var owned in OwnedPads)
            {
                owned.Pad.Want(_frontendHeld, false, Vector2.zero);
                count++;
            }

            foreach (var pad in PendingPads)
            {
                pad.Want(_frontendHeld, false, Vector2.zero);
                count++;
            }

            _log.LogInfo("takeover: 前端 A " + (_frontendHeld ? "按下" : "松开") + "（" + count + " 个手柄）");
        }

        /// <summary>保证每个配置的玩家位都有一个我们的虚拟手柄（挂了就等游戏枚举）。</summary>
        private static void EnsurePads()
        {
            foreach (int slot in Slots)
            {
                if (HasPadFor(slot))
                {
                    continue;
                }

                var pad = new VirtualPad("WellCooked Pad " + (slot + 1), slot);
                InputManager.AttachDevice(pad);
                PendingPads.Add(pad);
                _log.LogInfo("takeover: 挂了虚拟手柄（玩家位 " + (slot + 1) + "），等游戏把它枚举出来");
                return; // 一次只挂一个，剩下的下一次检查再说
            }
        }

        /// <summary>这个玩家位是否已经有我们的手柄（已占位或在等枚举）。</summary>
        /// <summary>玩家位的显示名，和游戏里 PlayerID 的取值顺序一致。</summary>
        private static string SlotName(int slot)
        {
            string[] names = { "One", "Two", "Three", "Four" };
            return slot >= 0 && slot < names.Length ? names[slot] : ("#" + slot);
        }

        private static bool HasPadFor(int slot)
        {
            foreach (var owned in OwnedPads)
            {
                if (owned.Slot == slot)
                {
                    return true;
                }
            }

            foreach (var pad in PendingPads)
            {
                if (pad.Slot == slot)
                {
                    return true;
                }
            }

            return false;
        }

        /// <summary>
        /// 刚挂上的设备要等游戏自己枚举一轮才拿得到 pad 号。
        /// 拿到之后：游戏要是已经自己把它加入了（前端按 A 那条路），直接认下来；
        /// 否则在关卡里补位（游戏自己那条路走不通时用）。
        /// </summary>
        private static void ResolvePendingPads(bool inLevel)
        {
            if (PendingPads.Count == 0)
            {
                return;
            }

            var manager = GameUtils.RequestManager<PlayerManager>() as PCPlayerManager;
            if (manager == null)
            {
                return;
            }

            for (int i = PendingPads.Count - 1; i >= 0; i--)
            {
                var pad = PendingPads[i];
                int padNumber = FindPadNumber(pad);
                if (padNumber < 0)
                {
                    continue; // 游戏还没枚举到，下一轮再看
                }

                // 游戏自己已经把这只手柄加入了（pad 号即玩家位号）
                if (manager.GetUser((EngagementSlot)padNumber) != null)
                {
                    OwnedPads.Add(new Owned
                    {
                        Slot = padNumber,
                        PadNumber = padNumber,
                        Pad = pad,
                        ChefName = SlotName(padNumber)
                    });
                    PendingPads.RemoveAt(i);
                    _log.LogInfo("takeover: 游戏已自己加入 pad " + padNumber + " → 玩家位 " + (padNumber + 1));
                    continue;
                }

                if (!inLevel)
                {
                    continue; // 前端里继续靠按 A 让游戏自己加入
                }

                int slot = FirstFreeSlot(manager);
                if (slot < 0)
                {
                    PendingPads.RemoveAt(i);
                    continue;
                }

                var engagement = (EngagementSlot)slot;
                // EngagePadToSlot 是 protected，和 supercharged 一样走反射
                var engage = AccessTools.Method(manager.GetType(), "EngagePadToSlot",
                    new[] { typeof(ControlPadInput.PadNum), typeof(EngagementSlot) });
                if (engage == null)
                {
                    _log.LogError("takeover: 找不到 PCPlayerManager.EngagePadToSlot");
                    return;
                }

                object profile = engage.Invoke(manager, new object[] { (ControlPadInput.PadNum)padNumber, engagement });
                if (profile == null || manager.GetUser(engagement) == null)
                {
                    _log.LogWarning("takeover: 游戏拒绝了玩家位 " + (slot + 1) + " 的占位（pad " + padNumber + "）");
                    PendingPads.RemoveAt(i);
                    continue;
                }

                OwnedPads.Add(new Owned
                {
                    Slot = slot,
                    PadNumber = padNumber,
                    Pad = pad,
                    ChefName = SlotName(slot)
                });
                PendingPads.RemoveAt(i);
                _log.LogInfo("takeover: 已占玩家位 " + (slot + 1) + "（pad " + padNumber + "）");
                RegenerateControls();
            }
        }

        /// <summary>
        /// 让游戏按新的玩家/手柄绑定重建输入映射。占位只是改了用户表，
        /// 玩家的 ControlSchemeData 不一定跟着更新，所以显式叫它重建一次。
        /// </summary>
        private static void RegenerateControls()
        {
            var lookup = UnityEngine.Object.FindObjectOfType(typeof(PlayerInputLookup));
            if (lookup == null)
            {
                _log.LogWarning("takeover: 找不到 PlayerInputLookup 实例，跳过重建输入映射");
                return;
            }

            var regenerate = AccessTools.Method(lookup.GetType(), "RegenerateControls", Type.EmptyTypes);
            if (regenerate == null)
            {
                _log.LogWarning("takeover: 找不到 RegenerateControls");
                return;
            }

            try
            {
                regenerate.Invoke(lookup, null);
                _log.LogInfo("takeover: 已让游戏重建输入映射");
            }
            catch (Exception e)
            {
                _log.LogWarning("takeover: 重建输入映射失败: " + e.Message);
            }
        }

        private static int FirstFreeSlot(PCPlayerManager manager)
        {
            foreach (int slot in Slots)
            {
                if (manager.GetUser((EngagementSlot)slot) == null && !IsHandled(slot))
                {
                    return slot;
                }
            }

            return -1;
        }

        private static bool IsHandled(int slot)
        {
            foreach (var owned in OwnedPads)
            {
                if (owned.Slot == slot)
                {
                    return true;
                }
            }

            return false;
        }

        /// <summary>和 supercharged 一样的做法：靠 PCPadInputProvider 的 action set 反查 pad 号。</summary>
        private static int FindPadNumber(InputDevice device)
        {
            var lookup = AccessTools.Method(typeof(PCPadInputProvider), "GetActionSet",
                new[] { typeof(ControlPadInput.PadNum) });
            if (lookup == null)
            {
                _log.LogError("takeover: 找不到 PCPadInputProvider.GetActionSet");
                return -1;
            }

            int maximum = PlayerInputLookup.GetSystemControllerMaximum();
            for (int pad = 0; pad < maximum; pad++)
            {
                var actions = lookup.Invoke(null, new object[] { (ControlPadInput.PadNum)pad }) as PlayerActionSet;
                if (actions != null && actions.Device == device)
                {
                    return pad;
                }
            }

            return -1;
        }

        private static void StartDriving(int chefCount, float now)
        {
            _driving = true;
            _drivingSince = now;
            _log.LogInfo("takeover: 检测到 " + chefCount + " 名厨师，开始接管驱动（我们占了 " + OwnedPads.Count + " 位）");
            ChooseLogicalPlayers(UnityEngine.Object.FindObjectsOfType<PlayerControls>());
            CollectTargets();
            _log.LogInfo("takeover: 系统手柄槽位数=" + PlayerInputLookup.GetSystemControllerMaximum());
            LogDevices();
            LogPadInventory();
            LogUsers();
        }

        /// <summary>
        /// 挑出设备层管不到的玩家（典型是键盘占住的 1 号位），改成逻辑层注入驱动。
        /// 设备层只能写进我们自己的虚拟手柄，键盘占的位写不进去——截图实证键盘会抢 1 号位。
        /// </summary>
        private static void ChooseLogicalPlayers(PlayerControls[] chefs)
        {
            LogicalPlayers.Clear();
            foreach (var chef in chefs)
            {
                var provider = chef.GetComponent<PlayerIDProvider>();
                if (provider == null || !provider.IsLocallyControlled())
                {
                    continue;
                }

                var player = provider.GetID();
                int pad;
                try
                {
                    pad = (int)PlayerInputLookup.GetPadForPlayer(player);
                }
                catch (Exception)
                {
                    pad = -1;
                }

                bool ours = false;
                foreach (var owned in OwnedPads)
                {
                    if (owned.PadNumber == pad)
                    {
                        ours = true;
                    }
                }

                if (!ours)
                {
                    LogicalPlayers.Add(player);
                }
            }

            if (LogicalPlayers.Count == 0)
            {
                _log.LogInfo("takeover: 所有本地玩家都在我们的手柄上，不需要逻辑层注入");
                return;
            }

            var names = new List<string>();
            foreach (var player in LogicalPlayers)
            {
                names.Add(player.ToString());
            }

            _log.LogInfo("takeover: 逻辑层接管玩家 " + string.Join(",", names.ToArray())
                         + "（他们的设备不是我们的手柄）");
        }

        /// <summary>InControl 认得的所有设备：键盘、物理手柄、我们挂的虚拟手柄都在这里。</summary>
        private static void LogDevices()
        {
            try
            {
                foreach (var device in InputManager.Devices)
                {
                    bool ours = false;
                    foreach (var owned in OwnedPads)
                    {
                        if (ReferenceEquals(owned.Pad, device))
                        {
                            ours = true;
                        }
                    }

                    _log.LogInfo("takeover: 设备 " + device.Name + " 附加=" + device.IsAttached
                                 + (ours ? "（我们的）" : ""));
                }
            }
            catch (Exception e)
            {
                _log.LogWarning("takeover: 列设备失败: " + e.Message);
            }
        }

        /// <summary>客户端用户表：每个玩家位上是哪个用户、是否本地。</summary>
        private static void LogUsers()
        {
            try
            {
                var users = ClientUserSystem.m_Users;
                for (int i = 0; i < users.Count; i++)
                {
                    var user = users._items[i];
                    _log.LogInfo("takeover: 用户 " + (i + 1) + " 玩家位=" + user.Engagement
                                 + " 本地=" + user.IsLocal + " 半边=" + user.PadSide
                                 + " 状态=" + user.GameState);
                }
            }
            catch (Exception e)
            {
                _log.LogWarning("takeover: 列用户失败: " + e.Message);
            }
        }

        /// <summary>
        /// 把游戏认得的手柄槽位逐个列出来：哪个槽是我们的、还有别的什么设备在里面。
        /// 1P 不动时，这份清单能区分"我们的手柄没占住槽"和"占了槽但游戏没读它"。
        /// </summary>
        private static void LogPadInventory()
        {
            var lookup = AccessTools.Method(typeof(PCPadInputProvider), "GetActionSet",
                new[] { typeof(ControlPadInput.PadNum) });
            if (lookup == null)
            {
                _log.LogWarning("takeover: 拿不到 PCPadInputProvider.GetActionSet，槽位清单跳过");
                return;
            }

            int maximum = PlayerInputLookup.GetSystemControllerMaximum();
            for (int pad = 0; pad < maximum; pad++)
            {
                object actions = null;
                try
                {
                    actions = lookup.Invoke(null, new object[] { (ControlPadInput.PadNum)pad });
                }
                catch (Exception e)
                {
                    _log.LogWarning("takeover: pad 槽 " + pad + " 取不到: " + e.Message);
                    continue;
                }

                var set = actions as PlayerActionSet;
                var device = set == null ? null : set.Device;
                bool ours = false;
                foreach (var owned in OwnedPads)
                {
                    if (ReferenceEquals(owned.Pad, device))
                    {
                        ours = true;
                    }
                }

                _log.LogInfo("takeover: pad 槽 " + pad + " → "
                             + (device == null ? "空" : device.Name + (ours ? "（我们的）" : "（不是我们的）")));
            }
        }

        private static void StopDriving()
        {
            _driving = false;
            foreach (var owned in OwnedPads)
            {
                owned.Pad.Want(false, false, Vector2.zero);
            }

            LogicalInput.Clear();

            _log.LogInfo("takeover: 离开关卡，停止驱动");
        }

        /// <summary>
        /// 最小策略：来回走 + 周期按交互键。
        /// 后续任务把这里换成调度器给出的动作，其余（占位、接管、帧同步）都不用改。
        /// </summary>
        private static void Drive(float now)
        {
            float elapsed = now - _drivingSince;
            bool action = elapsed % ActionPeriod < ActionLength;
            ScanChefPositions(now);
            if (now >= _nextTargetScan)
            {
                _nextTargetScan = now + 3f;
                CollectTargets();
            }

            foreach (var owned in OwnedPads)
            {
                Vector2 position;
                if (!TryPosition(owned.Slot, out position))
                {
                    continue;
                }

                if (!Navigation.HasArrived(owned.Slot))
                {
                    string status;
                    var direction = Navigation.Steer(owned.Slot, position, now, out status);
                    owned.Pad.Want(action, false, direction);
                    if (_trace)
                    {
                        _log.LogInfo("trace 玩家位=" + (owned.Slot + 1)
                                     + " 方向=(" + direction.x.ToString("F2") + "," + direction.y.ToString("F2") + ")"
                                     + " 位置=(" + position.x.ToString("F3") + "," + position.y.ToString("F3") + ")"
                                     + " " + status);
                    }

                    if (now >= _nextDriveLog)
                    {
                        _log.LogInfo("takeover: 走位 玩家位=" + (owned.Slot + 1) + " 厨师=" + owned.ChefName + " " + status
                                     + " 方向=(" + direction.x.ToString("F2") + "," + direction.y.ToString("F2") + ")"
                                     + " 位置=(" + position.x.ToString("F2") + "," + position.y.ToString("F2") + ")");
                    }

                    if (direction == Vector2.zero)
                    {
                        AssignNextTarget(owned.Slot, position, now);
                    }
                }
                else
                {
                    // 到达是"走到路径终点"就成立，而走位日志每 2 秒才一行，正好会错过它。
                    // 所以到达单独计数、单独打日志，别让它看起来像"0 次到达"。
                    _arrivals++;
                    _log.LogInfo("takeover: 到达 玩家位=" + (owned.Slot + 1)
                                 + " 位置=(" + position.x.ToString("F2") + "," + position.y.ToString("F2") + ")");
                    AssignNextTarget(owned.Slot, position, now);
                }
            }

            foreach (var player in LogicalPlayers)
            {
                Vector2 position;
                if (!TryPosition((int)player, out position))
                {
                    continue;
                }

                string status;
                var direction = Navigation.Steer(player, position, now, out status);
                LogicalInput.Set(player, direction, action);
                if (direction == Vector2.zero)
                {
                    AssignNextTarget(player, position, now);
                }
            }

            if (now >= _nextDriveLog)
            {
                _nextDriveLog = now + 2f;
                _log.LogInfo("takeover: 驱动中 交互=" + action
                             + " 设备层=" + OwnedPads.Count + " 逻辑层=" + LogicalPlayers.Count
                             + " 目标点=" + Targets.Count + " 到达次数=" + _arrivals + " 换目标次数=" + _retargets);
                foreach (var owned in OwnedPads)
                {
                    _log.LogInfo("takeover: 手柄 玩家位=" + (owned.Slot + 1) + " pad=" + owned.PadNumber
                                 + " 想要 " + owned.Pad.Wanted + " | " + owned.Pad.State);
                }

                LogChefs();
            }
        }

        // ---- 导航：目标点由这里挑，走位交给 Navigation ----

        private static readonly List<Vector2> Targets = new List<Vector2>();
        private static readonly Dictionary<object, int> TargetIndex = new Dictionary<object, int>();
        private static readonly Dictionary<int, Vector3> ChefPositions = new Dictionary<int, Vector3>();
        private static float _nextChefScan;
        private static float _nextTargetScan;
        private static int _targetCursor;

        /// <summary>目标点取真实工位的位置：食材箱、出菜台、灶、水槽。调度器以后会自己决定去哪，这里先轮着走。</summary>
        private static void CollectTargets()
        {
            // 工位不是一开始就全在场上：用 GameUtils.LoadLevel 进关时，"厨师出现"那一刻
            // 工位还没生成完，只在这一刻采一次会得到 0 个目标（实测 目标点=0、厨师不动）。
            // 所以驱动期间每 3 秒重采一次，数量变了才打日志。
            int previous = Targets.Count;
            Targets.Clear();
            foreach (var typeName in new[]
                     {
                         "ClientIngredientContainer", "ClientPlateStation", "ClientCookingStation",
                         "ClientWashingStation", "ClientCookableContainer"
                     })
            {
                var type = AccessTools.TypeByName(typeName);
                if (type == null)
                {
                    continue;
                }

                foreach (var found in UnityEngine.Object.FindObjectsOfType(type))
                {
                    var component = found as Component;
                    if (component != null)
                    {
                        var p = component.transform.position;
                        Targets.Add(new Vector2(p.x, p.z));
                    }
                }
            }

            if (Targets.Count != previous)
            {
                _log.LogInfo("takeover: 收集到 " + Targets.Count + " 个目标点（工位）");
            }
        }

        private static void ScanChefPositions(float now)
        {
            if (now < _nextChefScan)
            {
                return;
            }

            _nextChefScan = now + 0.1f;
            ChefPositions.Clear();
            foreach (var chef in UnityEngine.Object.FindObjectsOfType<PlayerControls>())
            {
                var provider = chef.GetComponent<PlayerIDProvider>();
                if (provider != null)
                {
                    ChefPositions[(int)provider.GetID()] = chef.transform.position;
                }
            }
        }

        private static bool TryPosition(int playerIndex, out Vector2 position)
        {
            Vector3 p;
            if (ChefPositions.TryGetValue(playerIndex, out p))
            {
                position = new Vector2(p.x, p.z);
                return true;
            }

            position = Vector2.zero;
            return false;
        }

        /// <summary>给这个执行体排下一个目标点；到达后就换下一个，保持可见的移动。</summary>
        private static void AssignNextTarget(object owner, Vector2 position, float now)
        {
            if (Targets.Count == 0)
            {
                return;
            }

            int index;
            TargetIndex.TryGetValue(owner, out index);

            Vector2 current;
            bool hasCurrent = Navigation.TryTarget(owner, out current);

            // 挑一个"离当前位置至少 3 格、且不是当前目标"的点。
            // 必须排除当前目标：到达后 Drive 会再来要下一个目标，如果又挑回同一个，
            // Navigation.Assign 会因为"目标没变"直接返回、Arrived 一直是 true，
            // 于是每帧都换目标、每帧都把路径清零，厨师就永远走不出去
            // （实测 换目标次数每秒 2 次、到达 0 次、坐标不动）。
            int chosen = -1;
            float farthest = -1f;
            int farthestIndex = -1;
            for (int attempt = 0; attempt < Targets.Count; attempt++)
            {
                index = (index + 1) % Targets.Count;
                float distance = Vector2.Distance(position, Targets[index]);
                if (hasCurrent && (Targets[index] - current).sqrMagnitude < 0.01f)
                {
                    continue;
                }

                if (distance > farthest)
                {
                    farthest = distance;
                    farthestIndex = index;
                }

                if (distance > 3f)
                {
                    chosen = index;
                    break;
                }
            }

            if (chosen < 0)
            {
                chosen = farthestIndex;
            }

            if (chosen < 0)
            {
                return;
            }

            TargetIndex[owner] = chosen;
            _retargets++;
            Navigation.Assign(owner, Targets[chosen]);
        }

        /// <summary>
        /// 客观证据：把每名厨师的位置、玩家号、是否本地控制、绑在哪个手柄上打出来。
        /// 只看"我们想按什么"会骗人——写不进游戏时日志一模一样。
        /// </summary>
        private static void LogChefs()
        {
            var chefs = UnityEngine.Object.FindObjectsOfType<PlayerControls>();
            foreach (var chef in chefs)
            {
                var provider = chef.GetComponent<PlayerIDProvider>();
                string id = provider == null ? "?" : provider.GetID().ToString();
                string local = provider == null ? "?" : provider.IsLocallyControlled().ToString();
                string pad = "?";
                try
                {
                    if (provider != null)
                    {
                        pad = PlayerInputLookup.GetPadForPlayer(provider.GetID()).ToString();
                    }
                }
                catch (Exception e)
                {
                    pad = "取不到(" + e.GetType().Name + ")";
                }

                var p = chef.transform.position;
                _log.LogInfo("takeover: 厨师 玩家=" + id + " 本地=" + local + " 手柄=" + pad
                             + " 位置=(" + p.x.ToString("F2") + "," + p.y.ToString("F2") + ")");
            }

            _log.LogInfo("takeover: 用户 客户端=" + ClientUserSystem.m_Users.Count
                         + " 服务端=" + ServerUserSystem.m_Users.Count);

            // 回合计时开始的条件之一是每个用户的状态到了 RunKitchen 之后；
            // 卡在更早的状态就说明"占位"没走完游戏自己的用户注册流程。
            try
            {
                var server = ServerUserSystem.m_Users;
                for (int i = 0; i < server.Count; i++)
                {
                    _log.LogInfo("takeover: 服务端用户 " + (i + 1) + " 玩家位=" + server._items[i].Engagement
                                 + " 状态=" + server._items[i].GameState);
                }
            }
            catch (Exception e)
            {
                _log.LogWarning("takeover: 读服务端用户失败: " + e.Message);
            }

            try
            {
                var config = PlayerInputLookup.GetInputConfig();
                if (config != null)
                {
                    foreach (PlayerInputLookup.Player player in new[] { PlayerInputLookup.Player.One, PlayerInputLookup.Player.Two })
                    {
                        var data = config.GetInputData(player);
                        _log.LogInfo("takeover: 输入映射 玩家=" + player + " → "
                                     + (data == null ? "无" : data.ToString()));
                    }
                }
            }
            catch (Exception e)
            {
                _log.LogWarning("takeover: 读输入映射失败: " + e.Message);
            }
        }
    }
}
