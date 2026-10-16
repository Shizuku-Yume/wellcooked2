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

using System.Collections.Generic;
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
    const string SceneAsset = SetDir + "/scenes/s_" + LevelName + ".unity";
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
        PlaceFromLayout(scene);
        AttachLevelInfo(scene, levelInfo);
        AssetDatabase.SaveAssets();
        AssetDatabase.Refresh();
        Debug.Log("[wellcooked_demo] 完成。下一步：Tools → Toggle Prepare For Building → 保存场景 → Tools → Build AssetBundles");
    }

    [MenuItem("Tools/WellCooked/自检（bundle 名与文件）")]
    public static void SelfCheck()
    {
        Debug.Log("[wellcooked_demo] layout 存在=" + File.Exists(LayoutAsset)
                  + " scene=" + AssetDatabase.LoadAssetAtPath<SceneAsset>(SceneAsset)
                  + " sceneBundle=" + AssetImporter.GetAtPath(SceneAsset).assetBundleName
                  + " setBundle=" + AssetImporter.GetAtPath(SetDir).assetBundleName);
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

        // 1..4 人配置：从模板拷，再按人数调订单节奏
        info.config_1p = CopyConfig(1, 90f, 6f, 22f);
        info.config_2p = CopyConfig(2, 100f, 6f, 30f);
        info.config_3p = CopyConfig(3, 100f, 6f, 34f);
        info.config_4p = CopyConfig(4, 100f, 6f, 40f);
        EditorUtility.SetDirty(info);
        return info;
    }

    static LevelConfigSetupPerPlayerCountSO CopyConfig(int players, float roundTime, float betweenOrders, float lifetime)
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
        if (AssetDatabase.LoadAssetAtPath<SceneAsset>(SceneAsset) == null)
        {
            AssetDatabase.CopyAsset(SceneTemplate, SceneAsset);
        }
        return EditorSceneManager.OpenScene(SceneAsset, OpenSceneMode.Single);
    }

    static void PlaceFromLayout(Scene scene)
    {
        string repoLayout = Path.GetFullPath(Path.Combine(Application.dataPath, "..", LayoutAsset));
        string path = File.Exists(LayoutAsset) ? LayoutAsset : (File.Exists(repoLayout) ? repoLayout : null);
        if (path == null)
        {
            Debug.LogError("[wellcooked_demo] 找不到 layout.tsv：把它拷到 " + LayoutAsset);
            return;
        }

        var roots = new Dictionary<string, Transform>();
        int placed = 0;
        foreach (string raw in File.ReadAllLines(path))
        {
            string line = raw.Trim();
            if (line.Length == 0 || line[0] == '#')
            {
                continue;
            }
            string[] f = line.Split('\t');
            if (f.Length < 7 || f[0] == "kind")
            {
                continue;
            }
            string kind = f[0], prefabName = f[1], soName = f[2], ingredient = f[3];
            float x = float.Parse(f[4]) * 0.6f, z = float.Parse(f[5]) * 0.6f;

            if (kind == "spawn")
            {
                PlaceSpawn(Group(roots, scene, "Design"), x, z);
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

    static Transform Group(Dictionary<string, Transform> roots, Scene scene, string name)
    {
        Transform t;
        if (roots.TryGetValue(name, out t) && t != null)
        {
            return t;
        }
        var go = new GameObject(name);
        SceneManager.MoveGameObjectToScene(go, scene);
        roots[name] = go.transform;
        return go.transform;
    }

    static void PlaceSpawn(Transform parent, float x, float z)
    {
        var go = new GameObject("spawn");
        go.transform.SetParent(parent, false);
        go.transform.position = new Vector3(x, 0f, z);
    }

    static string PrefabPathFor(string kind, string prefabName)
    {
        switch (kind)
        {
            case "utensil":
                return "Assets/common01/prefabs/utensils/" + prefabName + ".prefab";
            default:
                return "Assets/common01/prefabs/counters/" + prefabName + ".prefab";
        }
    }

    static string SoPathFor(string kind, string soName)
    {
        switch (kind)
        {
            case "utensil":
                return "Assets/common01/pseudo_prefab_so/utensils/" + soName + ".asset";
            default:
                return "Assets/common01/pseudo_prefab_so/counters/" + soName + ".asset";
        }
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
        AssetImporter sceneImporter = AssetImporter.GetAtPath(SceneAsset);
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
