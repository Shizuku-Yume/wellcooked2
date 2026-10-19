using System;
using System.IO;
using BepInEx;

namespace WellCooked
{
    /// <summary>
    /// 启动自检：不依赖 BepInEx 的 Logger，直接往文件追加行。
    /// 用来定位插件到底有没有被实例化、Awake 挂在哪一步。
    ///
    /// 写文件路径会依次尝试：BepInEx 目录 → 进程当前目录（游戏根）→ 相对路径，
    /// 任何一步成功就固定下来；全部失败也只吞异常，绝不影响游戏。
    /// </summary>
    public static class BootTrace
    {
        private const string FileName = "wellcooked-boot.txt";

        private static readonly object Gate = new object();
        private static string _path;

        private static string Resolve()
        {
            if (_path != null)
            {
                return _path;
            }

            // 1) BepInEx 根目录
            try
            {
                string root = Paths.BepInExRootPath;
                if (!string.IsNullOrEmpty(root) && Directory.Exists(root))
                {
                    string candidate = System.IO.Path.Combine(root, FileName);
                    File.AppendAllText(candidate, string.Empty);
                    _path = candidate;
                    return _path;
                }
            }
            catch
            {
                // 继续尝试下一个
            }

            // 2) 进程当前目录（Unity 启动时就是游戏根目录）
            try
            {
                string candidate = System.IO.Path.Combine(Directory.GetCurrentDirectory(), FileName);
                File.AppendAllText(candidate, string.Empty);
                _path = candidate;
                return _path;
            }
            catch
            {
                // 继续尝试下一个
            }

            // 3) 相对路径兜底
            _path = FileName;
            return _path;
        }

        public static void Write(string message)
        {
            try
            {
                lock (Gate)
                {
                    File.AppendAllText(Resolve(),
                        DateTime.Now.ToString("HH:mm:ss.fff") + "  " + message + Environment.NewLine);
                }
            }
            catch
            {
                // 自检本身绝不能再抛异常
            }
        }

        public static void Step(string name)
        {
            Write("[step] " + name);
        }
    }
}
