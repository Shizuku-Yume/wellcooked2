using System.Collections;
using System.Reflection;
using BepInEx.Logging;
using HarmonyLib;
using UnityEngine;

namespace WellCooked
{
    /// <summary>
    /// 帧步进：`step=<n>` 先暂停游戏，再精确推进 n 帧，然后停在暂停状态。
    /// 调试时用来把"某一步之后世界变成了什么样"固定下来，不用靠反应速度点暂停。
    ///
    /// 暂停不能只写 `Time.timeScale`：游戏的 `TimeManager.Update()` 每帧都会把
    /// `Time.timeScale` 重置成它的 `m_debugtimeScale`（反编译证据，实测 step 后
    /// `Time.timeScale` 仍是 1.00）。所以这里改的是 `TimeManager.m_debugtimeScale`，
    /// 同时同步 `Time.timeScale`，让暂停在当前帧就生效。
    /// </summary>
    public static class FrameStep
    {
        private static ManualLogSource _log;
        private static MonoBehaviour _host;
        private static Coroutine _running;
        private static int _pending = -1;
        private static float _savedScale = 1f;
        private static bool _ownsPause;

        private static object _timeManager;
        private static FieldInfo _debugScale;

        public static void Install(ManualLogSource log)
        {
            _log = log;
        }

        /// <summary>`step=<n>`：n 帧后停在暂停；`step=0` 恢复暂停前的 timeScale。</summary>
        public static void Request(string value)
        {
            int frames;
            if (!int.TryParse(value, out frames) || frames < 0)
            {
                _log.LogWarning("step: 参数要写成非负整数，收到 " + value);
                return;
            }

            _pending = frames;
            _log.LogInfo("step: 收到 step=" + frames + (frames == 0 ? "（恢复）" : ""));
        }

        /// <summary>由 `SmokePlugin.Update` 每帧调用：把命令通道收到的请求变成协程。</summary>
        public static void Tick(MonoBehaviour host)
        {
            _host = host;
            if (_pending < 0 || _host == null)
            {
                return;
            }

            int frames = _pending;
            _pending = -1;

            if (_running != null)
            {
                _host.StopCoroutine(_running);
                _running = null;
            }

            if (frames == 0)
            {
                // 只恢复我们自己按下的暂停；游戏本来就在暂停（比如打开菜单）时不动它。
                if (_ownsPause)
                {
                    SetScale(_savedScale);
                    _ownsPause = false;
                    _log.LogInfo("step: 恢复 timeScale=" + ReadScale().ToString("F2"));
                }
                else
                {
                    _log.LogInfo("step: 没有被 step 暂停，保持 timeScale=" + ReadScale().ToString("F2"));
                }

                return;
            }

            // 连续 step 时不要把"已经暂停的 0"当成原始倍率；只记录第一次接管前的值。
            if (!_ownsPause)
            {
                _savedScale = ReadScale();
                _ownsPause = true;
            }

            _running = _host.StartCoroutine(Run(frames));
        }

        private static IEnumerator Run(int frames)
        {
            SetScale(0f);
            _log.LogInfo("step: 已暂停，准备推进 " + frames + " 帧（原 timeScale="
                         + _savedScale.ToString("F2") + "，Time.time=" + Time.time.ToString("F3")
                         + " frame=" + Time.frameCount + "）");

            for (int i = 0; i < frames; i++)
            {
                // 先把倍率放到 1，等这一帧结束；下一帧从帧首就按 1 计算 Time.deltaTime，
                // 整帧跑完后再放回 0。只 yield 一次的话，放 1 和放 0 都发生在同一帧，
                // 下一帧开头仍是 0，Time.time 不会前进（实测 frame+1 但 Time.time 不变）。
                SetScale(1f);
                yield return new WaitForEndOfFrame();
                yield return new WaitForEndOfFrame();
                SetScale(0f);
            }

            _running = null;
            _log.LogInfo("step: 已推进 " + frames + " 帧，游戏保持暂停；Time.time="
                         + Time.time.ToString("F3") + " frame=" + Time.frameCount + "；step=0 恢复");
        }

        /// <summary>
        /// 游戏的时间倍率真正由 `TimeManager.m_debugtimeScale` 决定（它的 Update 每帧
        /// 覆盖 `Time.timeScale`）。拿不到这个私有字段时退回 `Time.timeScale`，并在日志里说。
        /// </summary>
        private static float ReadScale()
        {
            RefreshManager();
            if (_timeManager != null && _debugScale != null)
            {
                return (float)_debugScale.GetValue(_timeManager);
            }

            return Time.timeScale;
        }

        private static void SetScale(float scale)
        {
            RefreshManager();
            if (_timeManager != null && _debugScale != null)
            {
                _debugScale.SetValue(_timeManager, scale);
            }

            Time.timeScale = scale;
        }

        private static void RefreshManager()
        {
            if (_timeManager != null && _debugScale != null)
            {
                return;
            }

            var type = AccessTools.TypeByName("TimeManager");
            if (type == null)
            {
                return;
            }

            var instance = type.GetField("s_timeManager", BindingFlags.Static | BindingFlags.NonPublic);
            var debugScale = type.GetField("m_debugtimeScale", BindingFlags.Instance | BindingFlags.NonPublic);
            if (instance == null || debugScale == null)
            {
                return;
            }

            var manager = instance.GetValue(null);
            if (manager == null)
            {
                return;
            }

            _timeManager = manager;
            _debugScale = debugScale;
            _log.LogInfo("step: 拿到 TimeManager.m_debugtimeScale，暂停走游戏自己的时间倍率");
        }
    }
}
