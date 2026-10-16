using BepInEx.Logging;

namespace WellCooked
{
    /// <summary>
    /// 诊断输出的唯一开关（任务 10 收敛）。
    ///
    /// 为什么要有它：这一轮调试时"每帧/每 2 秒打一行"的代码散在 `Takeover`、`WorkOrder`、
    /// `WorldState` 三处，各自判各自的标志（`_trace`、`work`、`_verbose`），于是跑一次工序
    /// 日志每秒几十行，真正要看的"取料/切/装盘/交付的结果"被淹掉。
    ///
    /// 规矩（新加日志时按这个选）：
    /// - **结果行**：状态迁移、每步的开始与结果、失败原因、指令执行结果 —— 不受开关影响，总是打。
    /// - **过程行**：每帧或每几秒一次的位置、路点、手柄状态、反射取到的中间值 —— 一律走
    ///   <see cref="Trace"/>，只在开关打开时打。
    ///
    /// 开关位置：`BepInEx/wellcooked-takeover.txt` 的 `diag=true`（旧的 `trace=true` 等价，仍然认）。
    /// </summary>
    internal static class Diag
    {
        private static ManualLogSource _log;
        private static bool _verbose;

        public static bool Verbose
        {
            get { return _verbose; }
        }

        public static void Install(ManualLogSource log)
        {
            _log = log;
        }

        public static void Set(bool on)
        {
            _verbose = on;
        }

        /// <summary>过程行：默认不打。</summary>
        public static void Trace(string line)
        {
            if (_verbose && _log != null)
            {
                _log.LogInfo(line);
            }
        }
    }
}
