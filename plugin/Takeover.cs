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
        private const float WalkHalfPeriod = 2f;
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

        private static int _targetPlayers = 2;
        private static bool _frontendPress;

        private sealed class Owned
        {
            public int Slot;
            public int PadNumber;
            public VirtualPad Pad;
        }

        /// <summary>
        /// 参数放在游戏目录的 `BepInEx/wellcooked-takeover.txt`，改文件不用重新编译：
        /// `players=N` 目标玩家数（含用户自己占的那位）；`frontend_press=true` 让虚拟手柄在前端也按 A
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
                    if (key == "players")
                    {
                        int players;
                        if (int.TryParse(value, out players) && players >= 1 && players <= 4 && players != _targetPlayers)
                        {
                            _targetPlayers = players;
                            _log.LogInfo("takeover: 目标玩家数改成 " + players);
                        }
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
            _log.LogInfo("takeover: 每帧检查已挂上，目标玩家数 " + TargetPlayers);
        }

        private static void Tick()
        {
            var now = Time.realtimeSinceStartup;
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
            EnsurePads();
            ResolvePendingPads();

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

            _nextFrontendPress = now + 3f;
            bool press = true;
            foreach (var owned in OwnedPads)
            {
                owned.Pad.Want(press, false, Vector2.zero);
            }

            _log.LogInfo("takeover: 前端按 A 帮 " + OwnedPads.Count + " 个占位加入/确认");
        }

        /// <summary>把空着的玩家位占上；用户自己占着的那位不动。</summary>
        private static void EnsurePads()
        {
            var manager = GameUtils.RequestManager<PlayerManager>() as PCPlayerManager;
            if (manager == null)
            {
                return;
            }

            for (int slot = 0; slot < TargetPlayers; slot++)
            {
                var engagement = (EngagementSlot)slot;
                if (manager.GetUser(engagement) != null || IsHandled(slot))
                {
                    continue;
                }

                var pad = new VirtualPad("WellCooked Pad " + (slot + 1), slot);
                InputManager.AttachDevice(pad);
                PendingPads.Add(pad);
                _log.LogInfo("takeover: 挂了虚拟手柄准备占玩家位 " + (slot + 1) + "，等游戏把它枚举出来");
                return; // 一次只处理一位，剩下的下一次检查再说
            }
        }

        /// <summary>刚挂上的设备要等游戏自己枚举一轮才拿得到 pad 号。</summary>
        private static void ResolvePendingPads()
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

                OwnedPads.Add(new Owned { Slot = slot, PadNumber = padNumber, Pad = pad });
                PendingPads.RemoveAt(i);
                _log.LogInfo("takeover: 已占玩家位 " + (slot + 1) + "（pad " + padNumber + "）");
            }
        }

        private static int FirstFreeSlot(PCPlayerManager manager)
        {
            for (int slot = 0; slot < TargetPlayers; slot++)
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
        }

        private static void StopDriving()
        {
            _driving = false;
            foreach (var owned in OwnedPads)
            {
                owned.Pad.Want(false, false, Vector2.zero);
            }

            _log.LogInfo("takeover: 离开关卡，停止驱动");
        }

        /// <summary>
        /// 最小策略：来回走 + 周期按交互键。
        /// 后续任务把这里换成调度器给出的动作，其余（占位、接管、帧同步）都不用改。
        /// </summary>
        private static void Drive(float now)
        {
            float elapsed = now - _drivingSince;
            float direction = elapsed % (WalkHalfPeriod * 2f) < WalkHalfPeriod ? 1f : -1f;
            bool action = elapsed % ActionPeriod < ActionLength;

            foreach (var owned in OwnedPads)
            {
                owned.Pad.Want(action, false, new Vector2(direction, 0f));
            }

            if (now >= _nextDriveLog)
            {
                _nextDriveLog = now + 2f;
                _log.LogInfo("takeover: 驱动中 方向=" + direction + " 交互=" + action
                             + " | " + VirtualPad.Diagnostics);
                LogChefs();
            }
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
        }
    }
}
