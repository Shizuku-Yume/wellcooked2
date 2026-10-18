using System;
using System.IO;
using System.Linq;
using System.Reflection;
using LevelEditor;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.SceneManagement;

/// <summary>
/// 无人值守批处理驱动：把 levels/wellcooked_demo/README.md 第 6、8 步的三次菜单点击
/// （Tools → WellCooked → 生成关卡集与场景 / Tools → Toggle Prepare For Building / Ctrl+S / Tools → Build AssetBundles）
/// 串成一次 -executeMethod 调用。
///
/// 用法：
///   Unity.exe -batchmode -nographics -quit \
///            -projectPath &lt;工程&gt; \
///            -executeMethod BatchBuildWellCookedDemo.Run \
///            -logFile &lt;日志&gt;
///
/// 与手工点击的对应关系：
///   第 6 步        → BuildWellCookedDemo.Build()
///   第 8.1 步      → PseudoPrefabManager.DeInit() + prepareForBuilding = true
///   第 8.2 步      → EditorSceneManager.SaveScene()
///   第 8.3 步      → bundle 名由 Build() 内部设置（可用 BuildWellCookedDemo.SelfCheck 复核）
///   第 8.4 步      → BuildPipeline.BuildAssetBundles()
///
/// 之所以直接操作场景里的 PseudoPrefabManager，而不是调用工程自带的
/// CreateAssetBundles.TogglePrepareForBuilding()：那个方法是 private static 且依赖
/// PseudoPrefabManager.Instance 单例，批处理下单例可能未经 Awake 初始化。
/// </summary>
public static class BatchBuildWellCookedDemo
{
    private const string BundleDir = "Assets/AssetBundles";

    public static void Run()
    {
        Debug.Log("[batch] ===== 开始无人值守构建 =====");

        // ---- 第 6 步：生成关卡集与场景（内部会打开新场景）----
        BuildWellCookedDemo.Build();

        Scene scene = EditorSceneManager.GetActiveScene();
        Debug.Log(string.Format("[batch] 活动场景 = '{0}'  isLoaded={1}  根物体数={2}",
            scene.path, scene.isLoaded, scene.GetRootGameObjects().Length));

        // ---- 第 8.1 步：清除临时编辑物体，并把 prepareForBuilding 置真 ----
        // 打包前置校验 TargetSceneSaveValidator.CheckPrepareForBuilding 要求该字段为真，
        // 否则保存与构建都会被拦下（批处理下弹窗失效，会静默跳过）。
        PseudoPrefabManager manager = scene.GetRootGameObjects()
            .Select(root => root.GetComponent<PseudoPrefabManager>())
            .FirstOrDefault(comp => comp != null);

        if (manager == null)
        {
            Debug.LogWarning("[batch] 场景根节点上没有 PseudoPrefabManager —— 跳过临时物体清理");
        }
        else
        {
            // 这里绝不能调 DeInit()：它会把 PlaceFromLayout 摆好的物体从场景里摘掉，
            // 于是保存下来的是一张空场景（实测就是这样丢掉 20 个物体的）。
            // 打包前置校验 TargetSceneSaveValidator.CheckPrepareForBuilding 只认下面这个标记字段。
            manager.prepareForBuilding = true;
            Debug.Log("[batch] prepareForBuilding = " + manager.prepareForBuilding);
        }

        // ---- 第 8.2 步：保存场景 ----
        EditorSceneManager.MarkSceneDirty(scene);
        bool saved = EditorSceneManager.SaveScene(scene);
        AssetDatabase.SaveAssets();
        AssetDatabase.Refresh();
        Debug.Log("[batch] 保存场景 -> " + saved);

        // ---- 第 8.3 步：场景已落盘，再卸载 AssetBundle ----
        // PlaceFromLayout 摆好的物体此刻已经写进 .unity 文件，DeInit 只影响内存，
        // 同时释放 PseudoPrefabManager 持有的 AssetBundle 句柄。
        // 不 DeInit 的话 BuildPipeline.BuildAssetBundles 会静默返回 null。
        // DeInit 之后绝不能再保存场景，否则又把摆好的东西摘掉。
        if (manager != null)
        {
            MethodInfo deInit = typeof(PseudoPrefabManager).GetMethod(
                "DeInit", BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
            if (deInit != null)
            {
                deInit.Invoke(manager, null);
                Debug.Log("[batch] 场景已保存，随后 DeInit() 卸载 AssetBundle");
            }
        }

        // ---- 第 8.4 步：打包（bundle 名已由 Build() 设置）----
        if (!Directory.Exists(BundleDir))
        {
            Directory.CreateDirectory(BundleDir);
        }

        // layout.tsv 是生成器的输入，不是 Unity 资源；它躺在被打成 bundle 的
        // Assets/LevelSets/wellcooked_demo/ 目录里，会让 BuildAssetBundles 直接失败：
        //   Unrecognized assets cannot be included in AssetBundles: ".../layout.tsv"
        // 光挪走文件不够——孤儿 .meta 会让导入器反复报错，所以连 .meta 一起删掉，
        // 打完再把内容原样写回（tsv 的 GUID 无人引用，重建无副作用）。
        const string layout = "Assets/LevelSets/wellcooked_demo/layout.tsv";
        byte[] layoutBytes = File.Exists(layout) ? File.ReadAllBytes(layout) : null;
        if (layoutBytes != null)
        {
            File.Delete(layout);
            if (File.Exists(layout + ".meta"))
            {
                File.Delete(layout + ".meta");
            }
            AssetDatabase.Refresh();
            Debug.Log("[batch] 打包前删除 layout.tsv（含 .meta）");
        }

        AssetBundleManifest manifest = BuildPipeline.BuildAssetBundles(
            BundleDir, BuildAssetBundleOptions.None, BuildTarget.StandaloneWindows);

        if (layoutBytes != null)
        {
            File.WriteAllBytes(layout, layoutBytes);
            AssetDatabase.Refresh();
            Debug.Log("[batch] 打包后写回 layout.tsv");
        }

        if (manifest == null)
        {
            throw new Exception("[batch] BuildAssetBundles 返回 null —— 打包失败");
        }

        string[] bundles = manifest.GetAllAssetBundles();
        Debug.Log("[batch] 打包完成，bundle 数 = " + bundles.Length);
        foreach (string bundle in bundles)
        {
            Debug.Log("[batch]   bundle: " + bundle);
        }

        foreach (string name in new[] { "info_wellcooked_demo", "s_wellcooked_demo_1" })
        {
            string path = Path.Combine(BundleDir, name);
            Debug.Log(string.Format("[batch] 产物 {0}: {1}", name,
                File.Exists(path) ? (new FileInfo(path).Length + " 字节") : "缺失"));
        }

        Debug.Log("[batch] ===== 构建结束 =====");
    }
}
