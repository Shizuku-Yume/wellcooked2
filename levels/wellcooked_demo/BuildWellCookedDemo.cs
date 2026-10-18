// wellcooked_demo 关卡生成器（编辑器脚本）。
//
// 放到编辑器工程的 Assets/Editor/ 下，然后把 layout.tsv 拷到
// Assets/LevelSets/wellcooked_demo/layout.tsv（脚本读它摆台），
// 菜单 Tools → WellCooked → 生成关卡集与场景。
//
// 生成的东西：
//   Assets/LevelSets/wellcooked_demo/data/wellcooked_demo_1/{LevelInfo_*.asset, config_1p..4p.asset}
//   Assets/LevelSets/wellcooked_demo/LevelSetInfo.asset
//   Assets/LevelSets/wellcooked_demo/scenes/s_wellcooked_demo_1.unity（从 Assets/Template/s_template.unity 复制）
//   bundle 名：集合目录 = info_wellcooked_demo、场景 = s_wellcooked_demo_1
// 之后照教程用 Tools → Toggle Prepare For Building → 保存 → Tools → Build AssetBundles 出包。
//
// 设计意图（为什么这样摆）：一屋子、无机关；4 厨；3 道菜覆盖 切（2 砧板）/煮（灶+锅）/煎（炸台+锅）/装盘（盘堆+上菜口），
// 两道菜共用砧板与盘子堆 —— 资源争用是调度演示要看的东西。

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
    const string LevelName = "wellcooked_demo_1";
    const string SetDir = "Assets/LevelSets/" + SetName;
    // 这个常量不要叫 SceneAsset：LoadAssetAtPath<SceneAsset>() 里的 SceneAsset 是
    // UnityEditor.SceneAsset 类型名，同名遮蔽虽然合法（已实测两套编译器都能过），但极易误读。
    const string ScenePath = SetDir + "/scenes/s_" + LevelName + ".unity";
    const string DataDir = SetDir + "/data/" + LevelName;
    const string LayoutAsset = SetDir + "/layout.tsv";
    const string ConfigTemplateSO = "Assets/common01/pseudo_prefab_so/game_environment/LevelConfigTemplateSO.asset";
    const string LevelInfoTemplate = "Assets/Template/levelinfo_template.asset";
    const string SceneTemplate = "Assets/Template/s_template.unity";

    static readonly string[] Recipes =
    {
        "Assets/common01/food/Recipes/Burrito_Chicken_SO.asset",
        "Assets/common01/food/Recipes/Sushi_PlainFish_SO.asset",
        "Assets/common01/food/Recipes/Sushi_PlainPrawn_SO.asset",
    };

    [MenuItem("Tools/WellCooked/生成关卡集与场景")]
    public static void Build()
    {
        CreateFolders();
        LevelInfoSO levelInfo = CreateLevelInfo();
        CreateLevelSetInfo(levelInfo);
        Scene scene = CreateScene();
        // batchmode 下 Awake/OnEnable 不会自动跑（-nographics 没有场景更新循环），
        // 伪预制体管理器必须显式 DeInit→Init；否则摆放时拿到的预制体是没有资源的空壳，
        // PseudoPrefabManager 会刷 VertexData 报错、一个物体都摆不上。
        // 等价于菜单 Tools → Reload Pseudo Assets。
        // 注意：Init 之后摆出来的是「真实预览物体」（场景会含整套层级），这是对的；
        // 卸载 AssetBundle 的动作放在驱动脚本里——保存场景之后、打包之前。
        PseudoPrefabManager.Instance.DeInit();
        PseudoPrefabManager.Instance.Init();
        Debug.Log("[wellcooked_demo] 已初始化伪预制体管理器");
        PlaceFromLayout(scene);
        AttachLevelInfo(scene, levelInfo);
        // README §5.1 第 7 条：场景标记 s_wellcooked_demo_1、关卡集目录标记 info_wellcooked_demo
        SetBundleNames();
        AssetDatabase.SaveAssets();
        AssetDatabase.Refresh();
        Debug.Log("[wellcooked_demo] 完成。下一步：Tools → Toggle Prepare For Building → 保存场景 → Tools → Build AssetBundles");
    }

    [MenuItem("Tools/WellCooked/自检（bundle 名与文件）")]
    public static void SelfCheck()
    {
        Debug.Log("[wellcooked_demo] layout 存在=" + File.Exists(LayoutAsset)
                  + " scene=" + AssetDatabase.LoadAssetAtPath<SceneAsset>(ScenePath)
                  + " sceneBundle=" + BundleNameOf(ScenePath)
                  + " setBundle=" + BundleNameOf(SetDir));
    }

    // 资产或目录还不存在时 AssetImporter.GetAtPath 返回 null，自检不能因此抛空引用。
    static string BundleNameOf(string path)
    {
        AssetImporter importer = AssetImporter.GetAtPath(path);
        if (importer == null)
        {
            return "(路径不存在)";
        }
        return importer.assetBundleName.Length == 0 ? "(未设置)" : importer.assetBundleName;
    }

    static void CreateFolders()
    {
        foreach (string path in new[] { SetDir, SetDir + "/scenes", SetDir + "/data", DataDir })
        {
            if (AssetDatabase.IsValidFolder(path))
            {
                continue;
            }
            string parent = Path.GetDirectoryName(path).Replace('\\', '/');
            AssetDatabase.CreateFolder(parent, Path.GetFileName(path));
        }
    }

    static LevelInfoSO CreateLevelInfo()
    {
        string path = DataDir + "/LevelInfo_" + LevelName + ".asset";
        var info = AssetDatabase.LoadAssetAtPath<LevelInfoSO>(path);
        if (info == null)
        {
            AssetDatabase.CopyAsset(LevelInfoTemplate, path);
            info = AssetDatabase.LoadAssetAtPath<LevelInfoSO>(path);
        }
        info.levelName = LevelName;
        info.levelNameZH = "WellCooked 演示关卡";
        info.sceneName = "s_" + LevelName;
        info.recipes = Recipes.Select(AssetDatabase.LoadAssetAtPath<ScriptableObject>).ToArray();
        info.debugRecipeCount = 0;
        info.disableDynamicParenting = true;

        // 1..4 人配置：从模板拷，再按人数调订单节奏。
        // 这三个字段在 LevelConfigSetupPerPlayerCountSO 里都是 int，所以传整数。
        info.config_1p = CopyConfig(1, 90, 6, 22);
        info.config_2p = CopyConfig(2, 100, 6, 30);
        info.config_3p = CopyConfig(3, 100, 6, 34);
        info.config_4p = CopyConfig(4, 100, 6, 40);
        EditorUtility.SetDirty(info);
        return info;
    }

    static LevelConfigSetupPerPlayerCountSO CopyConfig(int players, int roundTime, int betweenOrders, int lifetime)
    {
        string src = "Assets/Template/config_" + players + "p.asset";
        string dst = DataDir + "/config_" + players + "p.asset";
        if (AssetDatabase.LoadAssetAtPath<LevelConfigSetupPerPlayerCountSO>(dst) == null)
        {
            AssetDatabase.CopyAsset(src, dst);
        }
        var cfg = AssetDatabase.LoadAssetAtPath<LevelConfigSetupPerPlayerCountSO>(dst);
        if (cfg == null)
        {
            Debug.LogError("[wellcooked_demo] 拷不到配置模板 " + src);
            return null;
        }
        cfg.roundTime = roundTime;
        cfg.timeBetweenOrders = betweenOrders;
        cfg.orderLifeTime = lifetime;
        EditorUtility.SetDirty(cfg);
        return cfg;
    }

    static void CreateLevelSetInfo(LevelInfoSO levelInfo)
    {
        // 名字必须是 LevelSetInfo：DIY 加载器按这个文件名找集合信息
        string path = SetDir + "/LevelSetInfo.asset";
        var info = AssetDatabase.LoadAssetAtPath<LevelSetInfoSO>(path);
        if (info == null)
        {
            info = ScriptableObject.CreateInstance<LevelSetInfoSO>();
            AssetDatabase.CreateAsset(info, path);
        }
        info.levelSetName = SetName;
        info.levelSetNameZH = "WellCooked 演示关卡集";
        info.uid = SetName;
        info.version = "1.0";
        info.levelInfos = new[] { levelInfo };
        EditorUtility.SetDirty(info);
    }

    static Scene CreateScene()
    {
        if (AssetDatabase.LoadAssetAtPath<SceneAsset>(ScenePath) == null)
        {
            AssetDatabase.CopyAsset(SceneTemplate, ScenePath);
        }
        return EditorSceneManager.OpenScene(ScenePath, OpenSceneMode.Single);
    }

    static void PlaceFromLayout(Scene scene)
    {
        // 注意：.NET 2.0 Subset（本工程 apiCompatibilityLevel=2）的 Path.Combine 只有两个参数的重载，
        // Path.Combine(a, b, c) 会报 CS1501，所以嵌套调用。
        string repoLayout = Path.GetFullPath(Path.Combine(Path.Combine(Application.dataPath, ".."), LayoutAsset));
        string path = File.Exists(LayoutAsset) ? LayoutAsset : (File.Exists(repoLayout) ? repoLayout : null);
        if (path == null)
        {
            Debug.LogError("[wellcooked_demo] 找不到 layout.tsv：把它拷到 " + LayoutAsset);
            return;
        }

        int placed = 0;
        foreach (string raw in File.ReadAllLines(path))
        {
            string line = raw.Trim();
            if (line.Length == 0 || line[0] == '#')
            {
                continue;
            }
            string[] f = line.Split('\t');
            // 第 7 列 note 只给人看、脚本不读（README §5.2），所以只要求前 6 列存在：
            // note 为空的行被 Trim 掉行尾制表符后只剩 6 列，不应该被当成坏行丢掉。
            if (f.Length < 6 || f[0] == "kind")
            {
                continue;
            }
            string kind = f[0], prefabName = f[1], soName = f[2], ingredient = f[3];
            // 用 InvariantCulture 解析格号，免得系统区域设置用逗号做小数点时解析失败。
            float x = float.Parse(f[4], CultureInfo.InvariantCulture) * 0.6f;
            float z = float.Parse(f[5], CultureInfo.InvariantCulture) * 0.6f;

            if (kind == "spawn")
            {
                PlaceSpawn(Group(scene, "Design"), x, z);
                placed++;
                continue;
            }

            string prefabPath = PrefabPathFor(kind, prefabName);
            string soPath = SoPathFor(kind, soName);
            GameObject prefab = AssetDatabase.LoadAssetAtPath<GameObject>(prefabPath);
            if (prefab == null)
            {
                Debug.LogError("[wellcooked_demo] 找不到预制体 " + prefabPath);
                continue;
            }
            var go = (GameObject)PrefabUtility.InstantiatePrefab(prefab, scene);
            // 教程 Docs/zh/tutorial.md:62（#No-Root）要求物体不能挂在场景根物体上，
            // 模板场景 s_template.unity 里正好预留了空的 Design/Counters 与 Design/Utensils。
            go.transform.SetParent(Group(scene, IsUtensilKind(kind) ? "Design/Utensils" : "Design/Counters"), false);
            go.transform.position = new Vector3(x, 0f, z);
            go.name = kind + "_" + soName + "_" + f[4] + "_" + f[5];

            var stub = go.GetComponentInChildren<PseudoPrefabStub>();
            if (stub != null && soPath != null)
            {
                stub.pseudoPrefabSO = AssetDatabase.LoadAssetAtPath<PseudoPrefabSO>(soPath);
            }
            var dispenser = go.GetComponentInChildren<PseudoPrefabDispenserStub>();
            if (dispenser != null && ingredient.Length > 0)
            {
                dispenser.spawnerItemPrefabSO = AssetDatabase.LoadAssetAtPath<PseudoPrefabSO>("Assets/common01/food/Ingredients/" + ingredient + ".asset");
            }
            var plates = go.GetComponentInChildren<PseudoPrefabCleanPlateStackStub>();
            if (plates != null)
            {
                plates.plateCount = 12;
            }
            if (kind == "utensil")
            {
                var utensil = go.GetComponentInChildren<PseudoPrefabCookingUtensilStub>();
                if (utensil != null)
                {
                    utensil.capacity = 4;
                }
            }
            placed++;
        }
        EditorSceneManager.MarkSceneDirty(scene);
        Debug.Log("[wellcooked_demo] 摆了 " + placed + " 个物体");
    }

    // 取场景里已有的分组（模板场景自带 Design、Design/Counters、Design/Utensils），缺哪级补哪级。
    // 这样重复执行生成不会造出第二个 Design 根物体。path 形如 "Design/Counters"。
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

    static void PlaceSpawn(Transform parent, float x, float z)
    {
        var go = new GameObject("spawn");
        go.transform.SetParent(parent, false);
        go.transform.position = new Vector3(x, 0f, z);
    }

    // 第 2/3 列的目录规则（README §5.2）：器具在 utensils/，台面在 counters/。
    // 干净盘堆的 kind 是 plate_stack，但它在游戏资源里属于器具：
    //   Assets/common01/prefabs/utensils/CleanPlateStack.prefab
    //   Assets/common01/pseudo_prefab_so/utensils/CleanPlateStackSO.asset
    // counters/ 下没有同名文件，所以必须归到 utensils/，否则这一项会摆不出来（只摆 19 个）。
    static bool IsUtensilKind(string kind)
    {
        return kind == "utensil" || kind == "plate_stack";
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
        // 要改的是编辑器侧的 stub（PseudoPrefabManagerStub.levelInfo），运行时的
        // PseudoPrefabManager 由它在构建/运行时读出来，不直接持有 LevelInfoSO。
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

    static void SetBundleNames()
    {
        AssetImporter sceneImporter = AssetImporter.GetAtPath(ScenePath);
        if (sceneImporter != null)
        {
            sceneImporter.assetBundleName = "s_" + LevelName;
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
