// wellcooked_demo level generator (editor script, multi-level).
//
// Drop this into the editor project's Assets/Editor/ and copy layout_1..4.tsv to
// Assets/LevelSets/wellcooked_demo/ (the script reads them to place stations).
// Menu: Tools -> WellCooked -> Build level set and scenes.
//
// Output per level:
//   Assets/LevelSets/wellcooked_demo/data/wellcooked_demo_N/{LevelInfo_*.asset, config_1p..4p.asset}
//   Assets/LevelSets/wellcooked_demo/scenes/s_wellcooked_demo_N.unity (copied from Assets/Template/s_template.unity)
//   bundle names: set folder = info_wellcooked_demo, scene = s_wellcooked_demo_N
// Plus the set asset Assets/LevelSets/wellcooked_demo/LevelSetInfo.asset (4 levels).
// Then Tools -> Toggle Prepare For Building -> save -> Tools -> Build AssetBundles.
//
// NOTE: this file is intentionally ASCII-only. Chinese text is emitted through
// C# \uXXXX escapes so that no encoding round-trip can corrupt the source.

using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.SceneManagement;
using LevelEditor;
using LevelEditorStub;

public static class BuildWellCookedDemo
{
    const string SetName = "wellcooked_demo";
    const string SetDir = "Assets/LevelSets/" + SetName;
    const string ConfigTemplateSO = "Assets/common01/pseudo_prefab_so/game_environment/LevelConfigTemplateSO.asset";
    const string LevelInfoTemplate = "Assets/Template/levelinfo_template.asset";
    const string SceneTemplate = "Assets/Template/s_template.unity";
    const string CounterPrefabPath = "Assets/common01/prefabs/counters/Counter.prefab";
    const string CounterSoPath = "Assets/common01/pseudo_prefab_so/counters/CounterSO.asset";

    // Grid = 0.6 m (the measured GCD of station sizes: CounterTop/Cooker are 1.20 m wide,
    // Sink is 2.40 m wide). One station therefore covers 2x2 cells.
    // Room bounds (cell coordinates): x -10..28, z -16..4 => 22.8 x 12 m.
    const float CellSize = 0.6f;
    const float UtensilYOffset = 0.60f;   // 锅具相对台面抬升高度（米），看不见就调这个
    const int RoomX0Default = -10, RoomX1Default = 28, RoomZ0Default = -16, RoomZ1Default = 4;

    // Room bounds (cell coordinates) are measured from every layout in PlaceFromLayout.
    // The old hard-coded -10..28 / -16..4 only matched level 2 and 4; level 1 uses the
    // shell x -8..20 and the ring of level 3 uses x 0..18, z -10..0. With the fixed values
    // their wall-mounted stations (serving window, sink) counted as "inside the room" and
    // were rotated to the island yaw instead of the wall yaw.
    // Spawn cells collected while placing the layout; applied to the template Players afterwards.
    static readonly List<Vector3> pendingSpawns = new List<Vector3>();
    static float RoomX0 = RoomX0Default, RoomX1 = RoomX1Default;
    static float RoomZ0 = RoomZ0Default, RoomZ1 = RoomZ1Default;

    static readonly string[] LevelNames =
    {
        "wellcooked_demo_1",
        "wellcooked_demo_2",
        "wellcooked_demo_3",
        "wellcooked_demo_4",
        // 四个"素材展示关"：每个主题一个，把该主题所有素材摆成画廊
        "wellcooked_show_sushi",
        "wellcooked_show_beach",
        "wellcooked_show_summer",
        "wellcooked_show_city",
    };

    // 关卡名 -> layout 文件名（玩法关用 layout_1..4，展示关用 layout_show_*）
    static readonly Dictionary<string, string> LayoutFiles = new Dictionary<string, string>
    {
        { "wellcooked_demo_1", "layout_1.tsv" },
        { "wellcooked_demo_2", "layout_2.tsv" },
        { "wellcooked_demo_3", "layout_3.tsv" },
        { "wellcooked_demo_4", "layout_4.tsv" },
        { "wellcooked_show_sushi", "layout_show_sushi.tsv" },
        { "wellcooked_show_beach", "layout_show_beach.tsv" },
        { "wellcooked_show_summer", "layout_show_summer.tsv" },
        { "wellcooked_show_city", "layout_show_city.tsv" },
    };

    static string LayoutPathOf(string level)
    {
        string file;
        if (LayoutFiles.TryGetValue(level, out file)) return Path.Combine(SetDir, file);
        return Path.Combine(SetDir, "layout_" + level.Substring(level.Length - 1) + ".tsv");
    }

    static readonly Dictionary<string, string> LevelDisplayNames = new Dictionary<string, string>
    {
        { "wellcooked_demo_1", "WellCooked Demo 1 - Sushi Street" },
        { "wellcooked_demo_2", "WellCooked Demo 2 - Beach Resort" },
        { "wellcooked_demo_3", "WellCooked Demo 3 - Summer Festival" },
        { "wellcooked_demo_4", "WellCooked Demo 4 - Old Town" },
        { "wellcooked_show_sushi", "Gallery - Sushi Street (city_sushi)" },
        { "wellcooked_show_beach", "Gallery - Beach (dlc02_beach)" },
        { "wellcooked_show_summer", "Gallery - Summer Festival (dlc11_summer)" },
        { "wellcooked_show_city", "Gallery - Old Town (city)" },
    };

    // Layout files may carry a menu line:  # menu: RecipeA_SO, RecipeB_SO
    static string[] ParseMenuLine(string layoutPath)
    {
        if (!File.Exists(layoutPath)) return null;
        foreach (var line in File.ReadAllLines(layoutPath))
        {
            var s = line.Trim();
            if (!s.StartsWith("# menu:")) continue;
            var body = s.Substring("# menu:".Length).Trim();
            var names = body.Split(',');
            var list = new List<string>();
            foreach (var n in names)
            {
                var name = n.Trim();
                if (name.Length == 0) continue;
                var path = "Assets/common01/food/Recipes/" + name + ".asset";
                if (File.Exists(path)) list.Add(path);
                else Debug.LogWarning("[wellcooked_demo] menu recipe not found: " + name);
            }
            if (list.Count > 0) return list.ToArray();
        }
        return null;
    }
    // Per-level menu: a level may only require dishes its dispensers can actually supply.
    // Missing key => use the default Recipes list above.
    static readonly Dictionary<string, string[]> LevelRecipes = new Dictionary<string, string[]>
    {
        { "wellcooked_demo_1", new[]
            {
                "Assets/common01/food/Recipes/Burrito_Chicken_SO.asset",
                "Assets/common01/food/Recipes/Sushi_PlainPrawn_SO.asset",
            }
        },
    };
    static readonly string[] Recipes =
    {
        "Assets/common01/food/Recipes/Burrito_Chicken_SO.asset",
        "Assets/common01/food/Recipes/Sushi_PlainFish_SO.asset",
        "Assets/common01/food/Recipes/Sushi_PlainPrawn_SO.asset",
    };

    static string ScenePathOf(string level) { return SetDir + "/scenes/s_" + level + ".unity"; }
    static string DataDirOf(string level) { return SetDir + "/data/" + level; }
    static string LayoutAssetOf(string level) { return LayoutPathOf(level); }

    [MenuItem("Tools/WellCooked/Build Level Set And Scenes")]
    public static void Build()
    {
        var infos = new List<LevelInfoSO>();

        foreach (string level in LevelNames)
        {
            CreateFolders(level);
            LevelInfoSO info = CreateLevelInfo(level);
            Scene scene = CreateScene(level);
            TileFloor(scene);

            // In batchmode Awake/OnEnable do not run (no scene update loop under -nographics),
            // so the pseudo prefab manager must be re-initialised explicitly; otherwise the
            // prefabs handed out while placing have no assets behind them.
            PseudoPrefabManager.Instance.DeInit();
            PseudoPrefabManager.Instance.Init();

            pendingSpawns.Clear();
            PlaceFromLayout(scene, level);
            ApplySpawnPositions(scene, pendingSpawns, level);
            AttachLevelInfo(scene, info);

            // TargetSceneSaveValidator.CheckPrepareForBuilding requires this flag to be true;
            // in batchmode a missing dialog would silently skip saving/building.
            PseudoPrefabManager manager = FindManager(scene);
            if (manager != null)
            {
                manager.prepareForBuilding = true;
            }

            SetBundleNames(level);
            EditorSceneManager.MarkSceneDirty(scene);
            bool saved = EditorSceneManager.SaveScene(scene);
            Debug.Log("[wellcooked_demo] " + level + " scene saved=" + saved
                      + " rootObjects=" + scene.GetRootGameObjects().Length);
            infos.Add(info);
        }

        CreateLevelSetInfo(infos);
        AssetDatabase.SaveAssets();
        AssetDatabase.Refresh();
        Debug.Log("[wellcooked_demo] done, levels=" + infos.Count);
    }

    [MenuItem("Tools/WellCooked/Self Check (bundle names and files)")]
    public static void SelfCheck()
    {
        foreach (string level in LevelNames)
        {
            Debug.Log("[wellcooked_demo] " + level
                      + " layout=" + File.Exists(LayoutAssetOf(level))
                      + " scene=" + (AssetDatabase.LoadAssetAtPath<SceneAsset>(ScenePathOf(level)) != null)
                      + " sceneBundle=" + BundleNameOf(ScenePathOf(level)));
        }
        Debug.Log("[wellcooked_demo] setBundle=" + BundleNameOf(SetDir));
    }

    // AssetImporter.GetAtPath returns null for a path that does not exist yet.
    static string BundleNameOf(string path)
    {
        AssetImporter importer = AssetImporter.GetAtPath(path);
        if (importer == null)
        {
            return "(missing)";
        }
        return importer.assetBundleName.Length == 0 ? "(unset)" : importer.assetBundleName;
    }

    static PseudoPrefabManager FindManager(Scene scene)
    {
        foreach (GameObject root in scene.GetRootGameObjects())
        {
            var comp = root.GetComponent<PseudoPrefabManager>();
            if (comp != null)
            {
                return comp;
            }
        }
        return null;
    }

    static void CreateFolders(string level)
    {
        foreach (string path in new[] { SetDir, SetDir + "/scenes", SetDir + "/data", DataDirOf(level) })
        {
            if (AssetDatabase.IsValidFolder(path))
            {
                continue;
            }
            string parent = Path.GetDirectoryName(path).Replace('\\', '/');
            AssetDatabase.CreateFolder(parent, Path.GetFileName(path));
        }
    }

    static LevelInfoSO CreateLevelInfo(string level)
    {
        string path = DataDirOf(level) + "/LevelInfo_" + level + ".asset";
        var info = AssetDatabase.LoadAssetAtPath<LevelInfoSO>(path);
        if (info == null)
        {
            AssetDatabase.CopyAsset(LevelInfoTemplate, path);
            info = AssetDatabase.LoadAssetAtPath<LevelInfoSO>(path);
        }
        info.levelName = level;
        info.levelNameZH = LevelDisplayNames.ContainsKey(level) ? LevelDisplayNames[level] : level;
        info.sceneName = "s_" + level;
        var menuOverride = ParseMenuLine(LayoutPathOf(level));
            info.recipes = ((menuOverride != null) ? menuOverride : (LevelRecipes.ContainsKey(level) ? LevelRecipes[level] : Recipes)).Select(AssetDatabase.LoadAssetAtPath<ScriptableObject>).ToArray();
        info.debugRecipeCount = 0;
        info.disableDynamicParenting = true;

        // 1..4 player configs copied from the templates, then tuned by player count.
        // wellcooked_demo_3 is the tuned level: a 2-minute round, and every order stays open
        // for the whole round (120 s) so a slow dish can still be finished after it appears.
        int round1 = 90, round24 = 100, life1 = 22, life2 = 30, life3 = 34, life4 = 40;
        if (level == "wellcooked_demo_3")
        {
            round1 = 120;
            round24 = 120;
            life1 = 120;
            life2 = 120;
            life3 = 120;
            life4 = 120;
        }
        info.config_1p = CopyConfig(level, 1, round1, 6, life1);
        info.config_2p = CopyConfig(level, 2, round24, 6, life2);
        info.config_3p = CopyConfig(level, 3, round24, 6, life3);
        info.config_4p = CopyConfig(level, 4, round24, 6, life4);
        EditorUtility.SetDirty(info);
        return info;
    }

    static LevelConfigSetupPerPlayerCountSO CopyConfig(string level, int players, int roundTime, int betweenOrders, int lifetime)
    {
        string src = "Assets/Template/config_" + players + "p.asset";
        string dst = DataDirOf(level) + "/config_" + players + "p.asset";
        if (AssetDatabase.LoadAssetAtPath<LevelConfigSetupPerPlayerCountSO>(dst) == null)
        {
            AssetDatabase.CopyAsset(src, dst);
        }
        var cfg = AssetDatabase.LoadAssetAtPath<LevelConfigSetupPerPlayerCountSO>(dst);
        if (cfg == null)
        {
            Debug.LogError("[wellcooked_demo] cannot copy config template " + src);
            return null;
        }
        cfg.roundTime = roundTime;
        cfg.timeBetweenOrders = betweenOrders;
        cfg.orderLifeTime = lifetime;
        EditorUtility.SetDirty(cfg);
        return cfg;
    }

    static void CreateLevelSetInfo(List<LevelInfoSO> levelInfos)
    {
        // The file must be named LevelSetInfo: the DIY loader finds the set info by this name.
        string path = SetDir + "/LevelSetInfo.asset";
        var info = AssetDatabase.LoadAssetAtPath<LevelSetInfoSO>(path);
        if (info == null)
        {
            info = ScriptableObject.CreateInstance<LevelSetInfoSO>();
            AssetDatabase.CreateAsset(info, path);
        }
        info.levelSetName = SetName;
        info.levelSetNameZH = "WellCooked Demo Levels";
        info.uid = SetName;
        info.version = "1.1";
        info.levelInfos = levelInfos.ToArray();
        EditorUtility.SetDirty(info);
    }

    static Scene CreateScene(string level)
    {
        string path = ScenePathOf(level);
        // Rebuild from the template every time: re-opening a previously generated scene
        // and placing again would stack a second copy of everything.
        if (AssetDatabase.LoadAssetAtPath<SceneAsset>(path) != null)
        {
            AssetDatabase.DeleteAsset(path);
        }
        AssetDatabase.CopyAsset(SceneTemplate, path);
        return EditorSceneManager.OpenScene(path, OpenSceneMode.Single);
    }

    // Tile the template Floor (Unity builtin plane: 10 x 10 units, scale 2.4 / 1.32
    // => 24 x 13.2 m) over a 5 x 5 area WITHOUT touching its scale, so the camera never
    // sees black void around the kitchen.
    static void TileFloor(Scene scene)
    {
        Transform source = null;
        foreach (GameObject root in scene.GetRootGameObjects())
        {
            foreach (Transform tr in root.GetComponentsInChildren<Transform>(true))
            {
                if (tr.name == "Floor")
                {
                    source = tr;
                    break;
                }
            }
            if (source != null)
            {
                break;
            }
        }
        if (source == null)
        {
            Debug.LogError("[wellcooked_demo] no Floor object in the template, cannot tile the floor");
            return;
        }

        float tileX = 10f * source.localScale.x;
        float tileZ = 10f * source.localScale.z;
        Vector3 origin = source.localPosition;
        Quaternion rotation = source.localRotation;
        Vector3 scale = source.localScale;

        var group = new GameObject("FloorTiles");
        group.transform.SetParent(source.parent, false);

        int count = 0;
        for (int ix = -2; ix <= 2; ix++)
        {
            for (int iz = -2; iz <= 2; iz++)
            {
                Transform copy = UnityEngine.Object.Instantiate(source, group.transform);
                copy.name = "Floor_" + ix + "_" + iz;
                copy.localPosition = origin + new Vector3(ix * tileX, 0f, iz * tileZ);
                copy.localRotation = rotation;
                copy.localScale = scale;
                count++;
            }
        }
        // The tiled copies (one of which sits exactly on the original spot) replace it.
        source.gameObject.SetActive(false);
        Debug.Log("[wellcooked_demo] floor tiled: " + count + " tiles of "
                  + tileX.ToString("F1") + "x" + tileZ.ToString("F1") + " m");
    }

    // Room shell = min/max cell of every non-spawn station in the layout. Wall-mounted
    // stations sit exactly on that boundary, so WallYaw() can tell a wall from the middle.
    static void ComputeRoomBounds(string level, string[] lines)
    {
        float x0 = float.MaxValue, x1 = float.MinValue, z0 = float.MaxValue, z1 = float.MinValue;
        foreach (string raw in lines)
        {
            string line = raw.Trim();
            if (line.Length == 0 || line[0] == '#')
            {
                continue;
            }
            string[] f = line.Split('\t');
            // spawn 不占地；art 是纯背景（可能放在房间外很远的地方），两者都不能参与
            // 房间边界计算，否则墙边工位的朝向（wall yaw）会被背景道具带偏。
            if (f.Length < 6 || f[0] == "kind" || f[0] == "spawn" || f[0] == "art")
            {
                continue;
            }
            float gx = float.Parse(f[4], CultureInfo.InvariantCulture);
            float gz = float.Parse(f[5], CultureInfo.InvariantCulture);
            if (gx < x0) { x0 = gx; }
            if (gx > x1) { x1 = gx; }
            if (gz < z0) { z0 = gz; }
            if (gz > z1) { z1 = gz; }
        }
        if (x0 > x1 || z0 > z1)
        {
            return;
        }
        RoomX0 = x0; RoomX1 = x1; RoomZ0 = z0; RoomZ1 = z1;
        Debug.Log("[wellcooked_demo] " + level + " room bounds: x " + x0 + ".." + x1
                  + ", z " + z0 + ".." + z1);
    }

    // Station footprint in metres: width along local X, depth along local Z. Sink and serving
    // window are wider than a counter, and wall-mounted stations are rotated by yaw.
    // Serving window: the clean base is 2.40 x 1.20 m (4 x 2 cells); the 2.77 / 1.82 m boxes
    // are only the decorative trim and must not drive the collision test.
    static void FootprintOf(string kind, float yaw, out float w, out float d)
    {
        w = 1.2f; d = 1.2f;
        if (kind == "washing") { w = 2.4f; d = 1.2f; }
        else if (kind == "serving") { w = 2.4f; d = 1.2f; }
        float y = Mathf.Repeat(yaw, 360f);
        if (y > 45f && y < 135f || y > 225f && y < 315f)
        {
            float t = w; w = d; d = t;
        }
    }

    static void PlaceFromLayout(Scene scene, string level)
    {
        string layoutAsset = LayoutAssetOf(level);
        // Path.Combine in the .NET 2.0 subset only has the two-argument overload.
        string repoLayout = Path.GetFullPath(Path.Combine(Path.Combine(Application.dataPath, ".."), layoutAsset));
        string path = File.Exists(layoutAsset) ? layoutAsset : (File.Exists(repoLayout) ? repoLayout : null);
        if (path == null)
        {
            Debug.LogError("[wellcooked_demo] cannot find " + layoutAsset + ": copy it into the project");
            return;
        }

        string[] layoutLines = File.ReadAllLines(path);
        ComputeRoomBounds(level, layoutLines);

        int placed = 0;
        int duplicates = 0;
        int badSpawns = 0;
        int spawnIndex = 0;

        int overlaps = 0;
        int standaloneUtensils = 0;
        int plateBases = 0;
        int artPlaced = 0;
        // cell -> kind of the station occupying it (utensils are mounted, not occupying)
        var occupied = new Dictionary<string, string>();
        // cell -> station GameObject a utensil / plate stack can be attached to
        var placedByCell = new Dictionary<string, GameObject>();
        // cell -> ground footprint (x0, x1, z0, z1) for overlap detection
        var footprints = new Dictionary<string, float[]>();

        // Place every ground station before its mounts. A plate stack / utensil that shares a
        // cell with a counter must attach to that counter; if the mount were seen first the
        // plate stack branch below would create a second counter_base in the same cell (two
        // overlapping counters made the level-4 plate stack unreachable). Spawns stay after
        // the ground stations so the badSpawns check still sees occupied cells.
        var solidLines = new List<string>();
        var spawnLines = new List<string>();
        var mountLines = new List<string>();
        foreach (string raw in layoutLines)
        {
            string line = raw.Trim();
            if (line.Length == 0 || line[0] == '#') { solidLines.Add(raw); continue; }
            string[] f = line.Split('\t');
            if (f.Length < 6 || f[0] == "kind") { solidLines.Add(raw); continue; }
            if (f[0] == "utensil" || f[0] == "plate_stack") { mountLines.Add(raw); }
            else if (f[0] == "spawn") { spawnLines.Add(raw); }
            else { solidLines.Add(raw); }
        }
        var orderedLayoutLines = new List<string>(solidLines.Count + spawnLines.Count + mountLines.Count);
        orderedLayoutLines.AddRange(solidLines);
        orderedLayoutLines.AddRange(spawnLines);
        orderedLayoutLines.AddRange(mountLines);

        foreach (string raw in orderedLayoutLines)
        {
            string line = raw.Trim();
            if (line.Length == 0 || line[0] == '#')
            {
                continue;
            }
            string[] f = line.Split('\t');
            if (f.Length < 6 || f[0] == "kind")
            {
                continue;
            }
            string kind = f[0], prefabName = f[1], soName = f[2], ingredient = f[3];
            float gx = float.Parse(f[4], CultureInfo.InvariantCulture);
            float gz = float.Parse(f[5], CultureInfo.InvariantCulture);
            float x = gx * CellSize;
            float z = gz * CellSize;
            string cellKey = gx.ToString("0.###") + "," + gz.ToString("0.###");

            if (kind == "spawn")
            {
                if (gx <= RoomX0 || gx >= RoomX1 || gz <= RoomZ0 || gz >= RoomZ1)
                {
                    Debug.LogError("[wellcooked_demo] " + level + " spawn outside the room: " + cellKey);
                    badSpawns++;
                }
                if (occupied.ContainsKey(cellKey))
                {
                    Debug.LogError("[wellcooked_demo] " + level + " spawn overlaps "
                                   + occupied[cellKey] + ": " + cellKey);
                    badSpawns++;
                }
                PlaceSpawn(Group(scene, "Design"), x, z, spawnIndex++);
                pendingSpawns.Add(new Vector3(x, 0.2f, z));
                placed++;
                continue;
            }

            // Environment art (walls, floors, decorations, NPCs). Exactly like the official
            // example project: the scene only carries a placeholder, PseudoPrefab.ResetChild()
            // swaps in the real model from the game bundle at runtime, so the layout stores
            // nothing but position / rotation. Art is pure decoration and therefore does not
            // take part in the gameplay self-checks (it may overlap counters, never blocks a
            // cell, and does not widen the room bounds).
            //   art <theme> <prefabName> <yOffset> <gx> <gz> <note> <dir>
            if (kind == "art")
            {
                if (PlaceArt(scene, level, f, x, z))
                {
                    artPlaced++;
                    placed++;
                }
                continue;
            }

            if (kind != "utensil" && kind != "plate_stack")
            {
                if (occupied.ContainsKey(cellKey))
                {
                    Debug.LogError("[wellcooked_demo] " + level + " duplicate cell " + cellKey
                                   + ": " + occupied[cellKey] + " + " + kind);
                    duplicates++;
                }
                else
                {
                    occupied[cellKey] = kind;
                }
            }

            // Wall-mounted stations must face into the room; with the prefab default yaw
            // (e.g.) the serving window ends up facing the wall.
            // The serving window is the exception: its arrow is part of the pass and has to
            // point OUT of the kitchen, so it always uses ServingYaw() and ignores "dir".
            // Every other station takes the optional 8th layout column ("dir", the layout
            // editor's R key) when it holds a valid angle; without it the wall the station
            // belongs to decides. The editor writes that column, so a rotation made there is
            // exactly the rotation that gets built instead of silently snapping back.
            float yaw = (kind == "serving") ? ServingYaw(gx, gz) : WallYaw(gx, gz);
            if (kind != "serving" && f.Length >= 8)
            {
                float dir;
                if (float.TryParse(f[7], NumberStyles.Float, CultureInfo.InvariantCulture, out dir)
                    && dir >= 0f && dir < 360f)
                {
                    yaw = dir;
                }
            }

            // A station footprint must never intersect another one: the first version of
            // layout 3 put a counter on every 0.6 m cell, so 1.2 m counters overlapped by
            // half and the kitchen was unusable. Sink (2.4 x 1.2 m) and serving window
            // (2.4 x 1.2 m) are checked with their real base size, not the 2.77 / 1.82 m
            // decorative outer box.
            //
            // UNIFIED PLACEHOLDER MODEL (keep identical to layout-editor/index.html):
            // a station at cell (gx,gz) is CENTERED on that cell and owns the w x h cells
            // [gx-w/2, gx+w/2) x [gz-h/2, gz+h/2). In metres its centre is (gx,gz)*CellSize
            // and the box is centre +/- (w,d)/2, which is what the code below already does.
            // Example: a 2x2 counter at (7,0) owns cells 6..7 x -1..0. The editor draws and
            // collides exactly these cells, so a placement it accepts cannot fail here.
            if (kind != "utensil" && kind != "plate_stack")   // mounts sit on the host cell by design
            {
                float fw, fd;
                FootprintOf(kind, yaw, out fw, out fd);
                float fx0 = x - fw * 0.5f, fx1 = x + fw * 0.5f;
                float fz0 = z - fd * 0.5f, fz1 = z + fd * 0.5f;
                foreach (var other in footprints)
                {
                    float[] o = other.Value;
                    if (fx0 < o[1] - 0.01f && o[0] < fx1 - 0.01f
                        && fz0 < o[3] - 0.01f && o[2] < fz1 - 0.01f)
                    {
                        Debug.LogError("[wellcooked_demo] " + level + " footprint overlap at " + cellKey
                                       + ": " + kind + " x " + other.Key);
                        overlaps++;
                    }
                }
                footprints[cellKey] = new float[] { fx0, fx1, fz0, fz1 };   // mounts share the host cell on purpose
            }

            string prefabPath = PrefabPathFor(kind, prefabName);
            string soPath = SoPathFor(kind, soName);

            // A plate stack has to stand on a counter. If the layout already put a counter /
            // station on this cell, mount the stack on it; only a stand-alone plate stack
            // needs a generated Counter base. Creating a base on an occupied cell used to
            // leave two counters in the same cell and the plate stack could not be used.
            if (kind == "plate_stack")
            {
                GameObject existingHost;
                if (placedByCell.TryGetValue(cellKey, out existingHost) && existingHost != null)
                {
                    // Ground stations run first, so the layout's counter is already here.
                }
                else
                {
                    GameObject counterBase = AssetDatabase.LoadAssetAtPath<GameObject>(CounterPrefabPath);
                    GameObject counter = null;
                    if (counterBase != null)
                    {
                        counter = (GameObject)PrefabUtility.InstantiatePrefab(counterBase, scene);
                        counter.transform.SetParent(Group(scene, "Design/Counters"), false);
                        counter.transform.position = new Vector3(x, 0f, z);
                        counter.transform.localRotation = Quaternion.Euler(0f, yaw, 0f);
                        EditorUtility.SetDirty(counter);
                        PrefabUtility.RecordPrefabInstancePropertyModifications(counter);
                        counter.name = "counter_base_" + f[4] + "_" + f[5];
                        SetStubSo(counter, CounterSoPath);
                    }
                    else
                    {
                        Debug.LogError("[wellcooked_demo] cannot find " + CounterPrefabPath);
                    }
                    if (counter != null)
                    {
                        placedByCell[cellKey] = counter;
                        plateBases++;
                    }
                }
            }

            // Instantiate the placeholder PREFAB, not a bare GameObject. The prefab already
            // carries the specialised stub component together with its serialised prefabTag.
            // A hand-made "GameObject + PseudoPrefabStub + specialised stub" leaves that tag
            // empty, and the DIY runtime then throws InvalidCastException inside
            // PseudoPrefabServingStation.LateSetup -> PseudoPrefabManager.ResetAllPseudoPrefabs,
            // which wedges the whole level on the loading screen (bar full, never enters).
            GameObject placeholderPrefab = AssetDatabase.LoadAssetAtPath<GameObject>(prefabPath);
            if (placeholderPrefab == null)
            {
                Debug.LogError("[wellcooked_demo] cannot find prefab " + prefabPath);
                continue;
            }
            var go = (GameObject)PrefabUtility.InstantiatePrefab(placeholderPrefab, scene);
            go.transform.SetParent(Group(scene, IsUtensilKind(kind) ? "Design/Utensils" : "Design/Counters"), false);
            go.transform.position = new Vector3(x, 0f, z);
            go.transform.localRotation = Quaternion.Euler(0f, yaw, 0f);
            go.name = kind + "_" + soName + "_" + f[4] + "_" + f[5];
            SetStubSo(go, soPath);

            // A utensil / plate stack is attached through its cell:
            //   - if that cell already holds a cooker/frying/counter, the object is placed on
            //     the station's exact transform (a sibling, not a child: measured in game, a
            //     child at localPosition zero is swallowed by the station body and the pot
            //     disappears completely);
            //   - otherwise the layouts put the pot / pan / plate stack on its own cell and
            //     it is a stand-alone station there. It must never be dropped: deleting it
            //     used to punch holes into the kitchen wall and lose the cooking station.
            bool isMount = kind == "utensil" || kind == "plate_stack";
            if (isMount)
            {
                GameObject host;
                if (placedByCell.TryGetValue(cellKey, out host) && host != null)
                {
                    go.transform.position = host.transform.position + new Vector3(0f, UtensilYOffset, 0f);
                    go.transform.rotation = host.transform.rotation;
                    EditorUtility.SetDirty(go);
                    PrefabUtility.RecordPrefabInstancePropertyModifications(go);
                }
                else
                {
                    standaloneUtensils++;
                }
            }
            else
            {
                placedByCell[cellKey] = go;
            }

            // The specialised stub comes with the placeholder prefab, so it must be fetched,
            // never added again: a second stub would sit on the same object with an empty
            // pseudoPrefabSO / prefabTag and the runtime would rebuild nothing useful.
            if (kind == "dispenser")
            {
                var dispenser = go.GetComponentInChildren<PseudoPrefabDispenserStub>();
                if (dispenser != null && ingredient.Length > 0)
                {
                    var itemSo = AssetDatabase.LoadAssetAtPath<PseudoPrefabSO>("Assets/common01/food/Ingredients/" + ingredient + ".asset");
                    if (itemSo != null)
                    {
                        dispenser.spawnerItemPrefabSO = itemSo;
                        EditorUtility.SetDirty(dispenser);
                        PrefabUtility.RecordPrefabInstancePropertyModifications(dispenser);
                    }
                }
            }
            else if (kind == "plate_stack")
            {
                var plates = go.GetComponentInChildren<PseudoPrefabCleanPlateStackStub>();
                if (plates != null)
                {
                    plates.plateCount = 12;
                    EditorUtility.SetDirty(plates);
                    PrefabUtility.RecordPrefabInstancePropertyModifications(plates);
                }
            }
            else if (kind == "utensil")
            {
                var utensil = go.GetComponentInChildren<PseudoPrefabCookingUtensilStub>();
                if (utensil != null)
                {
                    // wellcooked_demo_3: every cooking utensil holds exactly one ingredient at a
                    // time (one dish per pot / pan / fryer). Other levels keep the game default.
                    utensil.capacity = (level == "wellcooked_demo_3") ? 1 : 4;
                    EditorUtility.SetDirty(utensil);
                    PrefabUtility.RecordPrefabInstancePropertyModifications(utensil);
                }
            }
            placed++;
        }
        EditorSceneManager.MarkSceneDirty(scene);
        // "Placed N objects; duplicates=.. badSpawns=.." - Chinese kept as \u escapes so
        // the build log stays readable while this source file remains pure ASCII.
        Debug.Log("[wellcooked_demo] " + level + " \u6446\u4e86 " + placed + " \u4e2a\u7269\u4f53"
                  + "; duplicates=" + duplicates + " badSpawns=" + badSpawns
                  + " overlaps=" + overlaps);
        Debug.Log("[wellcooked_demo] " + level + " plate stack counter bases=" + plateBases
                  + " standaloneUtensils=" + standaloneUtensils + " art=" + artPlaced);
        if (duplicates > 0 || badSpawns > 0 || overlaps > 0)
        {
            throw new System.Exception("[wellcooked_demo] " + level
                                + " layout self-check failed: duplicates=" + duplicates
                                + " badSpawns=" + badSpawns + " overlaps=" + overlaps);
        }
    }

    static float WallYaw(float gx, float gz)
    {
        if (gz >= RoomZ1) { return 180f; }      // north wall -> face south
        if (gz <= RoomZ0) { return 0f; }        // south wall -> face north
        if (gx <= RoomX0) { return 90f; }       // west wall -> face east
        if (gx >= RoomX1) { return 270f; }      // east wall -> face west
        return 0f;
    }

    // The serving window is a pass, not a piece of furniture: the arrow baked into the
    // prefab marks where plates leave the kitchen, so it has to point AWAY from the room.
    // The arrow points along the prefab's local +Z (measured in game: yaw 270 -> arrow on
    // the west side, yaw 90 -> arrow on the east side), hence the outward mapping:
    //   east wall  ->  90 (arrow +X)      north wall ->   0 (arrow +Z)
    //   south wall -> 180 (arrow -Z)      west wall  -> 270 (arrow -X)
    // An island pass (level 3) points east, toward the ring corridor (yaw 90).
    static float ServingYaw(float gx, float gz)
    {
        if (gz >= RoomZ1) { return 0f; }        // north wall -> arrow north (out)
        if (gz <= RoomZ0) { return 180f; }      // south wall -> arrow south (out)
        if (gx <= RoomX0) { return 270f; }      // west wall -> arrow west (out)
        if (gx >= RoomX1) { return 90f; }       // east wall -> arrow east (out)
        return 90f;                             // island pass -> arrow east
    }

    static bool IsInsideRoom(float gx, float gz)
    {
        return gx > RoomX0 && gx < RoomX1 && gz > RoomZ0 && gz < RoomZ1;
    }

    static void SetStubSo(GameObject go, string soPath)
    {
        if (string.IsNullOrEmpty(soPath))
        {
            return;
        }
        var stub = go.GetComponentInChildren<PseudoPrefabStub>();
        if (stub == null)
        {
            return;
        }
        var so = AssetDatabase.LoadAssetAtPath<PseudoPrefabSO>(soPath);
        if (so != null)
        {
            stub.pseudoPrefabSO = so;
            // Without the explicit record the value never reaches the saved scene and the DIY
            // runtime plugin sees an empty pseudoPrefabSO, so it cannot rebuild the object as a
            // real, interactive kitchen piece (this is why nothing used to highlight in game).
            EditorUtility.SetDirty(stub);
            PrefabUtility.RecordPrefabInstancePropertyModifications(stub);
        }
    }

    // Fetch (creating missing levels of) a group path inside the scene. The template scene
    // already ships Design, Design/Counters and Design/Utensils.
    static Transform Group(Scene scene, string path)
    {
        string[] parts = path.Split('/');
        int index = 0;
        Transform current = null;
        foreach (GameObject root in scene.GetRootGameObjects())
        {
            if (root.name == parts[0])
            {
                current = root.transform;
                break;
            }
        }
        if (current != null)
        {
            index = 1;
            while (index < parts.Length)
            {
                Transform child = current.Find(parts[index]);
                if (child == null)
                {
                    break;
                }
                current = child;
                index++;
            }
        }
        for (; index < parts.Length; index++)
        {
            var go = new GameObject(parts[index]);
            if (go.scene != scene)
            {
                SceneManager.MoveGameObjectToScene(go, scene);
            }
            if (current != null)
            {
                go.transform.SetParent(current, false);
            }
            current = go.transform;
        }
        return current;
    }

    // The vanilla / example levels place chefs by having "Chefs/Player 1..4" objects that carry
    // a PseudoPrefab pointing at common01/pseudo_prefab_so/PlayerSO.asset. A plain empty named
    // "spawn" is ignored by the game, which is why chefs used to appear near the world origin.
    const string PlayerSoPath = "Assets/common01/pseudo_prefab_so/PlayerSO.asset";

    // The template scene ships "Chefs/Player 1..4" prefab instances at fixed positions; the game
    // spawns chefs on THOSE (a plain "spawn" empty is ignored - verified via the plugin state stream).
    // So instead of creating new player objects we just move the template ones onto the layout cells.
    static void ApplySpawnPositions(Scene scene, List<Vector3> positions, string level)
    {
        if (positions.Count == 0) return;
        Transform chefs = null;
        foreach (GameObject root in scene.GetRootGameObjects())
        {
            if (root.name == "Chefs") { chefs = root.transform; break; }
        }
        if (chefs == null)
        {
            Debug.LogWarning("[wellcooked_demo] " + level + ": no Chefs group in the template scene");
            return;
        }
        var players = new List<Transform>();
        for (int i = 0; i < chefs.childCount; i++)
        {
            Transform child = chefs.GetChild(i);
            if (child.name.StartsWith("Player")) players.Add(child);
        }
        players.Sort((a, b) => string.CompareOrdinal(a.name, b.name));
        for (int i = 0; i < players.Count; i++)
        {
            // 多出来的玩家挤到最后一个出生点上，避免留在模板的老位置
            Vector3 p = positions[Mathf.Min(i, positions.Count - 1)];
            players[i].position = p;
            players[i].localRotation = Quaternion.identity;
            EditorUtility.SetDirty(players[i].gameObject);
        }
        Debug.Log("[wellcooked_demo] " + level + " spawn positions applied: players=" + players.Count
                  + " layoutSpawns=" + positions.Count);
    }

    // The template scene already ships "Chefs/Player 1..4" at fixed positions; the game uses those
    // (and ignores a plain "spawn" empty), so they must be removed before our own are created.
    static void ClearTemplatePlayers(Scene scene)
    {
        foreach (GameObject root in scene.GetRootGameObjects())
        {
            if (root.name != "Chefs") continue;
            for (int i = root.transform.childCount - 1; i >= 0; i--)
            {
                Object.DestroyImmediate(root.transform.GetChild(i).gameObject);
            }
            return;
        }
    }

    static void PlaceSpawn(Transform parent, float x, float z, int index)
    {
        // Keep the plain empty: an empty named "spawn" is what the generator has always produced.
        // (The template scene's own "Chefs/Player 1..4" objects are what actually place chefs in
        // game; wiring our own player stubs made the game NRE in PlayerSwitchingManager, so that
        // experiment was reverted. See BACKGROUND.md for the open issue.)
        var go = new GameObject("spawn");
        go.transform.SetParent(parent, false);
        go.transform.position = new Vector3(x, 0f, z);   // x,z 已是米
        go.transform.localRotation = Quaternion.identity;
        EditorUtility.SetDirty(go);
    }

    static bool IsUtensilKind(string kind)
    {
        return kind == "utensil" || kind == "plate_stack";
    }

    // Whether a prefab instance can be resolved in any of the imported common bundles.
    static bool ArtPaths(string theme, string prefabName, out string prefabPath, out string soPath)
    {
        string[] roots = { "common01", "common02", "common03" };
        foreach (string root in roots)
        {
            string p = "Assets/" + root + "/prefabs/art/" + theme + "/" + prefabName + ".prefab";
            if (AssetDatabase.LoadAssetAtPath<GameObject>(p) != null)
            {
                prefabPath = p;
                soPath = "Assets/" + root + "/pseudo_prefab_so/art/" + theme + "/" + prefabName + ".asset";
                return true;
            }
        }
        prefabPath = null;
        soPath = null;
        return false;
    }

    static bool PlaceArt(Scene scene, string level, string[] f, float x, float z)
    {
        string theme = f[1];
        string prefabName = f[2];
        float y = 0f;
        if (f.Length > 3 && !string.IsNullOrEmpty(f[3]))
        {
            float.TryParse(f[3], NumberStyles.Float, CultureInfo.InvariantCulture, out y);
        }
        float yaw = 0f;
        if (f.Length > 7 && !string.IsNullOrEmpty(f[7]))
        {
            float.TryParse(f[7], NumberStyles.Float, CultureInfo.InvariantCulture, out yaw);
        }

        string prefabPath, soPath;
        if (!ArtPaths(theme, prefabName, out prefabPath, out soPath))
        {
            Debug.LogError("[wellcooked_demo] " + level + " cannot find art prefab "
                           + theme + "/" + prefabName);
            return false;
        }

        Transform artGroup = Group(scene, "Art");
        var go = Placeholder(artGroup,
                             "art_" + prefabName + "_" + f[4] + "_" + f[5],
                             soPath,
                             new Vector3(x, y, z), yaw);
        if (go == null)
        {
            Debug.LogError("[wellcooked_demo] " + level + " cannot find art so "
                           + theme + "/" + prefabName);
            return false;
        }
        return true;
    }

    // A DIY placeholder is a bare GameObject plus its stub pair: PseudoPrefabStub carries the
    // PseudoPrefabSO (which asset to build) and PseudoPrefab rebuilds the real prefab from the
    // game bundles at runtime. Instantiating the visual prefab into the scene instead (what this
    // generator did before) produces a fully visible but completely inert object in game: it can
    // never be highlighted or used, which is exactly the reported symptom.
    static GameObject Placeholder(Transform parent, string name, string soPath, Vector3 position, float yaw)
    {
        var so = AssetDatabase.LoadAssetAtPath<PseudoPrefabSO>(soPath);
        if (so == null)
        {
            return null;
        }
        var go = new GameObject(name);
        go.transform.SetParent(parent, false);
        go.transform.position = position;
        go.transform.localRotation = Quaternion.Euler(0f, yaw, 0f);
        var stub = go.AddComponent<PseudoPrefabStub>();
        stub.pseudoPrefabSO = so;
        go.AddComponent<PseudoPrefab>();
        EditorUtility.SetDirty(go);
        return go;
    }

    static string PrefabPathFor(string kind, string prefabName)
    {
        return "Assets/common01/prefabs/" + (IsUtensilKind(kind) ? "utensils/" : "counters/") + prefabName + ".prefab";
    }

    static string SoPathFor(string kind, string soName)
    {
        return "Assets/common01/pseudo_prefab_so/" + (IsUtensilKind(kind) ? "utensils/" : "counters/") + soName + ".asset";
    }

    static void AttachLevelInfo(Scene scene, LevelInfoSO levelInfo)
    {
        foreach (GameObject root in scene.GetRootGameObjects())
        {
            var stub = root.GetComponentInChildren<PseudoPrefabManagerStub>(true);
            if (stub != null)
            {
                stub.levelInfo = levelInfo;
                EditorUtility.SetDirty(stub);
                break;
            }
        }
        EditorSceneManager.MarkSceneDirty(scene);
    }

    static void SetBundleNames(string level)
    {
        AssetImporter sceneImporter = AssetImporter.GetAtPath(ScenePathOf(level));
        if (sceneImporter != null)
        {
            sceneImporter.assetBundleName = "s_" + level;
            EditorUtility.SetDirty(sceneImporter);
        }
        AssetImporter setImporter = AssetImporter.GetAtPath(SetDir);
        if (setImporter != null)
        {
            setImporter.assetBundleName = "info_" + SetName;
            EditorUtility.SetDirty(setImporter);
        }
    }
}
