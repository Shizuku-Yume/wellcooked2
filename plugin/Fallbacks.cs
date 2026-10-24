namespace WellCooked
{
    /// <summary>
    /// 兜底（任务 10 收敛）：这一轮为了"先跑起来"加的放宽与降级，全都放在这里，一处能看全。
    ///
    /// 它们**不是机制**，是已知不精确的临时做法。每条都写清"为什么加"和"什么时候该去掉"；
    /// 该去掉的那条对应的任务落地时，从这里删掉常量并改回严格判定。
    /// 新加兜底时也放这里 —— 散在状态机里的放宽，下一个人只会看到"它就是这么做的"。
    /// </summary>
    internal static class Fallbacks
    {
        /// <summary>
        /// 精确就位卡住这么久之后，放宽到 <see cref="LooseApproachRadius"/>、并且不再校验朝向。
        ///
        /// 为什么：工位的相邻格未必站得进去（出菜口嵌在墙里、盘子堆旁边被台面挡），
        /// 精确就位会一直打转（实测停在 0.28 米：既回不到 0.25 的到位判定、也不按、也不超时）。
        /// 代价：放宽之后按的是"大概方向"，站错格就可能按到别的东西 —— 垃圾桶那次就是这么把鱼扔掉的。
        /// 该去掉：到达判定改用游戏自己的网格（`GridNavSpace` / `GetFacingGridOccupant` 反推可用格），
        /// 或按工位类型分别定义"哪个方向能站"（任务 11/12 的边界）。
        /// </summary>
        public const float LooseApproachAfterSeconds = 6f;
        public const float LooseApproachRadius = 1.4f;

        /// <summary>
        /// 切菜"按住 X"的上限：到这个时间还没切完就报失败重来。
        ///
        /// 为什么：切菜进度其实在 `ClientWorkableItem` 上（`m_progress` / `HasFinished()`），
        /// 但当前实现是"按住一直等"，这个超时值就成了隐式行为参数。
        /// 代价：切完不及时松手，多切/切糊都有可能。
        /// 该去掉：改成按 `HasFinished()` 收手（任务 12）。
        /// </summary>
        public const float ChopHoldTimeoutSeconds = 60f;

        /// <summary>
        /// 只在这个距离内才做"侧向错身"。
        ///
        /// 为什么：两个厨师对着同一个点互推时谁都不让，会双双卡死，所以要错身。
        /// 代价：错身期间厨师一直在小幅移动，"卡住"计时攒不够 —— 离得远时用它就成了乱走
        /// （任务 09 第 4/5 步"20 秒只挪半米"就是这么来的），所以加了距离限制。
        /// 该去掉：寻路按格子做冲突消解（谁先到谁走、另一个等一拍），而不是靠横向偏移绕。
        /// </summary>
        public const float PassByRadius = 3f;

        /// <summary>另一个厨师在这个距离内才算"可能互推"，才允许侧向错身。</summary>
        public const float CrowdedRadius = 1.2f;
    }
}
