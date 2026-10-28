using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using LevelEditor;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.SceneManagement;

/// <summary>
/// 无人值守批处理驱动（多关卡版）：一次 -executeMethod 完成
///   生成 4 个关卡与场景 → 保存 → 释放 AssetBundle → 删除 layout*.tsv → BuildAssetBundles。
///
/// 用法：
///   Unity.exe -batchmode -nographics -quit \
///            -projectPath &lt;工程&gt; \
///            -executeMethod BatchBuildWellCookedDemo.Run \
///            -logFile &lt;日志&gt;
/// </summary>
public static class BatchBuildWellCookedDemo
{
    private const string BundleDir = "Assets/AssetBundles";
    private const string SetDir = "Assets/LevelSets/wellcooked_demo";

    /// <summary>
    /// 只打包：场景已经生成并保存好时用（不重新生成，避免重复摆放物体）。
    /// </summary>
    public static void BuildOnly()
    {
        Debug.Log("[batch] ===== 只打包（跳过生成） =====");
        // 先切到空场景：确保没有任何 PseudoPrefabManager 还持有 AssetBundle，
        // 否则 BuildAssetBundles 会因为包被占用而失败。
        EditorSceneManager.NewScene(NewSceneSetup.EmptyScene, NewSceneMode.Single);
        Scene scene = EditorSceneManager.GetActiveScene();
        Debug.Log("[batch] 活动场景 = '" + scene.path + "'");

        PseudoPrefabManager manager = scene.GetRootGameObjects()
            .Select(root => root.GetComponent<PseudoPrefabManager>())
            .FirstOrDefault(comp => comp != null);
        if (manager != null)
        {
            MethodInfo deInit = typeof(PseudoPrefabManager).GetMethod(
                "DeInit", BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
            if (deInit != null)
            {
                deInit.Invoke(manager, null);
            }
            Debug.Log("[batch] 已 DeInit()");
        }

        Package();
    }

    static void Package()
    {
        var parked = new Dictionary<string, byte[]>();
        foreach (string path in Directory.GetFiles(SetDir, "layout*.tsv"))
        {
            string normalized = path.Replace('\\', '/');
            parked[normalized] = File.ReadAllBytes(normalized);
            File.Delete(normalized);
            if (File.Exists(normalized + ".meta"))
            {
                File.Delete(normalized + ".meta");
            }
        }
        if (parked.Count > 0)
        {
            AssetDatabase.Refresh();
            Debug.Log("[batch] 打包前删除 " + parked.Count + " 个 layout tsv");
        }

        if (!Directory.Exists(BundleDir))
        {
            Directory.CreateDirectory(BundleDir);
        }

        AssetBundleManifest manifest = BuildPipeline.BuildAssetBundles(
            BundleDir, BuildAssetBundleOptions.None, BuildTarget.StandaloneWindows);

        foreach (var pair in parked)
        {
            File.WriteAllBytes(pair.Key, pair.Value);
        }
        if (parked.Count > 0)
        {
            AssetDatabase.Refresh();
            Debug.Log("[batch] 打包后写回 layout tsv");
        }

        if (manifest == null)
        {
            throw new Exception("[batch] BuildAssetBundles 返回 null —— 打包失败");
        }

        Debug.Log("[batch] 打包完成，bundle 数 = " + manifest.GetAllAssetBundles().Length);
        // 动态列出所有 wellcooked 产物（含 4 个展示关）
        foreach (string bundle in manifest.GetAllAssetBundles())
        {
            if (!bundle.Contains("wellcooked")) continue;
            string path = Path.Combine(BundleDir, bundle);
            Debug.Log(string.Format("[batch] 产物 {0}: {1}", bundle,
                File.Exists(path) ? (new FileInfo(path).Length + " 字节") : "缺失"));
        }

        Debug.Log("[batch] ===== 构建结束 =====");
    }

    public static void Run()
    {
        Debug.Log("[batch] ===== 开始无人值守构建（4 关） =====");

        // ---- 生成并保存 4 个关卡场景（Build 内部已逐关保存）----
        BuildWellCookedDemo.Build();

        Scene scene = EditorSceneManager.GetActiveScene();
        Debug.Log(string.Format("[batch] 最后活动场景 = '{0}'  isLoaded={1}  根物体数={2}",
            scene.path, scene.isLoaded, scene.GetRootGameObjects().Length));

        // ---- 场景都已落盘，释放 PseudoPrefabManager 持有的 AssetBundle ----
        // 不 DeInit 的话 BuildPipeline.BuildAssetBundles 会静默返回 null。
        PseudoPrefabManager manager = scene.GetRootGameObjects()
            .Select(root => root.GetComponent<PseudoPrefabManager>())
            .FirstOrDefault(comp => comp != null);

        if (manager != null)
        {
            MethodInfo deInit = typeof(PseudoPrefabManager).GetMethod(
                "DeInit", BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
            if (deInit != null)
            {
                deInit.Invoke(manager, null);
                Debug.Log("[batch] 已 DeInit() 卸载 AssetBundle");
            }
        }

        // ---- 打包前把 layout_1..4.tsv（及 .meta）挪走 ----
        // 非资源文件落在被打成 bundle 的目录里会让 BuildAssetBundles 直接失败：
        //   Unrecognized assets cannot be included in AssetBundles: ".../layout_1.tsv"
        var parked = new Dictionary<string, byte[]>();
        foreach (string path in Directory.GetFiles(SetDir, "layout*.tsv"))
        {
            string normalized = path.Replace('\\', '/');
            parked[normalized] = File.ReadAllBytes(normalized);
            File.Delete(normalized);
            if (File.Exists(normalized + ".meta"))
            {
                File.Delete(normalized + ".meta");
            }
        }
        if (parked.Count > 0)
        {
            AssetDatabase.Refresh();
            Debug.Log("[batch] 打包前删除 " + parked.Count + " 个 layout tsv（含 .meta）");
        }

        if (!Directory.Exists(BundleDir))
        {
            Directory.CreateDirectory(BundleDir);
        }

        AssetBundleManifest manifest = BuildPipeline.BuildAssetBundles(
            BundleDir, BuildAssetBundleOptions.None, BuildTarget.StandaloneWindows);

        foreach (var pair in parked)
        {
            File.WriteAllBytes(pair.Key, pair.Value);
        }
        if (parked.Count > 0)
        {
            AssetDatabase.Refresh();
            Debug.Log("[batch] 打包后写回 layout tsv");
        }

        if (manifest == null)
        {
            throw new Exception("[batch] BuildAssetBundles 返回 null —— 打包失败");
        }

        Debug.Log("[batch] 打包完成，bundle 数 = " + manifest.GetAllAssetBundles().Length);
        foreach (string bundle in manifest.GetAllAssetBundles())
        {
            if (bundle.Contains("wellcooked"))
            {
                Debug.Log("[batch]   bundle: " + bundle);
            }
        }

        // 动态列出所有 wellcooked 产物（含 4 个展示关）
        foreach (string bundle in manifest.GetAllAssetBundles())
        {
            if (!bundle.Contains("wellcooked")) continue;
            string path = Path.Combine(BundleDir, bundle);
            Debug.Log(string.Format("[batch] 产物 {0}: {1}", bundle,
                File.Exists(path) ? (new FileInfo(path).Length + " 字节") : "缺失"));
        }

        Debug.Log("[batch] ===== 构建结束 =====");
    }
}
