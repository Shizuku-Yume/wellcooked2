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
            Chop,
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
        private static string _stationName = "";
        private static float _closestDistance = float.MaxValue;
        private static float _closestAt;
        private static float _faceNextPulse;
        private static float _facePulseUntil;
        private static bool _chopStarted;
        private static bool _work;
        private static float _chopLogAt;
        private static bool _interacted;
        private static string _expectHeld = "";
        private static string _expectContents = "";
        private static Vector2 _target;
        private static Vector2 _counter;
        private static string _counterName = "";
        private static Vector2 _station;   // 真正要交互的工位位置（朝向它）
        private static Vector2 _faceDirection;
        private static float _faceUntil;
        private static bool _facing;
        private static bool _hasTarget;
        private static int _completed;
        private static string _lastError = "";

        /// <summary>每一步的超时（秒）。到点没做成就报失败、重来。</summary>
        private const float StepTimeout = 20f;
        private const float ChopTimeout = 60f;

        /// <summary>到达判定半径：工位都在台面里，厨师只能站到旁边一格。</summary>
        private const float ReachRadius = 0.25f;

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
        public static bool DesireFor(int slot, out Vector2 target, out bool interact, out bool work)
        {
            target = _target;
            interact = _slot == slot && _pressing;
            work = _slot == slot && _work;   // X 键：切菜/用台面
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
            float limit = _step == Step.Chop ? ChopTimeout : StepTimeout;
            if (now - _stepStart > limit)
            {
                Fail(now, _step + " 超时 " + limit.ToString("F0") + " 秒");
                return;
            }

            float distance = Vector2.Distance(chef.Position, _target);
            float toStation = Vector2.Distance(chef.Position, _station);
            string heldNow = chef.Held;

            // 已经到位了：站住、把朝向对准工位、按 A。
            // 到位之后不再寻路（Takeover 见到 _facing 就只喂朝向不导航）。
            // 带滞回：到位过之后放宽到 0.6 米，否则在半径边上反复横跳
            // （实测停在 0.28 米：既回不到 0.25 的到位判定、也不按、也不超时）。
            if (distance <= (_arrivedLogged ? ReachRadius * 2.4f : ReachRadius))
            {
                _facing = true;

                if (_step == Step.Chop && HandleChop(now, heldNow))
                {
                    return;
                }

                Vector2 toward = (chef.Position - _station).sqrMagnitude > 0.0001f
                    ? (_station - chef.Position).normalized
                    : Vector2.zero;

                if (!_arrivedLogged)
                {
                    _arrivedLogged = true;
                    _log.LogInfo("工序: " + _step + " 到位（距目标 " + distance.ToString("F2")
                                 + " 米），对准工位");
                }

                // 游戏挑的是"最对齐的邻居格"，站错格会挑到别的东西（实测挑到垃圾桶，
                // 一按就把盘子丢了）。所以先问游戏面前是什么，不是工位就只转身不按。
                string facing = chef.FacingName;

                // 名字不完全可靠：盘子堆挂在台面上，游戏返回的占位物是台面本身
                // （实测面前是「countertop_fake」而不是「Plate 1」）。所以再比一次格子：
                // 面前那东西和工位在同一格，就当成对上了。
                bool facingStation = facing == _stationName;
                if (!facingStation && chef.HasFacing)
                {
                    int fx, fz, sx, sz;
                    facingStation = WorldState.GameCellOf(chef.FacingPosition, out fx, out fz)
                                    && WorldState.GameCellOf(_station, out sx, out sz)
                                    && fx == sx && fz == sz;
                }

                if (!facingStation)
                {
                    // 转身要短促：一直喂方向会顶着工位沿它滑走（实测滑出 2.6 米）。
                    if (now >= _faceNextPulse)
                    {
                        _faceNextPulse = now + 0.45f;
                        _facePulseUntil = now + 0.12f;
                    }

                    _faceDirection = now < _facePulseUntil ? toward : Vector2.zero;

                    if (!_warned && now - _stepStart > 4f)
                    {
                        _warned = true;
                        _log.LogWarning("工序: " + _step + " 面前是「" + facing + "」不是「"
                                        + _stationName + "」，先不按，继续对准");
                    }

                    return;
                }

                _faceDirection = Vector2.zero; // 面前就是工位：站住别动，按 A

                // 按一次 → 等效果 → 没效果才再按。
                // 不能按完就接着按：0.5 秒后又按一次会把刚拿到的东西放下
                // （实测 手上在「」和「SushiFish」之间反复跳，永远判不出成功）。
                if (!_pressing && now >= _nextPress)
                {
                    _nextPress = now + PressInterval;
                    _pressUntil = now + InteractHold;
                    _pressing = true;
                    _lastCheckAt = now + InteractHold + 0.2f;
                    _log.LogInfo("工序: " + _step + " 按交互（手上「" + heldNow + "」，面前是「"
                                 + facing + "」，台面上是「" + Kitchen.StationItemName(_station) + "」）");
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

                // 检查过了就作废这次检查，别每帧都往后推"下一次按"——那样永远按不出去
                _lastCheckAt = float.MaxValue;
                _nextPress = now + PressInterval;

                return;
            }

            // 还没到位：驱动层在推，先不按
            if (_pressing && now >= _pressUntil)
            {
                _pressing = false;
            }
        }

        /// <summary>
        /// 切料这一步：按一次 A 开始（厨师站在砧板前就会一直切，切的动作由厨师动画驱动），
        /// 站着等 `ClientWorkableItem.HasFinished()` 变真，再按一次把切好的拿走。
        /// 返回 true 表示这一步还没走完（调用方直接 return）。
        /// </summary>
        private static bool HandleChop(float now, string heldNow)
        {
            bool finished;
            string workState;
            bool hasItem = Kitchen.WorkState(_station, out finished, out workState);

            if (hasItem && !finished)
            {
                _faceDirection = Vector2.zero; // 站着别动

                // 切按的是 **X（WorkstationInteract）**，不是 A：PlayerControls 里
                // m_worksurfaceUseButton = LogicalButtonID.WorkstationInteract，
                // 而 A 是 PickupAndDrop——空手按 A 只会把料拿走（实测就是这么丢的）。
                // 而且交互是 sticky 的，要**按住**，厨师才会一直切（动画事件驱动 OnChop 加进度）。
                _work = true;
                _pressing = false;

                if (!_chopStarted)
                {
                    _chopStarted = true;
                    _log.LogInfo("工序: 3/6 开始切（按住 X，当前 " + workState + "）");
                }

                if (now >= _chopLogAt)
                {
                    _chopLogAt = now + 3f;
                    _log.LogInfo("工序: 3/6 切料中（" + workState + "，手上「" + heldNow + "」）");
                }

                return true;
            }

            if (hasItem && finished)
            {
                _faceDirection = Vector2.zero;

                if (!_pressing && now >= _nextPress)
                {
                    _nextPress = now + PressInterval;
                    _pressUntil = now + InteractHold;
                    _pressing = true;
                    _lastCheckAt = now + InteractHold + 0.2f;
                    _log.LogInfo("工序: 3/6 切好了（" + workState + "），按 A 拿走");
                }

                if (_pressing && now >= _pressUntil)
                {
                    _pressing = false;
                }

                if (now >= _lastCheckAt)
                {
                    _lastCheckAt = float.MaxValue;

                    if (Matches(heldNow))
                    {
                        Next(now);
                        return true;
                    }

                    _nextPress = now + PressInterval;
                }

                return true;
            }

            // 砧板上没东西：要么已经拿在手上（成功），要么放料那步根本没放上去
            if (Matches(heldNow))
            {
                Next(now);
                return true;
            }

            return false;
        }

        /// <summary>把目标设成"工位相邻格"，并记住工位本身（到点后要朝它）。</summary>
        private static void AimAt(Vector2 station, string name)
        {
            _station = station;
            _stationName = name ?? "";
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
            _faceNextPulse = 0f;
            _facePulseUntil = 0f;
            _chopStarted = false;
            _work = false;
            _chopLogAt = 0f;
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

                    AimAt(crate.Position, crate.Name);
                    _hasTarget = true;
                    _expectHeld = want;
                    _expectContents = "";
                    _log.LogInfo("工序: 1/6 去取料「" + want + "」（" + crate.Name + " "
                                 + _target.x.ToString("F1") + "," + _target.y.ToString("F1") + "）");
                    break;
                }

                case Step.DropOnCounter:
                {
                    // 鱼要切：放的地方必须是砧板（ClientWorkstation），不是随便一块空台面。
                    var counter = Kitchen.ChoppingBoard();
                    if (counter == null)
                    {
                        Fail(now, "找不到砧板放料");
                        return;
                    }

                    _counter = counter.Position;
                    _counterName = counter.Name;
                    AimAt(counter.Position, counter.Name);
                    _hasTarget = true;
                    _expectHeld = ""; // 放下后空手
                    _expectContents = "";
                    _log.LogInfo("工序: 2/6 把料放到砧板（" + counter.Name + " "
                                 + _target.x.ToString("F1") + "," + _target.y.ToString("F1") + "）");
                    break;
                }

                case Step.Chop:
                {
                    // 切是自动的：站到砧板前按一次 A 开始，厨师站在那儿就一直切
                    // （`ClientWorkstation.OnChop` 由厨师的切菜动画驱动），切完再按一次拿走。
                    AimAt(_counter, _counterName);
                    _hasTarget = true;
                    _expectHeld = "";                 // 切的时候手是空的
                    _expectContents = "";
                    _log.LogInfo("工序: 3/6 切料（砧板 " + _counterName + " "
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

                    AimAt(stack.Position, stack.Name);
                    _hasTarget = true;
                    _expectHeld = "Plate";
                    _expectContents = "";
                    _log.LogInfo("工序: 4/6 去拿盘子（" + stack.Name + " "
                                 + _target.x.ToString("F1") + "," + _target.y.ToString("F1") + "）");
                    break;
                }

                case Step.PlateIt:
                {
                    AimAt(_counter, _counterName);
                    _hasTarget = true;
                    _expectHeld = "Plate";                  // 装盘后手上是"装着料的盘子"
                    _expectContents = WantedIngredient();
                    _log.LogInfo("工序: 5/6 回去装盘（砧板 "
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

                    AimAt(station.Position, station.Name);
                    _hasTarget = true;
                    _expectHeld = ""; // 交付后应该空手
                    _expectContents = "";
                    _log.LogInfo("工序: 6/6 去交付（" + station.Name + " "
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
                    Begin(Step.Chop, now);
                    break;

                case Step.Chop:
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
            public Vector2 FacingPosition;
            public bool HasFacing;
        }
    }
}
