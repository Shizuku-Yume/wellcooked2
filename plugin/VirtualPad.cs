using System;
using System.Collections.Generic;
using System.Reflection;
using InControl;
using UnityEngine;

namespace WellCooked
{
    /// <summary>
    /// 进程内的虚拟手柄（spike game-bridge/06 的路线 B）。
    ///
    /// 游戏通过 InControl 认设备；在游戏进程里建一个 InputDevice 挂上去，
    /// 就等于插了一个手柄，不需要 ViGEm/uinput，也不碰游戏自己的逻辑输入层。
    /// 按下模式是"每 3 秒按 0.3 秒 A 键"，共 30 秒，用来观察前端的加入/继续流程。
    /// </summary>
    public sealed class VirtualPad : InputDevice
    {
        private const float MashSeconds = 30f;
        private const float PressPeriod = 3f;
        private const float PressLength = 0.3f;

        private readonly InputControlType _action1;
        private readonly float _attachedAt;

        public VirtualPad(string name)
            : base(name)
        {
            Name = name;
            Meta = "wellcooked";

            // 用名字拿控制类型：这个 InControl 版本里没有 LeftStickX 之类的名字，缺哪个就跳过哪个
            _action1 = Required("Action1");
            Optional("Action2");
            Optional("LeftStickX");
            Optional("LeftStickY");
            Optional("DPadX");
            Optional("DPadY");

            _attachedAt = Time.realtimeSinceStartup;

            if (SetState == null)
            {
                throw new MissingMethodException("InControl.InputDevice.UpdateWithState 不存在，虚拟手柄无法设置按键状态");
            }
        }

        /// <summary>控制名在这个 build 里不存在时不要整体失败，记下来由调用方打日志。</summary>
        public static readonly List<string> Skipped = new List<string>();

        private InputControlType Required(string controlName)
        {
            var type = GetInputControlTypeByName(controlName);
            AddControl(type, controlName);
            return type;
        }

        private void Optional(string controlName)
        {
            try
            {
                AddControl(GetInputControlTypeByName(controlName), controlName);
            }
            catch (ArgumentException)
            {
                Skipped.Add(controlName);
            }
        }

        public override void Update(ulong updateTick, float deltaTime)
        {
            var elapsed = Time.realtimeSinceStartup - _attachedAt;
            var pressed = elapsed < MashSeconds && (elapsed % PressPeriod) < PressLength;
            // 这个 build 里 UpdateWithState 不是 protected，反射调用；找不到就直接报错，不要静默
            SetState.Invoke(this, new object[] { _action1, pressed, updateTick, deltaTime });
        }

        private static readonly MethodInfo SetState = typeof(InputDevice).GetMethod(
            "UpdateWithState",
            BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);

        /// <summary>调试用：当前是否在按下窗口里。</summary>
        public bool Action1Pressed
        {
            get
            {
                var elapsed = Time.realtimeSinceStartup - _attachedAt;
                return elapsed < MashSeconds && (elapsed % PressPeriod) < PressLength;
            }
        }
    }
}
