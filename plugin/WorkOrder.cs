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
            GetIngredient,
            DropOnCounter,
            GetPlate,
            PlateIt,
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
        private static float _nextPress;
        private static float _lastCheckAt;
        private static bool _warned;
        private static bool _arrivedLogged;
        private static ChefState _lastChef;
        private static string _facingName = "?";
        private static float _closestDistance = float.MaxValue;
        private static float _closestAt;
        private static bool _interacted;
        private static string _expectHeld = "";
        private static string _expectContents = "";
        private static Vector2 _target;
        private static Vector2 _counter;
        private static Vector2 _station;   // 真正要交互的工位位置（朝向它）
        private static Vector2 _faceDirection;
        private static float _faceUntil;
        private static bool _facing;
        private static bool _hasTarget;
        private static int _completed;
        private static string _lastError = "";

        /// <summary>每一步的超时（秒）。到点没做成就报失败、重来。</summary>
        private const float StepTimeout = 20f;

        /// <summary>到达判定半径：工位都在台面里，厨师只能站到旁边一格。</summary>
        private const float ReachRadius = 1.6f;

        /// <summary>交互按下的时长（秒）。游戏只认按下的边沿，必须松开。</summary>
        private const float InteractHold = 0.25f;

        /// <summary>隔多久按一次交互（秒）。交互有没有生效由世界状态判定，没生效就再按。</summary>
        private const float PressInterval = 0.5f;

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

        /// <summary>
        /// 到点后要先"朝着工位站好"再按 A：游戏的交互取的是"厨师所在格的邻居格里
        /// 朝向最对齐的那一格"，光走到旁边不够，还得面向它。
        /// 返回 true 时驱动层直接喂 face 方向，不走导航。
        /// </summary>
        public static bool FaceFor(int slot, out Vector2 face)
        {
            face = Vector2.zero;
            if (_slot != slot || !_facing)
            {
                return false;
            }

            face = _faceDirection;
            return true;
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
                Begin(Step.GetIngredient, now);
                return;
            }

            var chef = chefs.FirstOrDefault(c => c.Slot == _slot);
            _lastChef = chef;
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
            float toStation = Vector2.Distance(chef.Position, _station);
            string heldNow = chef.Held;

            // 最后一段不用寻路，直接朝工位推：像人一样顶着台面站到它旁边那一格，
            // 推的过程顺便把朝向也对准了（交互按的是"面前那一格"，见 GetFacingGridOccupant）。
            // 寻路停在哪一格是"我们网格"说了算，未必正好是游戏网格的相邻格（实测差 1.48 米）。
            if (toStation <= 2.4f)
            {
                if (!_arrivedLogged)
                {
                    _arrivedLogged = true;
                    _log.LogInfo("工序: " + _step + " 接近工位（" + toStation.ToString("F2") + " 米），直接朝它推");
                }

                if (!_facing)
                {
                    _nextPress = now + 0.35f; // 先转 0.35 秒身再按
                }

                _facing = true;
                _facingName = chef.FacingName;

                // 先朝工位推，推到推不动（离工位最近距离 0.35 秒没再变近）就停下再按：
                // 一边顶一边按，游戏那边不认（实测面前确实是料箱、按了很多次也拿不到料）。
                if (toStation < _closestDistance - 0.02f)
                {
                    _closestDistance = toStation;
                    _closestAt = now;
                }

                bool blocked = now - _closestAt > 0.35f;
                _faceDirection = blocked || (chef.Position - _station).sqrMagnitude <= 0.0001f
                    ? Vector2.zero
                    : (_station - chef.Position).normalized;

                if (!_pressing && now >= _nextPress)
                {
                    _nextPress = now + PressInterval;
                    _pressUntil = now + InteractHold;
                    _pressing = true;
                    _lastCheckAt = now + InteractHold + 0.2f;
                    _log.LogInfo("工序: " + _step + " 按交互（手上「" + heldNow + "」，距工位 "
                                 + toStation.ToString("F2") + " 米，面前是「" + _facingName + "」）");
                }

                if (_pressing && now >= _pressUntil)
                {
                    _pressing = false;
                }

                if (now >= _lastCheckAt)
                {
                    if (Matches(heldNow))
                    {
                        Next(now);
                        return;
                    }

                    // 检查过了就作废这次检查，别每帧都往后推"下一次按"——那样永远按不出去
                    _lastCheckAt = float.MaxValue;
                    _nextPress = now + PressInterval;
                }

                if (!_warned && now - _stepStart > 8f)
                {
                    _warned = true;
                    _log.LogWarning("工序: " + _step + " 推了几次没效果（手上「" + heldNow
                                    + "」，期望「" + (_expectHeld.Length == 0 ? "空手" : _expectHeld) + "」），继续试");
                }

                return;
            }

            // 已经到位了：先朝着工位站好，再按交互
            if (distance <= ReachRadius)
            {
                if (!_arrivedLogged)
                {
                    _arrivedLogged = true;
                    _log.LogInfo("工序: " + _step + " 到位（距目标 " + distance.ToString("F2") + " 米），先朝向工位");
                    _facing = true;
                    _faceUntil = now + 0.45f;
                    _faceDirection = (chef.Position - _station).sqrMagnitude > 0.0001f
                        ? (_station - chef.Position).normalized
                        : Vector2.zero;
                    return;
                }

                if (_facing)
                {
                    if (now < _faceUntil)
                    {
                        _faceDirection = (chef.Position - _station).sqrMagnitude > 0.0001f
                            ? (_station - chef.Position).normalized
                            : Vector2.zero;
                        return;
                    }

                    _facing = false;
                    _nextPress = now;
                    _lastCheckAt = float.MaxValue;
                    _log.LogInfo("工序: " + _step + " 朝好了，开始按交互");
                }

                // 按一次 → 等效果 → 没效果才再按。
                // 不能按完就接着按：0.5 秒后又按一次会把刚拿到的东西放下
                // （实测 手上在「」和「SushiFish」之间反复跳，永远判不出成功）。
                if (!_pressing && now >= _nextPress)
                {
                    _nextPress = now + PressInterval;
                    _pressUntil = now + InteractHold;
                    _pressing = true;
                    _lastCheckAt = now + InteractHold + 0.2f;
                    _log.LogInfo("工序: " + _step + " 按交互（手上「" + heldNow + "」）");
                }

                if (_pressing && now >= _pressUntil)
                {
                    _pressing = false;
                }

                if (now < _lastCheckAt)
                {
                    return;
                }

                if (Matches(heldNow))
                {
                    Next(now);
                    return;
                }

                // 没效果：把下一次按往后推（等按键边沿真的被游戏消费掉）
                if (now >= _lastCheckAt && _nextPress <= _lastCheckAt)
                {
                    _nextPress = now + PressInterval;
                }

                // 按了没效果：继续按（可能还没进交互范围），但不刷屏
                if (!_warned && now - _stepStart > 6f)
                {
                    _warned = true;
                    _log.LogWarning("工序: " + _step + " 按了几次没效果（手上「" + heldNow
                                    + "」，期望「" + (_expectHeld.Length == 0 ? "空手" : _expectHeld) + "」），继续试");
                }

                return;
            }

            // 还没到位：驱动层在推，先不按
            if (_pressing && now >= _pressUntil)
            {
                _pressing = false;
            }
        }

        /// <summary>把目标设成"工位相邻格"，并记住工位本身（到点后要朝它）。</summary>
        private static void AimAt(Vector2 station)
        {
            _station = station;
            Vector2 approach;
            var chef = _lastChef;
            Vector2 from = chef == null ? station : chef.Position;
            _target = Kitchen.ApproachPosition(station, from, out approach) ? approach : station;
        }

        /// <summary>手上状态是否符合这一步的预期。</summary>
        private static bool Matches(string held)
        {
            if (_expectHeld.Length == 0)
            {
                return held.Length == 0;
            }

            return held.Contains(_expectHeld)
                   && (_expectContents.Length == 0 || held.Contains(_expectContents));
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
            _nextPress = 0f;
            _lastCheckAt = float.MaxValue;
            _warned = false;
            _arrivedLogged = false;
            _facing = false;
            _closestDistance = float.MaxValue;
            _closestAt = now;
            _hasTarget = false;

            switch (step)
            {
                case Step.GetIngredient:
                {
                    string want = WantedIngredient();
                    var crate = Kitchen.Crates().FirstOrDefault(c => c.Gives.Contains(want));
                    if (crate == null)
                    {
                        Fail(now, "找不到给「" + want + "」的料箱");
                        return;
                    }

                    AimAt(crate.Position);
                    _hasTarget = true;
                    _expectHeld = want;
                    _expectContents = "";
                    _log.LogInfo("工序: 1/5 去取料「" + want + "」（" + crate.Name + " "
                                 + _target.x.ToString("F1") + "," + _target.y.ToString("F1") + "）");
                    break;
                }

                case Step.DropOnCounter:
                {
                    var counter = Kitchen.FreeCounter(_target);
                    if (counter == null)
                    {
                        Fail(now, "找不到空台面放料");
                        return;
                    }

                    _counter = counter.Position;
                    AimAt(counter.Position);
                    _hasTarget = true;
                    _expectHeld = ""; // 放下后空手
                    _expectContents = "";
                    _log.LogInfo("工序: 2/5 把料放到台面（" + counter.Name + " "
                                 + _target.x.ToString("F1") + "," + _target.y.ToString("F1") + "）");
                    break;
                }

                case Step.GetPlate:
                {
                    var stack = Kitchen.PlateStack();
                    if (stack == null)
                    {
                        Fail(now, "找不到盘子堆");
                        return;
                    }

                    AimAt(stack.Position);
                    _hasTarget = true;
                    _expectHeld = "Plate";
                    _expectContents = "";
                    _log.LogInfo("工序: 3/5 去拿盘子（" + stack.Name + " "
                                 + _target.x.ToString("F1") + "," + _target.y.ToString("F1") + "）");
                    break;
                }

                case Step.PlateIt:
                {
                    AimAt(_counter);
                    _hasTarget = true;
                    _expectHeld = "Plate";                  // 装盘后手上是"装着料的盘子"
                    _expectContents = WantedIngredient();
                    _log.LogInfo("工序: 4/5 回去装盘（台面 "
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

                    AimAt(station.Position);
                    _hasTarget = true;
                    _expectHeld = ""; // 交付后应该空手
                    _expectContents = "";
                    _log.LogInfo("工序: 5/5 去交付（" + station.Name + " "
                                 + _target.x.ToString("F1") + "," + _target.y.ToString("F1") + "）");
                    break;
                }
            }
        }

        private static void Next(float now)
        {
            switch (_step)
            {
                case Step.GetIngredient:
                    Begin(Step.DropOnCounter, now);
                    break;
                case Step.DropOnCounter:
                    Begin(Step.GetPlate, now);
                    break;
                case Step.GetPlate:
                    Begin(Step.PlateIt, now);
                    break;
                case Step.PlateIt:
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
            public string FacingName = "?";
        }
    }
}
