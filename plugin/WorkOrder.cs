using System;
using System.Collections.Generic;
using System.Linq;
using BepInEx.Logging;
using UnityEngine;

namespace WellCooked
{
    /// <summary>
    /// 工序状态机（任务 09）：按当前订单走完 取盘子 → 取料装盘 → 交付。
    ///
    /// 它只做"执行面"：给定订单，把动作一步步做出来并**确认结果**（手上多了什么、
    /// 得分有没有涨），做不成就报出来。谁决定"哪个厨师做哪一单"是 Scheduler 的事——
    /// 现在这一段由这里的 `PickChef` 替身代管，等 Bridge（任务 02/03）落地后换成外部下发。
    ///
    /// 每步都是：设目标点 → 等到达 → 按一次交互（A 键）→ 等世界状态出现预期变化 → 下一步。
    /// 每步带超时，超时报失败并重试，不静默卡住。
    /// </summary>
    public static class WorkOrder
    {
        private enum Step
        {
            Idle,
            GetPlate,
            GetIngredient,
            Deliver,
            Done
        }

        private static ManualLogSource _log;
        private static Step _step = Step.Idle;
        private static int _slot = -1;
        private static float _stepStart;
        private static float _interactAt;
        private static float _pressUntil;
        private static bool _pressing;
        private static bool _interacted;
        private static string _expectHeld = "";
        private static Vector2 _target;
        private static bool _hasTarget;
        private static int _completed;
        private static string _lastError = "";

        /// <summary>每一步的超时（秒）。到点没做成就报失败、重来。</summary>
        private const float StepTimeout = 20f;

        /// <summary>到达判定半径：工位都在台面里，厨师只能站到旁边一格。</summary>
        private const float ReachRadius = 1.3f;

        /// <summary>交互按下的时长（秒）。游戏只认按下的边沿，必须松开。</summary>
        private const float InteractHold = 0.25f;

        public static void Install(ManualLogSource log)
        {
            _log = log;
        }

        public static int CompletedOrders
        {
            get { return _completed; }
        }

        /// <summary>这一步要哪个厨师、去哪、要不要按交互。驱动层照这个喂输入。</summary>
        public static bool DesireFor(int slot, out Vector2 target, out bool interact)
        {
            target = _target;
            interact = _slot == slot && _pressing;
            return _slot == slot && _hasTarget;
        }

        public static string Status
        {
            get
            {
                return _step == Step.Idle
                    ? (_lastError.Length > 0 ? "空闲（上次：" + _lastError + "）" : "空闲")
                    : _step + " 厨师位=" + (_slot + 1) + " 目标=" + _target.x.ToString("F1") + ","
                      + _target.y.ToString("F1") + " 已交互=" + _interacted + " 完成单数=" + _completed;
            }
        }

        /// <summary>每帧推进一次；chefs 是"厨师位 → 控制器 + 位置 + 手上东西"。</summary>
        public static void Tick(float now, List<ChefState> chefs)
        {
            if (chefs.Count == 0)
            {
                return;
            }

            if (_step == Step.Idle)
            {
                string dish = Kitchen.FirstOrderDish();
                if (dish.Length == 0)
                {
                    return; // 还没有订单
                }

                _slot = PickChef(chefs);
                if (_slot < 0)
                {
                    return;
                }

                _log.LogInfo("工序: 接单 " + dish + "，交给厨师位 " + (_slot + 1));
                Begin(Step.GetPlate, now);
                return;
            }

            var chef = chefs.FirstOrDefault(c => c.Slot == _slot);
            if (chef == null)
            {
                Fail(now, "这个厨师位没有厨师了");
                return;
            }

            // 到点了还没做完 → 报失败、重来这一步
            if (now - _stepStart > StepTimeout)
            {
                Fail(now, _step + " 超时 " + StepTimeout.ToString("F0") + " 秒");
                return;
            }

            float distance = Vector2.Distance(chef.Position, _target);
            if (distance > ReachRadius)
            {
                return; // 还没走到，驱动层在推
            }

            if (!_interacted)
            {
                if (_interactAt <= 0f)
                {
                    _interactAt = now + InteractHold;
                    _pressUntil = now + InteractHold;
                    _pressing = true;
                    _log.LogInfo("工序: " + _step + " 到达（距目标 " + distance.ToString("F2")
                                 + " 米），按交互");
                    return;
                }

                if (now < _pressUntil)
                {
                    return; // 正在按
                }

                _pressing = false;
                _interacted = true;
                return;
            }

            // 交互已按下：等世界状态出现预期变化（手上多了什么 / 变成空手）
            if (now - _interactAt < InteractHold)
            {
                return;
            }

            string held = chef.Held;
            if (_expectHeld.Length > 0 && !held.Contains(_expectHeld))
            {
                if (now - _interactAt > 2.5f)
                {
                    Fail(now, _step + " 后手上应该是「" + _expectHeld + "」，实际「" + held + "」");
                }

                return;
            }

            if (_expectHeld.Length == 0 && held.Length > 0 && now - _interactAt > 2.5f)
            {
                Fail(now, _step + " 后应该空手，实际「" + held + "」");
                return;
            }

            Next(now);
        }

        /// <summary>谁去做这一单：现在取第一个厨师位（替身，等 Scheduler 接管）。</summary>
        private static int PickChef(List<ChefState> chefs)
        {
            return chefs[0].Slot;
        }

        private static void Begin(Step step, float now)
        {
            _step = step;
            _stepStart = now;
            _interactAt = 0f;
            _pressing = false;
            _interacted = false;
            _hasTarget = false;

            switch (step)
            {
                case Step.GetPlate:
                {
                    var stack = Kitchen.PlateStack();
                    if (stack == null)
                    {
                        Fail(now, "找不到盘子堆");
                        return;
                    }

                    _target = stack.Position;
                    _hasTarget = true;
                    _expectHeld = "Plate";
                    _log.LogInfo("工序: 1/3 去拿盘子（" + stack.Name + " "
                                 + _target.x.ToString("F1") + "," + _target.y.ToString("F1") + "）");
                    break;
                }

                case Step.GetIngredient:
                {
                    string want = WantedIngredient();
                    var crate = Kitchen.Crates().FirstOrDefault(c => c.Gives.Contains(want));
                    if (crate == null)
                    {
                        Fail(now, "找不到给「" + want + "」的料箱");
                        return;
                    }

                    _target = crate.Position;
                    _hasTarget = true;
                    _expectHeld = want;
                    _log.LogInfo("工序: 2/3 去取料「" + want + "」（" + crate.Name + " "
                                 + _target.x.ToString("F1") + "," + _target.y.ToString("F1") + "）");
                    break;
                }

                case Step.Deliver:
                {
                    var station = Kitchen.DeliveryStation();
                    if (station == null)
                    {
                        Fail(now, "找不到出菜台");
                        return;
                    }

                    _target = station.Position;
                    _hasTarget = true;
                    _expectHeld = ""; // 交付后应该空手
                    _log.LogInfo("工序: 3/3 去交付（" + station.Name + " "
                                 + _target.x.ToString("F1") + "," + _target.y.ToString("F1") + "）");
                    break;
                }
            }
        }

        private static void Next(float now)
        {
            switch (_step)
            {
                case Step.GetPlate:
                    Begin(Step.GetIngredient, now);
                    break;
                case Step.GetIngredient:
                    Begin(Step.Deliver, now);
                    break;
                case Step.Deliver:
                    _completed++;
                    _log.LogInfo("工序: 完成一单（累计 " + _completed + " 单）");
                    _lastError = "";
                    _step = Step.Idle;
                    _hasTarget = false;
                    break;
            }
        }

        private static void Fail(float now, string reason)
        {
            _log.LogWarning("工序: 失败——" + reason + "，重来这一步");
            _lastError = reason;
            if (_step == Step.Idle)
            {
                return;
            }

            Begin(_step, now);
        }

        /// <summary>这一单要什么料：从菜谱的步骤树里取叶子节点名（如 SushiFish）。</summary>
        private static string WantedIngredient()
        {
            var controller = WorldState.FirstOrderController();
            var leaves = WorldState.OrderStepLeaves(controller);
            if (leaves.Count > 0)
            {
                return leaves[0];
            }

            return "SushiFish";
        }

        /// <summary>一个厨师位当前的状态：谁、在哪、手上拿着什么。</summary>
        public sealed class ChefState
        {
            public int Slot;
            public Vector2 Position;
            public string Held;
        }
    }
}
