using System;
using System.Text;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Reflection;
using BepInEx;
using BepInEx.Logging;
using HarmonyLib;
using UnityEngine;

namespace WellCooked
{
    /// <summary>
    /// 调试指令通道：往游戏目录写 `BepInEx/wellcooked-command.txt`，插件 1 秒内执行并删掉文件。
    ///
    /// 为什么要它：调试时人在终端、游戏在后台，不可能每次都手动点菜单进关卡，
    /// 也不该为了看一眼状态去截图。有了这个通道，"重进关""看某类型有哪些方法"
    /// "看暂停状态"都能一条命令做完，日志里留证据。
    ///
    /// 支持的指令（一行一条，`#` 开头是注释）：
    ///   load=&lt;场景名&gt;   直接加载关卡场景（如 s_sushi_1_1）
    ///   restart=1        重新加载当前场景（相当于原地重进这一关）
    ///   continue=1       标题画面的"继续游戏"：加载存档档位，之后 load 才是正常进关
    ///   engage=1         标题画面：直接替我们的占位完成"加入"（等价于按 A）
    ///   dump=&lt;类型名&gt;    把这个类型的所有方法签名打到日志
    ///   pause=1          打当前暂停状态（TimeManager 各层）
    ///   action=&lt;JSON&gt;    把一行 action 按线上格式交给 ActionDriver（见 LinkRunner.HandleAction）
    ///   path_act=…/face_act=…/work_act=…   动作调试的简写
    ///   action_test=1    在已占的玩家位上自测"同 seq 追加"与"work 按到下一条动作"，
    ///                    结果写 `action_selftest PASS/FAIL ...`；不需要 Bridge
    ///   state=1          把当前 `state_update` 的完整 JSON 打进日志（核对 facing/progress 等字段）
    ///   pause_game=1/0   走游戏的 TimeManager.SetPaused(Main)，真正冻结游戏时钟
    /// </summary>
    public static class Commands
    {
        private static ManualLogSource _log;
        private static float _nextPoll;

        public static void Install(ManualLogSource log)
        {
            _log = log;
        }

        public static void Tick()
        {
            var now = Time.realtimeSinceStartup;
            if (now < _nextPoll)
            {
                return;
            }

            _nextPoll = now + 1f;

            string path = Path.Combine(Paths.BepInExRootPath, "wellcooked-command.txt");
            if (!File.Exists(path))
            {
                return;
            }

            string text;
            try
            {
                text = File.ReadAllText(path);
            }
            catch (Exception e)
            {
                _log.LogWarning("指令: 读不到 " + path + " " + e.Message);
                return;
            }

            try
            {
                File.Delete(path);
            }
            catch (Exception e)
            {
                _log.LogWarning("指令: 删不掉 " + path + " " + e.Message);
            }

            foreach (var raw in text.Split('\n'))
            {
                var line = raw.Trim();
                if (line.Length == 0 || line[0] == '#')
                {
                    continue;
                }

                if (line[0] == '{')
                {
                    try
                    {
                        RunActionJson(line);
                    }
                    catch (Exception e)
                    {
                        _log.LogError("指令 JSON 失败: " + e);
                    }
                    continue;
                }

                int eq = line.IndexOf('=');
                string key = (eq < 0 ? line : line.Substring(0, eq)).Trim();
                string value = eq < 0 ? "" : line.Substring(eq + 1).Trim();
                try
                {
                    Run(key, value);
                }
                catch (Exception e)
                {
                    _log.LogError("指令 " + line + " 失败: " + e);
                }
            }
        }

        private static void Run(string key, string value)
        {
            switch (key)
            {
                case "load":
                    LoadLevel(value);
                    break;
                case "restart":
                    LoadLevel(UnityEngine.SceneManagement.SceneManager.GetActiveScene().name);
                    break;
                case "engage":
                    Engage();
                    break;
                case "continue":
                    ContinueFromTitle();
                    break;
                case "session":
                    StartSession(value);
                    break;
                case "path":
                    ProbePath(value);
                    break;
                case "order":
                    DumpOrder();
                    break;
                case "menu":
                    Orders.DumpMenu();
                    break;
                case "orders":
                    if (value == "reload")
                    {
                        Orders.Reload();
                    }

                    Orders.Describe();
                    break;
                case "levels":
                    DumpLevels(value);
                    break;
                case "unlock":
                    UnlockAllLevels();
                    break;
                case "mech":
                    DumpMechanics();
                    break;
                case "scan":
                    Scan(value);
                    break;
                case "probe":
                    Probe(value);
                    break;
                case "dump":
                    DumpType(value);
                    break;
                case "pause":
                    DumpPause();
                    break;
                case "action":
                    RunActionJson(value);
                    break;
                case "path_act":
                    RunPathAct(value);
                    break;
                case "face_act":
                    RunFaceAct(value);
                    break;
                case "work_act":
                    RunWorkAct(value);
                    break;
                case "action_test":
                    ActionSelftest();
                    break;
                case "state":
                    _log.LogInfo("state_frame: " + StateFrame.Build(Time.realtimeSinceStartup));
                    break;
                case "pause_game":
                    SetGamePaused(value == "1" || value == "true");
                    break;
                default:
                    _log.LogWarning("指令: 不认识 " + key);
                    break;
            }
        }

        private static void RunActionJson(string json)
        {
            var m = UnityEngine.JsonUtility.FromJson<InboundMessage>(json);
            if (m != null)
            {
                LinkRunner.HandleAction(m, json);
            }
        }

        private static void RunPathAct(string value)
        {
            var parts = value.Split(':');
            if (parts.Length < 2)
            {
                _log.LogWarning("指令: path_act 格式 chef_id:x1,y1;x2,y2...[:seq]");
                return;
            }
            int chefId = int.Parse(parts[0]);
            var wpCoords = parts[1].Split(';');
            var wps = new List<Vec2>();
            foreach (var coord in wpCoords)
            {
                var xy = coord.Split(',');
                if (xy.Length == 2)
                {
                    wps.Add(new Vec2 { x = float.Parse(xy[0], CultureInfo.InvariantCulture), y = float.Parse(xy[1], CultureInfo.InvariantCulture) });
                }
            }
            long seq = parts.Length > 2 ? long.Parse(parts[2]) : 1L;
            var m = new InboundMessage
            {
                type = MsgType.Action,
                chef_id = chefId,
                action = MsgType.Path,
                seq = seq,
                waypoints = wps.ToArray()
            };
            LinkRunner.HandleAction(m, null);
        }

        private static void RunFaceAct(string value)
        {
            var parts = value.Split(':');
            if (parts.Length < 2)
            {
                _log.LogWarning("指令: face_act 格式 chef_id:x,y 或 chef_id:target_object");
                return;
            }
            int chefId = int.Parse(parts[0]);
            var xy = parts[1].Split(',');
            var m = new InboundMessage
            {
                type = MsgType.Action,
                chef_id = chefId,
                action = MsgType.Face
            };
            float x, y;
            if (xy.Length == 2 && float.TryParse(xy[0], NumberStyles.Float, CultureInfo.InvariantCulture, out x)
                && float.TryParse(xy[1], NumberStyles.Float, CultureInfo.InvariantCulture, out y))
            {
                m.target = new Vec2 { x = x, y = y };
            }
            else
            {
                m.target_object = parts[1];
            }
            LinkRunner.HandleAction(m, null);
        }

        private static void RunWorkAct(string value)
        {
            var parts = value.Split(':');
            if (parts.Length < 2)
            {
                _log.LogWarning("指令: work_act 格式 chef_id:target_object");
                return;
            }
            int chefId = int.Parse(parts[0]);
            var m = new InboundMessage
            {
                type = MsgType.Action,
                chef_id = chefId,
                action = MsgType.Work,
                target_object = parts[1]
            };
            LinkRunner.HandleAction(m, null);
        }

        /// <summary>
        /// 动作执行器的行为自测（不依赖 Bridge，也不需要真人操作）：在已占的玩家位上
        /// 直接调用 `ActionDriver.Handle`，回读登记状态。覆盖两条只有插件侧才有的语义：
        /// 同 seq 的 path 只追加路点、work 一直按到收到下一条动作。
        /// </summary>
        private static void ActionSelftest()
        {
            int slot = -1;
            PlayerControls chef = null;
            for (int i = 0; i < 8; i++)
            {
                PlayerControls candidate;
                if (Takeover.IsOwnedSlot(i) && Takeover.TryChefOf(i, out candidate))
                {
                    slot = i;
                    chef = candidate;
                    break;
                }
            }
            if (slot < 0 || chef == null)
            {
                _log.LogWarning("action_selftest: 没有已占的玩家位与厨师，跳过");
                return;
            }

            var failures = new List<string>();
            Vector3 raw = chef.transform.position;
            var p = new Vector2(raw.x, raw.z);

            Func<int, Vec2> at = offset => new Vec2 { x = p.x + offset * 0.01f, y = p.y };

            ActionDriver.Handle(new InboundMessage
            {
                type = MsgType.Action,
                chef_id = slot,
                action = MsgType.Path,
                seq = 90210L,
                waypoints = new[] { at(0) }
            });
            string state = ActionDriver.DebugState(slot);
            if (!state.Contains("seq=90210") || !state.Contains("waypoints=1"))
            {
                failures.Add("首个 path 未登记: " + state);
            }

            // 同一 seq 的第二条消息带两项（旧项 + 新项）：只应追加到 2，而不是重置。
            ActionDriver.Handle(new InboundMessage
            {
                type = MsgType.Action,
                chef_id = slot,
                action = MsgType.Path,
                seq = 90210L,
                waypoints = new[] { at(0), at(1) }
            });
            state = ActionDriver.DebugState(slot);
            if (!state.Contains("waypoints=2"))
            {
                failures.Add("同 seq 追加失败: " + state);
            }

            // 重复发送同一条（桥每帧重发）不应继续追加。
            ActionDriver.Handle(new InboundMessage
            {
                type = MsgType.Action,
                chef_id = slot,
                action = MsgType.Path,
                seq = 90210L,
                waypoints = new[] { at(0), at(1) }
            });
            state = ActionDriver.DebugState(slot);
            if (!state.Contains("waypoints=2"))
            {
                failures.Add("重复的同 seq 消息被当成新路点: " + state);
            }

            // 新 seq 重置队列。
            ActionDriver.Handle(new InboundMessage
            {
                type = MsgType.Action,
                chef_id = slot,
                action = MsgType.Path,
                seq = 90211L,
                waypoints = new[] { at(0) }
            });
            state = ActionDriver.DebugState(slot);
            if (!state.Contains("seq=90211") || !state.Contains("waypoints=1"))
            {
                failures.Add("新 seq 未重置 path: " + state);
            }

            // 负数 seq 与超过 8 个路点必须被拒。
            if (ActionDriver.Handle(new InboundMessage
            {
                type = MsgType.Action,
                chef_id = slot,
                action = MsgType.Path,
                seq = -1L,
                waypoints = new[] { at(0) }
            }) == null)
            {
                failures.Add("负数 seq 被接受");
            }
            var tooMany = new Vec2[9];
            for (int i = 0; i < tooMany.Length; i++)
            {
                tooMany[i] = at(i);
            }
            if (ActionDriver.Handle(new InboundMessage
            {
                type = MsgType.Action,
                chef_id = slot,
                action = MsgType.Path,
                seq = 90212L,
                waypoints = tooMany
            }) == null)
            {
                failures.Add("9 个路点被接受");
            }

            // work 一直按着，直到下一条动作。
            ActionDriver.Handle(new InboundMessage
            {
                type = MsgType.Action,
                chef_id = slot,
                action = MsgType.Work,
                target_object = "cutting_board_0"
            });
            state = ActionDriver.DebugState(slot);
            if (!state.StartsWith("work", StringComparison.Ordinal))
            {
                failures.Add("work 未登记: " + state);
            }
            ActionDriver.Handle(new InboundMessage
            {
                type = MsgType.Action,
                chef_id = slot,
                action = MsgType.Work,
                target_object = "cutting_board_0"
            });
            if (!ActionDriver.DebugState(slot).StartsWith("work", StringComparison.Ordinal))
            {
                failures.Add("重复的 work 把按住的键松开了");
            }
            ActionDriver.Handle(new InboundMessage
            {
                type = MsgType.Action,
                chef_id = slot,
                action = "stop"
            });
            if (ActionDriver.DebugState(slot) != "none")
            {
                failures.Add("stop 之后 work 仍在: " + ActionDriver.DebugState(slot));
            }

            if (failures.Count == 0)
            {
                _log.LogInfo("action_selftest PASS 玩家位=" + (slot + 1)
                             + " 同seq追加=ok work按住=ok 拒绝非法path=ok");
            }
            else
            {
                _log.LogWarning("action_selftest FAIL " + string.Join("；", failures.ToArray()));
            }
        }

        /// <summary>
        /// 打印关卡表：菜单里的位置（世界 + 关卡标签）↔ 场景名 ↔ 解锁/通关状态。
        /// 挑关卡和"去菜单里找某一关"都靠它（`levels=场景名` 只打匹配的那几条）。
        /// </summary>
        private static void DumpLevels(string filter)
        {
            var session = GameUtils.GetGameSession();
            var progress = session == null ? null : session.Progress;
            if (progress == null)
            {
                _log.LogWarning("levels: 还没进游戏");
                return;
            }

            var directory = progress.GetSceneDirectory();
            if (directory == null)
            {
                _log.LogWarning("levels: 拿不到关卡表");
                return;
            }

            var data = progress.SaveData;
            for (int i = 0; i < directory.Scenes.Length; i++)
            {
                var entry = directory.Scenes[i];
                var varient = entry.GetSceneVarient(2) ?? entry.GetSceneVarient(1);
                string scene = varient == null ? "?" : varient.SceneName;
                if (filter.Length > 0 && scene.IndexOf(filter, StringComparison.OrdinalIgnoreCase) < 0)
                {
                    continue;
                }

                bool unlocked = data.IsLevelUnlocked(i);
                bool completed = data.IsLevelComplete(i);
                _log.LogInfo("levels: [" + i.ToString().PadLeft(3) + "] " + entry.World
                             + "/" + entry.Label + " 场景=" + scene
                             + (unlocked ? " 已解锁" : " 锁着")
                             + (completed ? " 已通关" : "")
                             + (entry.IsHidden ? " 隐藏" : ""));
            }
        }

        /// <summary>
        /// 解锁全部关卡：把存档里每一关标成已通关，再用游戏自己的 `IsLevelUnlocked` 校验一遍。
        ///
        /// 为什么要走存档数据：`GameProgressData.IsLevelUnlocked(i)` 判的是"前一关通关了没"
        /// （`IsLevelChainComplete`），所以把所有关卡标成 `Completed` 就等于全解锁；
        /// 直接改存档文件不行——`.save` 是加密的。
        /// </summary>
        private static void UnlockAllLevels()
        {
            var session = GameUtils.GetGameSession();
            if (session == null)
            {
                _log.LogWarning("解锁: 还没进游戏（拿不到 GameSession）");
                return;
            }

            var progress = session.Progress;
            if (progress == null)
            {
                _log.LogWarning("解锁: 拿不到 GameProgress");
                return;
            }

            var data = progress.SaveableData;
            if (data == null || data.Levels == null || data.Levels.Length == 0)
            {
                _log.LogWarning("解锁: 存档里没有关卡表");
                return;
            }

            int changed = 0;
            foreach (var level in data.Levels)
            {
                if (!level.Completed)
                {
                    changed++;
                }

                level.Completed = true;
                level.Revealed = true;
                level.Purchased = true;
                if (level.ScoreStars < 1)
                {
                    level.ScoreStars = 1;
                }

                if (level.HighScore == int.MinValue)
                {
                    level.HighScore = 0;
                }
            }

            session.SaveSession();
            _log.LogInfo("解锁: 关卡表 " + data.Levels.Length + " 关，本次新置已通关 " + changed
                         + " 关，已存盘（槽位 " + session.SaveSlot + "）");

            int unlocked = 0;
            for (int i = 0; i < data.Levels.Length; i++)
            {
                if (progress.SaveData.IsLevelUnlocked(i))
                {
                    unlocked++;
                }
            }

            _log.LogInfo("解锁: 游戏自己判 IsLevelUnlocked = " + unlocked + "/" + data.Levels.Length);
        }

        /// <summary>
        /// 数当前场景里"真实存在"的机制组件与工位（挑关卡用）。
        ///
        /// 为什么不看物体名字：布景也叫 `city_bridge`/`raft_flower_01`，按名字筛会误判。
        /// 这里直接遍历场景根物体上的组件，按**组件类型名**归类。
        /// </summary>
        private static void DumpMechanics()
        {
            var scene = UnityEngine.SceneManagement.SceneManager.GetActiveScene();
            var mechanics = new SortedDictionary<string, int>(StringComparer.Ordinal);
            var stations = new SortedDictionary<string, int>(StringComparer.Ordinal);
            var roots = scene.GetRootGameObjects();
            foreach (var root in roots)
            {
                foreach (var component in root.GetComponentsInChildren<Component>(true))
                {
                    if (component == null)
                    {
                        continue;
                    }

                    string name = component.GetType().Name;
                    if (name.IndexOf("Conveyor", StringComparison.OrdinalIgnoreCase) >= 0
                        || name.IndexOf("Teleport", StringComparison.OrdinalIgnoreCase) >= 0
                        || name.IndexOf("Portal", StringComparison.OrdinalIgnoreCase) >= 0
                        || name.IndexOf("Ice", StringComparison.OrdinalIgnoreCase) >= 0
                        || name.IndexOf("Bridge", StringComparison.OrdinalIgnoreCase) >= 0
                        || name.IndexOf("Raft", StringComparison.OrdinalIgnoreCase) >= 0
                        || name.IndexOf("Wind", StringComparison.OrdinalIgnoreCase) >= 0
                        || name.IndexOf("Cannon", StringComparison.OrdinalIgnoreCase) >= 0
                        || name.IndexOf("Switch", StringComparison.OrdinalIgnoreCase) >= 0
                        || name.IndexOf("Geyser", StringComparison.OrdinalIgnoreCase) >= 0
                        || name.IndexOf("Trolley", StringComparison.OrdinalIgnoreCase) >= 0
                        || name.IndexOf("Elevator", StringComparison.OrdinalIgnoreCase) >= 0
                        || name.IndexOf("Fire", StringComparison.OrdinalIgnoreCase) >= 0)
                    {
                        mechanics[name] = mechanics.TryGetValue(name, out var m) ? m + 1 : 1;
                    }

                    if (name.IndexOf("AttachStation", StringComparison.OrdinalIgnoreCase) >= 0
                        || name.IndexOf("Workstation", StringComparison.OrdinalIgnoreCase) >= 0
                        || name.IndexOf("CookingStation", StringComparison.OrdinalIgnoreCase) >= 0
                        || name.IndexOf("PlateStation", StringComparison.OrdinalIgnoreCase) >= 0
                        || name.IndexOf("PlateStack", StringComparison.OrdinalIgnoreCase) >= 0
                        || name.IndexOf("PickupItemSpawner", StringComparison.OrdinalIgnoreCase) >= 0
                        || name.IndexOf("IngredientContainer", StringComparison.OrdinalIgnoreCase) >= 0
                        || name.IndexOf("RubbishBin", StringComparison.OrdinalIgnoreCase) >= 0
                        || name.IndexOf("ChoppingBoard", StringComparison.OrdinalIgnoreCase) >= 0)
                    {
                        stations[name] = stations.TryGetValue(name, out var c) ? c + 1 : 1;
                    }
                }
            }

            _log.LogInfo("mech: 场景 " + scene.name + " 根物体 " + roots.Length);
            _log.LogInfo("mech: 机制 = " + (mechanics.Count == 0 ? "无" : Join(mechanics)));
            _log.LogInfo("mech: 工位 = " + Join(stations));
        }

        private static string Join(SortedDictionary<string, int> map)
        {
            var text = new StringBuilder();
            foreach (var pair in map)
            {
                if (text.Length > 0)
                {
                    text.Append(", ");
                }

                text.Append(pair.Key).Append('×').Append(pair.Value);
            }

            return text.Length == 0 ? "无" : text.ToString();
        }

        /// <summary>
        /// 进关卡。**不能**用 GameUtils.LoadLevel：它只换场景，关卡流程不会起来
        /// （实测订单为空、得分 None、计时全 0）。暂停菜单里的"重开"用的是
        /// `ServerMessenger.LoadLevel(场景, GameState.LoadKitchen, true, GameState.RunKitchen)`，
        /// 状态机会把厨房流程带起来，这里照做。
        /// </summary>
        private static void LoadLevel(string scene)
        {
            if (scene.Length == 0)
            {
                _log.LogWarning("指令: load 少了场景名");
                return;
            }

            var messenger = AccessTools.TypeByName("ServerMessenger");
            var stateType = AccessTools.TypeByName("GameState");
            if (messenger == null || stateType == null)
            {
                _log.LogWarning("指令: 找不到 ServerMessenger/GameState");
                return;
            }

            var method = AccessTools.Method(messenger, "LoadLevel",
                new[] { typeof(string), stateType, typeof(bool), stateType });
            if (method == null)
            {
                _log.LogWarning("指令: 找不到 ServerMessenger.LoadLevel(string, GameState, bool, GameState)");
                return;
            }

            StopSynchronisation();

            object begin = Enum.Parse(stateType, "LoadKitchen");
            object run = Enum.Parse(stateType, "RunKitchen");
            _log.LogInfo("指令: ServerMessenger.LoadLevel(" + scene + ", LoadKitchen, true, RunKitchen)");
            method.Invoke(null, new[] { scene, begin, (object)true, run });
            _log.LogInfo("指令: 已请求加载 " + scene);
        }

        /// <summary>暂停菜单重开前会先停同步，照做，否则旧关卡的对象会跟着过来。</summary>
        private static void StopSynchronisation()
        {
            try
            {
                var type = AccessTools.TypeByName("MultiplayerController");
                var utils = AccessTools.TypeByName("GameUtils");
                if (type == null || utils == null)
                {
                    return;
                }

                var require = AccessTools.Method(utils, "RequireManager");
                object controller = null;
                if (require != null && require.IsGenericMethodDefinition)
                {
                    controller = require.MakeGenericMethod(type).Invoke(null, null);
                }

                if (controller == null)
                {
                    return;
                }

                var stop = AccessTools.Method(type, "StopSynchronisation", Type.EmptyTypes);
                if (stop != null)
                {
                    stop.Invoke(controller, null);
                    _log.LogInfo("指令: 已停同步");
                }
            }
            catch (Exception e)
            {
                _log.LogWarning("指令: 停同步失败: " + e.Message);
            }
        }


        /// <summary>
        /// 标题画面按 A 的效果：游戏内部是"某个手柄完成加入" → StartScreenFlow.OnEngagementFinished。
        /// 我们的虚拟手柄已经占住了玩家位，所以直接把这个加入结果告诉标题画面。
        /// 只调 CheckForEngagement 不够：它在等一次新的按键加入，而我们早就占好位了。
        /// </summary>
        private static void Engage()
        {
            var type = AccessTools.TypeByName("StartScreenFlow");
            if (type == null)
            {
                _log.LogWarning("指令: 找不到 StartScreenFlow");
                return;
            }

            var instance = type.GetProperty("Instance", BindingFlags.Public | BindingFlags.Static);
            object flow = instance != null ? instance.GetValue(null, null) : null;
            if (flow == null)
            {
                _log.LogWarning("指令: StartScreenFlow 没有实例（现在不在标题画面？）");
                return;
            }

            object user = FirstEngagedUser();
            if (user == null)
            {
                _log.LogWarning("指令: 还没有任何已加入的手柄用户，没法完成加入");
                return;
            }

            var finished = AccessTools.Method(type, "OnEngagementFinished");
            if (finished == null)
            {
                _log.LogWarning("指令: 找不到 OnEngagementFinished");
                return;
            }

            _log.LogInfo("指令: 用 " + user.GetType().Name + " 完成加入（等价按 A）");
            finished.Invoke(flow, new[] { user });
        }

        private static object FindPlayerManager()
        {
            var type = AccessTools.TypeByName("PCPlayerManager");
            if (type == null)
            {
                return null;
            }

            var found = UnityEngine.Object.FindObjectsOfType(type);
            return found.Length > 0 ? found[0] : null;
        }

        private static object FirstEngagedUser()
        {
            object manager = FindPlayerManager();
            if (manager == null)
            {
                return null;
            }

            var slotType = AccessTools.TypeByName("EngagementSlot");
            var get = AccessTools.Method(manager.GetType(), "GetUser", new[] { slotType });
            if (get == null)
            {
                _log.LogWarning("指令: PlayerManager 上没有 GetUser(EngagementSlot)");
                return null;
            }

            for (int slot = 0; slot < 4; slot++)
            {
                object user = get.Invoke(manager, new[] { Enum.ToObject(slotType, slot) });
                if (user != null)
                {
                    _log.LogInfo("指令: 玩家位 " + (slot + 1) + " 已加入");
                    return user;
                }
            }

            return null;
        }

        /// <summary>开一个空会话（不带存档）：T17FrontendFlow.StartEmptySession(GameType, int)。</summary>
        private static void StartSession(string value)
        {
            var type = AccessTools.TypeByName("T17FrontendFlow");
            if (type == null)
            {
                _log.LogWarning("指令: 找不到 T17FrontendFlow");
                return;
            }

            var instance = type.GetProperty("Instance", BindingFlags.Public | BindingFlags.Static);
            object flow = instance != null ? instance.GetValue(null, null) : null;
            if (flow == null)
            {
                _log.LogWarning("指令: T17FrontendFlow 没有实例");
                return;
            }

            var method = AccessTools.Method(type, "StartEmptySession");
            if (method == null)
            {
                _log.LogWarning("指令: 找不到 StartEmptySession");
                return;
            }

            var parameters = method.GetParameters();
            var args = new object[parameters.Length];
            for (int i = 0; i < parameters.Length; i++)
            {
                if (parameters[i].ParameterType.IsEnum)
                {
                    var names = Enum.GetNames(parameters[i].ParameterType);
                    args[i] = Enum.Parse(parameters[i].ParameterType,
                        value.Length > 0 && Array.IndexOf(names, value) >= 0 ? value : names[0]);
                }
                else
                {
                    args[i] = System.Convert.ChangeType("0", parameters[i].ParameterType);
                }
            }

            _log.LogInfo("指令: StartEmptySession(" + args[0] + ", " + args[1] + ")");
            var session = method.Invoke(flow, args);
            _log.LogInfo("指令: 会话=" + (session == null ? "null" : session.GetType().Name));
        }

        /// <summary>
        /// 标题画面的"继续游戏"。必须走这一步：直接用 GameUtils.LoadLevel 加载关卡场景，
        /// 关卡流程不会起来（实测订单为空、得分 None、计时全 0，厨师也不受控）。
        /// StartScreenFlow.StartLoadProfile 是个协程，得拿 IEnumerator 交给 StartCoroutine 跑。
        /// </summary>
        private static void ContinueFromTitle()
        {
            var type = AccessTools.TypeByName("StartScreenFlow");
            if (type == null)
            {
                _log.LogWarning("继续游戏: 找不到 StartScreenFlow");
                return;
            }

            var property = type.GetProperty("Instance", BindingFlags.Public | BindingFlags.Static);
            object flow = property != null ? property.GetValue(null, null) : null;
            if (flow == null)
            {
                _log.LogWarning("继续游戏: 没有 StartScreenFlow 实例（现在不在标题画面）");
                return;
            }

            _log.LogInfo("继续游戏: 当前场景 " + UnityEngine.SceneManagement.SceneManager.GetActiveScene().name);
            var check = AccessTools.Method(type, "CheckForEngagement", Type.EmptyTypes);
            if (check != null)
            {
                check.Invoke(flow, null);
                _log.LogInfo("继续游戏: 已触发加入检查");
            }

            var loadProfile = AccessTools.Method(type, "StartLoadProfile");
            if (loadProfile == null)
            {
                _log.LogWarning("继续游戏: 找不到 StartLoadProfile");
                return;
            }

            object callback = MakeCallback(loadProfile.GetParameters()[0].ParameterType);
            object result = loadProfile.Invoke(flow, new[] { callback });
            var routine = result as System.Collections.IEnumerator;
            if (routine == null)
            {
                _log.LogWarning("继续游戏: StartLoadProfile 没返回协程，拿到 " + (result == null ? "null" : result.GetType().Name));
                return;
            }

            var behaviour = flow as MonoBehaviour;
            if (behaviour == null)
            {
                _log.LogWarning("继续游戏: StartScreenFlow 不是 MonoBehaviour，协程起不来");
                return;
            }

            behaviour.StartCoroutine(routine);
            _log.LogInfo("继续游戏: 协程已启动（等场景切换）");
        }

        /// <summary>回调类型未知，按它的委托签名现场造一个空实现。</summary>
        private static object MakeCallback(Type callbackType)
        {
            try
            {
                var method = typeof(Commands).GetMethod(nameof(Noop), BindingFlags.Static | BindingFlags.NonPublic);
                return Delegate.CreateDelegate(callbackType, method);
            }
            catch (Exception e)
            {
                _log.LogWarning("继续游戏: 造不出回调 " + callbackType.Name + ": " + e.Message);
                return null;
            }
        }

        private static void Noop()
        {
        }

        /// <summary>order=1：把当前订单/菜谱/步骤叶子打出来（工序状态机读的就是这些）。</summary>
        private static void DumpOrder()
        {
            var controller = WorldState.FirstOrderController();
            _log.LogInfo("指令: 订单控制器=" + (controller == null ? "null" : controller.GetType().Name));
            var recipes = WorldState.ActiveRecipes(controller);
            _log.LogInfo("指令: 活跃订单菜名=" + (recipes.Count == 0 ? "（空）" : string.Join(",", recipes.ToArray())));
            var leaves = WorldState.OrderStepLeaves(controller);
            _log.LogInfo("指令: 步骤叶子=" + (leaves.Count == 0 ? "（空）" : string.Join(",", leaves.ToArray())));
            var stack = Kitchen.PlateStack();
            _log.LogInfo("指令: 盘子堆=" + (stack == null ? "null" : stack.Name + " (" + stack.Position.x.ToString("F1") + "," + stack.Position.y.ToString("F1") + ")"));
            var station = Kitchen.DeliveryStation();
            _log.LogInfo("指令: 出菜台=" + (station == null ? "null" : station.Name + " (" + station.Position.x.ToString("F1") + "," + station.Position.y.ToString("F1") + ")"));
            foreach (var crate in Kitchen.Crates())
            {
                _log.LogInfo("指令: 料箱 " + crate.Name + " 给=" + crate.Gives
                             + " (" + crate.Position.x.ToString("F2") + "," + crate.Position.y.ToString("F2") + ") "
                             + GameCellNeighbours(crate.Position));
            }
        }

        /// <summary>
        /// 工位所在的游戏格（1.2 米那套）、格心、本格占位物体，以及四个邻格的格心/占位/能不能站。
        /// 交互只认"站在空邻格、朝着工位格"，站位点必须落在这些邻格里。
        /// </summary>
        private static string GameCellNeighbours(Vector2 station)
        {
            int ix, iz;
            Vector2 centre;
            if (!WorldState.GameCellOf(station, out ix, out iz) || !WorldState.GameCellPosition(ix, iz, out centre))
            {
                return "游戏格=?";
            }
            var own = WorldState.GameCellOccupant(ix, iz);
            var text = "游戏格=(" + ix + "," + iz + ") 格心=(" + centre.x.ToString("F2") + "," + centre.y.ToString("F2")
                       + ") 本格=「" + (own == null ? "" : own.name) + "」";
            var offsets = new[] { new[] { 1, 0 }, new[] { -1, 0 }, new[] { 0, 1 }, new[] { 0, -1 } };
            foreach (var o in offsets)
            {
                Vector2 p;
                if (!WorldState.GameCellPosition(ix + o[0], iz + o[1], out p))
                {
                    continue;
                }
                var occupant = WorldState.GameCellOccupant(ix + o[0], iz + o[1]);
                text += " [" + o[0] + "," + o[1] + "]=(" + p.x.ToString("F2") + "," + p.y.ToString("F2") + ")"
                        + (occupant == null ? "空" : "「" + occupant.name + "」")
                        + (WorldState.IsWalkablePoint(p.x, p.y) ? "可站" : "不可站");
            }
            return text;
        }

        /// <summary>scan=关键字：列出物体名或组件类型名含这个关键字的对象（找料箱/工位用）。</summary>
        private static void Scan(string value)
        {
            string keyword = value.ToLowerInvariant();
            if (keyword.Length == 0)
            {
                _log.LogWarning("指令: scan=关键字");
                return;
            }

            int shown = 0;
            foreach (var component in UnityEngine.Object.FindObjectsOfType<Component>())
            {
                if (component == null)
                {
                    continue;
                }

                string typeName = component.GetType().Name;
                string objectName = component.gameObject.name;
                if (!typeName.ToLowerInvariant().Contains(keyword)
                    && !objectName.ToLowerInvariant().Contains(keyword))
                {
                    continue;
                }

                var p = component.transform.position;
                _log.LogInfo("     " + objectName + " [" + typeName + "] ("
                             + p.x.ToString("F2") + "," + p.z.ToString("F2") + ")"
                             + " 组件=" + component.gameObject.GetComponents<Component>().Length);
                if (++shown >= 60)
                {
                    _log.LogInfo("     …（到 60 条截断）");
                    return;
                }
            }

            _log.LogInfo("     scan " + value + "：共 " + shown + " 条");
        }

        /// <summary>把某个点周围 2 米内的碰撞体全打出来：卡住时用它看物理布局，不靠猜。</summary>
        private static void Probe(string value)
        {
            var parts = value.Split(',');
            if (parts.Length < 2)
            {
                _log.LogWarning("指令: probe=x,z");
                return;
            }

            float x, z;
            if (!float.TryParse(parts[0], out x) || !float.TryParse(parts[1], out z))
            {
                _log.LogWarning("指令: probe 坐标看不懂: " + value);
                return;
            }

            var center = new Vector3(x, 0.5f, z);
            _log.LogInfo("指令: probe (" + x.ToString("F2") + "," + z.ToString("F2") + ") 半径 2 米内的碰撞体：");
            foreach (var collider in UnityEngine.Object.FindObjectsOfType<Collider>())
            {
                var bounds = collider.bounds;
                var closest = bounds.ClosestPoint(center);
                float distance = Vector3.Distance(closest, center);
                if (distance > 2f)
                {
                    continue;
                }

                var owner = collider.gameObject;
                _log.LogInfo("     " + owner.name + " [" + LayerMask.LayerToName(owner.layer) + "]"
                             + " 触发=" + collider.isTrigger
                             + " 最近点=" + closest.x.ToString("F2") + "," + closest.z.ToString("F2")
                             + " 距离=" + distance.ToString("F2")
                             + " 尺寸=" + bounds.size.x.ToString("F2") + "×" + bounds.size.y.ToString("F2") + "×" + bounds.size.z.ToString("F2"));
            }
        }

        /// <summary>path=x1,z1,x2,z2：直接问一次 A*，把起点/终点格与结果打出来。</summary>
        private static void ProbePath(string value)
        {
            var parts = value.Split(',');
            if (parts.Length < 4)
            {
                _log.LogWarning("指令: path=x1,z1,x2,z2");
                return;
            }

            float x1, z1, x2, z2;
            if (!float.TryParse(parts[0], out x1) || !float.TryParse(parts[1], out z1)
                || !float.TryParse(parts[2], out x2) || !float.TryParse(parts[3], out z2))
            {
                _log.LogWarning("指令: path 坐标看不懂");
                return;
            }

            int sx, sz, tx, tz;
            WorldState.WorldToCell(x1, z1, out sx, out sz);
            WorldState.WorldToCell(x2, z2, out tx, out tz);
            _log.LogInfo("指令: path 起点格 " + sx + "," + sz + " 可走=" + WorldState.IsWalkableCell(sx, sz)
                         + " 格心=" + WorldState.CellCenter(sx, sz)
                         + "；终点格 " + tx + "," + tz + " 可走=" + WorldState.IsWalkableCell(tx, tz)
                         + " 格心=" + WorldState.CellCenter(tx, tz));
            _log.LogInfo("指令: 起点四邻可走=" + WorldState.CanStep(sx, sz, 1, 0) + "/"
                         + WorldState.CanStep(sx, sz, -1, 0) + "/"
                         + WorldState.CanStep(sx, sz, 0, 1) + "/"
                         + WorldState.CanStep(sx, sz, 0, -1));

            var path = Navigation.FindPath(new Vector2(x1, z1), new Vector2(x2, z2));
            if (path == null)
            {
                _log.LogInfo("指令: A* 返回 null（不可达）");
                return;
            }

            _log.LogInfo("指令: A* 返回 " + path.Count + " 个路点: " + string.Join(" ", path.ConvertAll(v => "(" + v.x.ToString("F1") + "," + v.y.ToString("F1") + ")").ToArray()));
        }

        private static void DumpType(string name)
        {
            var type = AccessTools.TypeByName(name);
            if (type == null)
            {
                _log.LogWarning("指令: 找不到类型 " + name);
                return;
            }

            if (type.IsEnum)
            {
                _log.LogInfo("指令: 枚举 " + type.FullName + " = " + string.Join(", ", Enum.GetNames(type)));
                return;
            }

            _log.LogInfo("指令: 类型 " + type.FullName + " 的方法：");
            foreach (var method in type.GetMethods(BindingFlags.Public | BindingFlags.NonPublic
                                                   | BindingFlags.Static | BindingFlags.Instance
                                                   | BindingFlags.DeclaredOnly))
            {
                _log.LogInfo("     " + method.Name + "(" + Describe(method.GetParameters()) + ") → "
                             + (method.ReturnType == null ? "?" : method.ReturnType.Name));
            }
        }

        /// <summary>
        /// 走游戏自己的暂停机制（`TimeManager.SetPaused(PauseLayer.Main, …)`），
        /// 用于验证 `game_time_ms` 在暂停时不走。`Time.timeScale` 会被 TimeManager
        /// 每帧覆盖，所以不能靠它。
        /// </summary>
        private static void SetGamePaused(bool paused)
        {
            var type = AccessTools.TypeByName("TimeManager");
            if (type == null)
            {
                _log.LogWarning("指令: 找不到 TimeManager");
                return;
            }

            var field = type.GetField("s_timeManager", BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static);
            object manager = field != null ? field.GetValue(null) : null;
            var layerType = AccessTools.TypeByName("TimeManager+PauseLayer");
            var setPaused = type.GetMethod("SetPaused", BindingFlags.Public | BindingFlags.Instance);
            if (manager == null || layerType == null || setPaused == null)
            {
                _log.LogWarning("指令: TimeManager 暂停接口不可用");
                return;
            }

            object layer = Enum.Parse(layerType, "Main");
            try
            {
                setPaused.Invoke(manager, new[] { layer, (object)paused, (object)"WellCooked.Verify" });
                _log.LogInfo("指令: pause_game=" + (paused ? "1" : "0") + " 已调用 TimeManager.SetPaused(Main)");
            }
            catch (Exception e)
            {
                _log.LogWarning("指令: pause_game 调用失败 " + e.Message);
            }
        }

        private static void DumpPause()
        {
            var type = AccessTools.TypeByName("TimeManager");
            if (type == null)
            {
                _log.LogWarning("指令: 找不到 TimeManager");
                return;
            }

            var field = type.GetField("s_timeManager", BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static);
            object manager = field != null ? field.GetValue(null) : null;
            _log.LogInfo("指令: TimeManager 实例=" + (manager != null)
                         + " Time.timeScale=" + Time.timeScale.ToString("F2"));
            if (manager == null)
            {
                return;
            }

            foreach (var f in type.GetFields(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance))
            {
                object v = null;
                try
                {
                    v = f.GetValue(manager);
                }
                catch
                {
                    continue;
                }

                _log.LogInfo("     " + f.Name + " = " + (v == null ? "null" : v.ToString()));
            }
        }

        private static string Describe(ParameterInfo[] parameters)
        {
            var parts = new List<string>();
            foreach (var p in parameters)
            {
                parts.Add(p.ParameterType.Name + " " + p.Name);
            }

            return string.Join(", ", parts.ToArray());
        }

        private static object Convert(string value, Type target)
        {
            try
            {
                if (target == typeof(string))
                {
                    return value;
                }

                if (target.IsEnum)
                {
                    return Enum.Parse(target, value, true);
                }

                return System.Convert.ChangeType(value, target);
            }
            catch (Exception e)
            {
                _log.LogWarning("指令: 参数 " + value + " 转不成 " + target.Name + ": " + e.Message);
                return null;
            }
        }
    }
}
