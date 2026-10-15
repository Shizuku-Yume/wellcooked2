using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Text;
using BepInEx;
using BepInEx.Logging;
using HarmonyLib;
using UnityEngine;

namespace WellCooked
{
    /// <summary>
    /// 订单可复现：按 `BepInEx/wellcooked-orders.json` 指定每个阶段出哪道菜，并把这一局的随机源固定。
    /// 方案 §4.3 的策略对比要求"所有策略使用相同地图、订单、时间限制"，游戏自己随机出单做不到这一点。
    ///
    /// 做法借自两个 MIT 参考项目（只借这两处做法，实现自己写，证据与判定见 `.workflow/maps/prior-art.md`）：
    /// - `gua248/Overcooked2-TAS`（MIT，`Copyright (c) [year] [fullname]`，版权行为仓库原文）的
    ///   `plugin/Patch.cs:189-241`：Harmony Prefix 接管 `RoundData.GetNextRecipe` /
    ///   `DynamicRoundData.GetNextRecipe`，按"这是本阶段第几单"返回指定菜谱，并同步游戏的
    ///   `RecipeCount` 与 `CumulativeFrequencies`。
    /// - `toasterparty/oc2-modding`（MIT，`Copyright (c) 2022 toasterparty`）的
    ///   `OC2Modding/FixedRNG.cs:59-140`：回合开始时把 `UnityEngine.Random` 固定到一个 seed。
    ///
    /// 它只决定"第几阶段出哪道菜"，不决定订单何时生成——到达时刻由订单控制器掌握，见
    /// `decisions.md: 2026-10-15 - 固定菜单复用 TAS 的 GetNextRecipe Prefix`。
    /// </summary>
    public static class Orders
    {
        /// <summary>一个阶段（静态关卡只有一个）要按顺序出的菜名。</summary>
        [Serializable]
        public class Phase
        {
            public string[] recipes;
        }

        /// <summary>`BepInEx/wellcooked-orders.json` 的结构。`seed &lt; 0` 表示不固定随机源。</summary>
        [Serializable]
        public class Config
        {
            public int seed = -1;
            public Phase[] menu;
        }

        private static ManualLogSource _log;
        private static Config _config;
        private static string _configPath;
        private static bool _noted;      // 本局是否已经记过"接管 / 不接管"那一行
        private static bool _warnedNoConfig;
        private static bool _warnedExhausted;
        private static int _generated;   // 本局已按配置生成的单数
        private static int _phase = -1;  // 当前阶段（静态关卡恒为 0）
        private static int _phaseBase;   // 进入当前阶段时 CumulativeFrequencies 的和
        private static readonly List<object> _menu = new List<object>();   // 当前关卡的菜谱表（`menu=` 导出用）

        public static void Install(ManualLogSource log)
        {
            _log = log;
            _configPath = Path.Combine(Paths.BepInExRootPath, "wellcooked-orders.json");
            Reload();

            var harmony = new Harmony(SmokePlugin.Guid + ".orders");
            Patch(harmony, typeof(RoundData), "GetNextRecipe", nameof(BeforeRoundDataGetNextRecipe), null);
            Patch(harmony, typeof(DynamicRoundData), "GetNextRecipe", nameof(BeforeDynamicRoundDataGetNextRecipe), null);
            Patch(harmony, typeof(RoundData), "InitialiseRound", nameof(BeforeRoundDataInitialiseRound), null);
            Patch(harmony, typeof(DynamicRoundData), "InitialiseRound", nameof(BeforeDynamicRoundDataInitialiseRound), null);
        }

        /// <summary>重新读一次配置文件（`orders=reload` 用）。</summary>
        public static void Reload()
        {
            _config = null;
            _warnedNoConfig = false;

            if (!File.Exists(_configPath))
            {
                _log.LogInfo("订单: 没有 " + _configPath + "，用游戏自己的随机订单");
                return;
            }

            try
            {
                _config = Parse(File.ReadAllText(_configPath));
            }
            catch (Exception e)
            {
                _log.LogError("订单: 解析 " + _configPath + " 失败（" + e.Message + "），用游戏自己的随机订单");
                _config = null;
                return;
            }

            if (_config == null || _config.menu == null || _config.menu.Length == 0)
            {
                _log.LogWarning("订单: " + _configPath + " 里没有 menu，用游戏自己的随机订单");
                _config = null;
                return;
            }

            var parts = new List<string>();
            for (int i = 0; i < _config.menu.Length; i++)
            {
                var recipes = _config.menu[i] == null ? null : _config.menu[i].recipes;
                parts.Add("阶段 " + i + " = " + (recipes == null ? "(空)" : string.Join(", ", recipes)));
            }

            _log.LogInfo("订单: 已读 " + _configPath + "，seed=" + (_config.seed < 0 ? "不固定" : _config.seed.ToString())
                         + "，" + string.Join("；", parts.ToArray()));
        }

        /// <summary>记下这一关的菜谱表（分阶段关卡每个阶段各有一份，按菜名去重）。</summary>
        private static void SetMenu(object list)
        {
            var entries = list == null ? null : list.GetType().GetField("m_recipes")?.GetValue(list) as Array;
            if (entries == null)
            {
                return;
            }

            for (int i = 0; i < entries.Length; i++)
            {
                var entry = entries.GetValue(i);
                if (entry == null || _menu.Contains(entry))
                {
                    continue;
                }

                var dish = DishName(entry);
                if (dish != "(null)" && _menu.Exists(delegate(object e) { return DishName(e) == dish; }))
                {
                    continue;
                }

                _menu.Add(entry);
            }
        }

        /// <summary>
        /// `menu=1` 指令：把当前关卡的整张菜谱表写成 `BepInEx/wellcooked-menu.json`
        /// （`config/recipes.json` 的输入）。菜谱表来自游戏自己的 `RecipeList`，不是我们编的。
        /// </summary>
        public static void DumpMenu()
        {
            if (_menu.Count == 0)
            {
                _log.LogWarning("订单: 还没拿到关卡菜谱表——先进关卡（`load=` 或 `restart=`）再发 menu=1");
                return;
            }

            var seen = new List<string>();
            var dishes = new List<string>();
            for (int i = 0; i < _menu.Count; i++)
            {
                var described = WorldState.DescribeRecipe(_menu[i]);
                if (described == null)
                {
                    continue;
                }

                dishes.Add(described);
            }

            var text = new StringBuilder();
            text.Append("{\"scene\":\"").Append(WorldState.ActiveScene()).Append('"');
            text.Append(",\"dishes\":[").Append(string.Join(",", dishes.ToArray())).Append("]}");
            var path = Path.Combine(Paths.BepInExRootPath, "wellcooked-menu.json");
            File.WriteAllText(path, text.ToString());
            _log.LogInfo("订单: 菜谱表 " + dishes.Count + " 道菜写到 " + path);
        }

        /// <summary>`orders=` 指令：打印当前配置与已生成几单。</summary>
        public static void Describe()
        {
            if (_config == null)
            {
                _log.LogInfo("订单: 当前没有配置（" + _configPath + " 不存在或为空），本局已生成 " + _generated + " 单");
                return;
            }

            _log.LogInfo("订单: seed=" + (_config.seed < 0 ? "不固定" : _config.seed.ToString())
                         + "，阶段数 " + _config.menu.Length + "，本局已生成 " + _generated + " 单，当前阶段 " + _phase);
            for (int i = 0; i < _config.menu.Length; i++)
            {
                var recipes = _config.menu[i] == null ? null : _config.menu[i].recipes;
                _log.LogInfo("订单:   阶段 " + i + " = " + (recipes == null ? "(空)" : string.Join(", ", recipes)));
            }
        }

        /// <summary>
        /// 自己解析这份小配置，不用 `JsonUtility`：实测在 Unity 2017.4 下它把 `menu` 解成 null
        /// （日志 `订单: ... 里没有 menu`），而这份格式只有"整数 + 字符串数组"两种值。
        /// 只认需要的键，其它键跳过（跳过时按括号配对，不会把后面的内容吃掉）。
        /// </summary>
        private static Config Parse(string text)
        {
            int i = 0;
            var config = new Config();
            var phases = new List<Phase>();

            SkipSpace(text, ref i);
            Expect(text, ref i, '{');
            while (true)
            {
                SkipSpace(text, ref i);
                if (Peek(text, i) == '}')
                {
                    i++;
                    break;
                }

                string key = ReadString(text, ref i);
                SkipSpace(text, ref i);
                Expect(text, ref i, ':');
                SkipSpace(text, ref i);

                if (key == "seed")
                {
                    config.seed = ReadInt(text, ref i);
                }
                else if (key == "menu")
                {
                    Expect(text, ref i, '[');
                    SkipSpace(text, ref i);
                    while (Peek(text, i) != ']')
                    {
                        phases.Add(ReadPhase(text, ref i));
                        SkipSpace(text, ref i);
                        if (Peek(text, i) == ',')
                        {
                            i++;
                            SkipSpace(text, ref i);
                        }
                    }

                    i++;
                }
                else
                {
                    SkipValue(text, ref i);
                }

                SkipSpace(text, ref i);
                if (Peek(text, i) == ',')
                {
                    i++;
                    continue;
                }

                if (Peek(text, i) == '}')
                {
                    i++;
                    break;
                }

                throw new FormatException("位置 " + i + " 期望 , 或 }");
            }

            config.menu = phases.ToArray();
            return config;
        }

        /// <summary>一个阶段：`{ "recipes": ["菜名", ...] }`。</summary>
        private static Phase ReadPhase(string text, ref int i)
        {
            var phase = new Phase();
            SkipSpace(text, ref i);
            Expect(text, ref i, '{');
            while (true)
            {
                SkipSpace(text, ref i);
                if (Peek(text, i) == '}')
                {
                    i++;
                    break;
                }

                string key = ReadString(text, ref i);
                SkipSpace(text, ref i);
                Expect(text, ref i, ':');
                SkipSpace(text, ref i);

                if (key == "recipes")
                {
                    phase.recipes = ReadStringArray(text, ref i);
                }
                else
                {
                    SkipValue(text, ref i);
                }

                SkipSpace(text, ref i);
                if (Peek(text, i) == ',')
                {
                    i++;
                    continue;
                }

                if (Peek(text, i) == '}')
                {
                    i++;
                    break;
                }

                throw new FormatException("位置 " + i + " 期望 , 或 }");
            }

            return phase;
        }

        private static string[] ReadStringArray(string text, ref int i)
        {
            var values = new List<string>();
            Expect(text, ref i, '[');
            SkipSpace(text, ref i);
            while (Peek(text, i) != ']')
            {
                values.Add(ReadString(text, ref i));
                SkipSpace(text, ref i);
                if (Peek(text, i) == ',')
                {
                    i++;
                    SkipSpace(text, ref i);
                }
            }

            i++;
            return values.ToArray();
        }

        private static string ReadString(string text, ref int i)
        {
            SkipSpace(text, ref i);
            Expect(text, ref i, '"');
            var sb = new StringBuilder();
            while (i < text.Length)
            {
                char c = text[i++];
                if (c == '"')
                {
                    return sb.ToString();
                }

                if (c != '\\')
                {
                    sb.Append(c);
                    continue;
                }

                char escape = i < text.Length ? text[i++] : '\0';
                switch (escape)
                {
                    case 'n': sb.Append('\n'); break;
                    case 't': sb.Append('\t'); break;
                    case 'r': sb.Append('\r'); break;
                    case 'b': sb.Append('\b'); break;
                    case 'f': sb.Append('\f'); break;
                    case 'u':
                        if (i + 4 > text.Length)
                        {
                            throw new FormatException("位置 " + i + " 的 \\u 转义不完整");
                        }

                        sb.Append((char)Convert.ToInt32(text.Substring(i, 4), 16));
                        i += 4;
                        break;
                    default: sb.Append(escape); break;
                }
            }

            throw new FormatException("字符串没有收尾的引号");
        }

        private static int ReadInt(string text, ref int i)
        {
            SkipSpace(text, ref i);
            int start = i;
            if (i < text.Length && (text[i] == '-' || text[i] == '+'))
            {
                i++;
            }

            while (i < text.Length && text[i] >= '0' && text[i] <= '9')
            {
                i++;
            }

            if (i == start)
            {
                throw new FormatException("位置 " + start + " 期望整数");
            }

            return int.Parse(text.Substring(start, i - start));
        }

        /// <summary>跳过不认识的键的值（字符串 / 数字 / 对象 / 数组 / 字面量），按括号配对。</summary>
        private static void SkipValue(string text, ref int i)
        {
            SkipSpace(text, ref i);
            if (i >= text.Length)
            {
                throw new FormatException("值的位置超出文件末尾");
            }

            if (text[i] == '"')
            {
                ReadString(text, ref i);
                return;
            }

            if (text[i] == '{' || text[i] == '[')
            {
                char open = text[i];
                char close = open == '{' ? '}' : ']';
                int depth = 0;
                while (i < text.Length)
                {
                    char c = text[i];
                    if (c == '"')
                    {
                        ReadString(text, ref i);
                        continue;
                    }

                    if (c == open)
                    {
                        depth++;
                    }
                    else if (c == close)
                    {
                        depth--;
                        if (depth == 0)
                        {
                            i++;
                            return;
                        }
                    }

                    i++;
                }

                throw new FormatException("括号没有配对");
            }

            while (i < text.Length && text[i] != ',' && text[i] != '}' && text[i] != ']')
            {
                i++;
            }
        }

        private static char Peek(string text, int i)
        {
            return i < text.Length ? text[i] : '\0';
        }

        private static void SkipSpace(string text, ref int i)
        {
            while (i < text.Length && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' || text[i] == '\r'))
            {
                i++;
            }
        }

        private static void Expect(string text, ref int i, char expected)
        {
            if (Peek(text, i) != expected)
            {
                throw new FormatException("位置 " + i + " 期望 " + expected);
            }

            i++;
        }

        private static void Patch(Harmony harmony, Type type, string method, string prefix, string postfix)
        {
            var target = AccessTools.Method(type, method);
            if (target == null)
            {
                _log.LogWarning("订单: 找不到 " + type.Name + "." + method + "，订单不会固定");
                return;
            }

            var pre = new HarmonyMethod(Static(prefix));
            var post = postfix == null ? null : new HarmonyMethod(Static(postfix));
            harmony.Patch(target, pre, post);
        }

        private static MethodInfo Static(string name)
        {
            return typeof(Orders).GetMethod(name, BindingFlags.Static | BindingFlags.NonPublic | BindingFlags.Public);
        }

        /// <summary>回合开始（RoundData 路径）：记菜谱表、清计数、固定随机源。</summary>
        private static void BeforeRoundDataInitialiseRound(RoundData __instance)
        {
            SetMenu(__instance.m_recipes);
            ResetRound();
        }

        /// <summary>回合开始（动态关卡路径）：菜谱表按阶段收集。</summary>
        private static void BeforeDynamicRoundDataInitialiseRound(DynamicRoundData __instance)
        {
            if (__instance.Phases != null)
            {
                for (int i = 0; i < __instance.Phases.Length; i++)
                {
                    SetMenu(__instance.Phases[i].Recipes);
                }
            }

            ResetRound();
        }

        /// <summary>回合开始：计数清零，随机源按配置固定（`seed &lt; 0` 时不碰）。</summary>
        private static void ResetRound()
        {
            _generated = 0;
            _phase = -1;
            _phaseBase = 0;
            _noted = false;
            _warnedExhausted = false;

            if (_config == null || _config.seed < 0)
            {
                return;
            }

            UnityEngine.Random.InitState(_config.seed);
            _log.LogInfo("订单: 本局随机源固定 seed=" + _config.seed);
        }

        private static bool BeforeRoundDataGetNextRecipe(RoundData __instance, ref RecipeList.Entry[] __result, RoundInstanceDataBase _data)
        {
            return RoundDataAccess.Take(__instance, _data, ref __result);
        }

        private static bool BeforeDynamicRoundDataGetNextRecipe(DynamicRoundData __instance, ref RecipeList.Entry[] __result, RoundInstanceDataBase _data)
        {
            return DynamicRoundDataAccess.Take(__instance, _data, ref __result);
        }

        /// <summary>
        /// `RoundData` 的嵌套实例类型是 protected，只有派生类里看得见——所以补丁体放在空壳派生类里
        /// （TAS 的 `FixedMenuRoundData` 就是这个用途；这两个类不实例化，只用来获得访问权）。
        /// </summary>
        private class RoundDataAccess : RoundData
        {
            public static bool Take(RoundData roundData, RoundInstanceDataBase raw, ref RecipeList.Entry[] result)
            {
                var instance = raw as RoundData.RoundInstanceData;
                if (instance == null)
                {
                    Note(false, "实例不是 RoundData.RoundInstanceData", raw);
                    return true;
                }

                // 剧本关卡自带一串固定订单（`m_manualOrder`），没走完之前不插手，否则会把它的剧本顶掉。
                var scripted = roundData as ScriptedRoundData;
                if (scripted != null && scripted.m_manualOrder != null
                    && instance.RecipeCount < scripted.m_manualOrder.Length)
                {
                    Note(false, "剧本关卡自带 " + scripted.m_manualOrder.Length + " 单手动订单，还没走完", raw);
                    return true;
                }

                Note(true, null, raw);
                SetMenu(roundData.m_recipes);
                int recipeCount = instance.RecipeCount;
                return TakeNext(0, roundData.m_recipes, instance.CumulativeFrequencies, recipeCount,
                                delegate(int recipeIndex)
                                {
                                    instance.RecipeCount++;
                                    instance.CumulativeFrequencies[recipeIndex]++;
                                },
                                ref result);
            }
        }

        /// <summary>动态关卡（分阶段换菜谱）：阶段下标与菜谱表同样藏在 protected 嵌套类型里。</summary>
        private class DynamicRoundDataAccess : DynamicRoundData
        {
            public static bool Take(DynamicRoundData roundData, RoundInstanceDataBase raw, ref RecipeList.Entry[] result)
            {
                var instance = raw as DynamicRoundData.DynamicRoundInstanceData;
                if (instance == null)
                {
                    Note(false, "实例不是 DynamicRoundData.DynamicRoundInstanceData", raw);
                    return true;
                }

                if (roundData.Phases == null)
                {
                    Note(false, "动态关卡没有阶段表", raw);
                    return true;
                }

                int phase = instance.CurrentPhase;
                if (phase < 0 || phase >= roundData.Phases.Length)
                {
                    Note(false, "阶段下标 " + phase + " 越界（共 " + roundData.Phases.Length + " 个阶段）", raw);
                    return true;
                }

                Note(true, null, raw);
                SetMenu(roundData.Phases[phase].Recipes);
                int recipeCount = instance.RecipeCount;
                return TakeNext(phase, roundData.Phases[phase].Recipes, instance.CumulativeFrequencies, recipeCount,
                                delegate(int recipeIndex)
                                {
                                    instance.RecipeCount++;
                                    instance.CumulativeFrequencies[recipeIndex]++;
                                },
                                ref result);
            }
        }

        /// <summary>
        /// 每局第一次碰到 `GetNextRecipe` 时记一行：接管还是不接管、为什么不接管、实例的真实类型。
        /// 没有这行的话，"补丁没生效"和"这关的订单不走这个接口"在日志里长得一样。
        /// </summary>
        private static void Note(bool intercept, string why, object raw)
        {
            if (_noted)
            {
                return;
            }

            _noted = true;
            string type = raw == null ? "null" : raw.GetType().Name;
            if (intercept)
            {
                _log.LogInfo("订单: 接管 GetNextRecipe（订单实例 " + type + "）");
            }
            else
            {
                _log.LogInfo("订单: 不接管 GetNextRecipe（" + why + "，订单实例 " + type + "），这一局用游戏自己的订单");
            }
        }

        /// <summary>
        /// 本阶段第几单 → 配置里的菜名 → 本关菜谱下标。返回 false 表示"已经给了结果，别跑原方法"。
        /// </summary>
        private static bool TakeNext(int phase, RecipeList list, int[] frequencies, int recipeCount,
                                     Action<int> markChosen, ref RecipeList.Entry[] result)
        {
            if (_config == null || list == null || list.m_recipes == null || frequencies == null)
            {
                WarnNoConfigOnce();
                return true;
            }

            if (phase < 0 || phase >= _config.menu.Length || _config.menu[phase] == null || _config.menu[phase].recipes == null)
            {
                WarnExhaustedOnce("配置里没有阶段 " + phase + " 的序列");
                return true;
            }

            // 阶段计数：CumulativeFrequencies 是每个菜被选过几次，和就是本阶段已出的单数；
            // 动态关卡跨阶段不清零，所以减去进入本阶段时的基数。
            int total = 0;
            for (int i = 0; i < frequencies.Length; i++)
            {
                total += frequencies[i];
            }

            if (phase != _phase)
            {
                _phase = phase;
                _phaseBase = total;
            }

            int index = total - _phaseBase;
            var sequence = _config.menu[phase].recipes;
            if (index >= sequence.Length)
            {
                WarnExhaustedOnce("阶段 " + phase + " 的序列用完（" + sequence.Length + " 单）");
                return true;
            }

            string name = sequence[index];
            int recipeIndex = Resolve(list, name, index, phase);
            if (recipeIndex < 0)
            {
                return true;
            }

            markChosen(recipeIndex);
            result = new RecipeList.Entry[] { list.m_recipes[recipeIndex] };
            _generated++;
            _log.LogInfo("订单: 阶段 " + phase + " 第 " + (index + 1) + "/" + sequence.Length + " 单 = " + name
                         + "（菜谱下标 " + recipeIndex + "，累计生成 " + _generated + " 单，原 RecipeCount=" + recipeCount + "）");
            return false;
        }

        /// <summary>菜名 → 本关菜谱下标；找不到就报错并列出可用菜名。</summary>
        private static int Resolve(RecipeList list, string name, int index, int phase)
        {
            var entries = list.m_recipes;
            for (int i = 0; i < entries.Length; i++)
            {
                if (string.Equals(DishName(entries[i]), name, StringComparison.OrdinalIgnoreCase))
                {
                    return i;
                }
            }

            var available = new List<string>();
            for (int i = 0; i < entries.Length; i++)
            {
                available.Add(i + ":" + DishName(entries[i]));
            }

            _log.LogError("订单: 阶段 " + phase + " 第 " + (index + 1) + " 单的菜名 \"" + name
                          + "\" 不在本关菜谱里（本单回到游戏自己的随机订单）。本关可用菜名：" + string.Join(", ", available.ToArray()));
            return -1;
        }

        /// <summary>菜名取 `RecipeList.Entry.m_order`（ScriptableObject）的 name，和 WorldState 里的读法一致。</summary>
        private static string DishName(object entry)
        {
            if (entry == null)
            {
                return "(null)";
            }

            var order = WorldState.Get(entry, "m_order");
            var unity = order as UnityEngine.Object;
            if (unity != null)
            {
                return unity.name;
            }

            return order == null ? "(无名)" : order.ToString();
        }

        private static void WarnNoConfigOnce()
        {
            if (_warnedNoConfig)
            {
                return;
            }

            _warnedNoConfig = true;
            _log.LogInfo("订单: 没有可用配置，本局用游戏自己的随机订单");
        }

        private static void WarnExhaustedOnce(string why)
        {
            if (_warnedExhausted)
            {
                return;
            }

            _warnedExhausted = true;
            _log.LogInfo("订单: " + why + "，之后回到游戏自己的随机订单");
        }
    }
}
