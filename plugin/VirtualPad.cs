using System;
using System.Collections.Generic;
using System.Reflection;
using InControl;
using UnityEngine;

namespace WellCooked
{
    /// <summary>
    /// 进程内的虚拟手柄：游戏把它当成一个真手柄。
    ///
    /// 为什么走设备层而不是补 PlayerInputLookup：spike game-bridge/06 实测，前端的"按 A 继续"
    /// 只在设备层生效（逻辑层替换被轮询但不驱动）。设备由游戏自己枚举，因此我们每帧只需
    /// 声明这一帧各控制项的状态。
    ///
    /// InControl 这套 build 的两个坑（同样来自那次实测）：
    /// - 控制项名字和文档不一致（没有 LeftStickX/Y），所以按枚举把所有控制项都注册一遍；
    /// - UpdateWithState / UpdateLeftStickWithValue 不是 protected，子类直接调用编译不过，得反射。
    /// </summary>
    public sealed class VirtualPad : InputDevice
    {
        private const BindingFlags Instance = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic;

        private static readonly MethodInfo SetControlState = typeof(InputDevice).GetMethod("UpdateWithState", Instance);
        private static readonly MethodInfo SetStickValue = typeof(InputDevice).GetMethod("UpdateLeftStickWithValue", Instance);
        private static readonly MethodInfo CommitStick = typeof(InputDevice).GetMethod("CommitLeftStick", Instance);

        private readonly InputControlType _action1;
        private readonly InputControlType _action2;

        private bool _wantAction1;
        private bool _wantAction2;
        private Vector2 _wantStick;

        public VirtualPad(string name, int slot)
            : base(name)
        {
            Name = name;
            Meta = "wellcooked-pad-" + slot;

            // 控制项名字在这套 InControl 里和文档对不上，直接按枚举全注册
            foreach (InputControlType control in Enum.GetValues(typeof(InputControlType)))
            {
                int index = (int)control;
                if (index > 0 && index < Controls.Length && !HasControl(control))
                {
                    AddControl(control, control.ToString());
                }
            }

            _action1 = GetInputControlTypeByName("Action1");
            _action2 = GetInputControlTypeByName("Action2");
            CacheDirectionType("LeftStickLeft");
            CacheDirectionType("LeftStickRight");
            CacheDirectionType("LeftStickUp");
            CacheDirectionType("LeftStickDown");

            if (SetControlState == null)
            {
                throw new MissingMethodException("InControl.InputDevice.UpdateWithState 不存在，虚拟手柄无法设置按键状态");
            }
        }

        /// <summary>策略每帧调这两个方法声明想要的输入，真正的写入在 Update 里做。</summary>
        public void Want(bool action1, bool action2, Vector2 stick)
        {
            _wantAction1 = action1;
            _wantAction2 = action2;
            _wantStick = stick;
        }

        public override void Update(ulong updateTick, float deltaTime)
        {
            try
            {
                SetControlState.Invoke(this, new object[] { _action1, _wantAction1, updateTick, deltaTime });
                SetControlState.Invoke(this, new object[] { _action2, _wantAction2, updateTick, deltaTime });
                ButtonWrites++;

                if (SetStickValue != null)
                {
                    SetStickValue.Invoke(this, new object[] { _wantStick, updateTick, deltaTime });
                    if (CommitStick != null)
                    {
                        CommitStick.Invoke(this, null);
                    }

                    StickWrites++;
                }

                // 双保险：这套 InControl 把摇杆拆成方向控制项，模拟量和数字方向一起给，
                // 免得只写其中一条路而游戏读的是另一条。
                PressDirection("LeftStickLeft", _wantStick.x < -0.3f, updateTick, deltaTime);
                PressDirection("LeftStickRight", _wantStick.x > 0.3f, updateTick, deltaTime);
                PressDirection("LeftStickUp", _wantStick.y > 0.3f, updateTick, deltaTime);
                PressDirection("LeftStickDown", _wantStick.y < -0.3f, updateTick, deltaTime);
            }
            catch (Exception e)
            {
                LastError = e.GetType().Name + ": " + e.Message;
            }
        }

        private void PressDirection(string controlName, bool pressed, ulong updateTick, float deltaTime)
        {
            InputControlType type;
            if (!_directionTypes.TryGetValue(controlName, out type))
            {
                return;
            }

            SetControlState.Invoke(this, new object[] { type, pressed, updateTick, deltaTime });
        }

        private readonly Dictionary<string, InputControlType> _directionTypes = new Dictionary<string, InputControlType>();

        private void CacheDirectionType(string controlName)
        {
            try
            {
                _directionTypes[controlName] = GetInputControlTypeByName(controlName);
            }
            catch (ArgumentException)
            {
                // 这套 InControl 里没有这个名字，跳过
            }
        }

        /// <summary>调试用：当前想要的输入，日志里读它。</summary>
        public string Wanted
        {
            get { return "A1=" + _wantAction1 + " A2=" + _wantAction2 + " stick=" + _wantStick; }
        }

        /// <summary>
        /// 反射调用是否真的成功过。日志只写"想要什么"会骗人：写不进游戏时看起来一样。
        /// </summary>
        public static int ButtonWrites;
        public static int StickWrites;
        public static string LastError = "";

        public static string Diagnostics
        {
            get
            {
                return "按键写入=" + ButtonWrites
                       + " 摇杆写入=" + StickWrites
                       + " 摇杆方法=" + (SetStickValue == null ? "缺" : "有")
                       + (LastError.Length == 0 ? "" : " 最近错误=" + LastError);
            }
        }

        /// <summary>没有摇杆 API 时退回到方向控制项（保留给调试）。</summary>
        public void WantDirections(bool up, bool down, bool left, bool right, ulong updateTick, float deltaTime)
        {
            foreach (var pair in new[]
                     {
                         new KeyValuePair<string, bool>("LeftStickUp", up),
                         new KeyValuePair<string, bool>("LeftStickDown", down),
                         new KeyValuePair<string, bool>("LeftStickLeft", left),
                         new KeyValuePair<string, bool>("LeftStickRight", right)
                     })
            {
                try
                {
                    SetControlState.Invoke(this, new object[] { GetInputControlTypeByName(pair.Key), pair.Value, updateTick, deltaTime });
                }
                catch (ArgumentException)
                {
                    // 这个名字在这套 InControl 里不存在，跳过
                }
            }
        }
    }
}
