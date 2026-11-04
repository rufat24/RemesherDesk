// Hand-written stubs of the small Unity API subset used by MeshDump.cs, for compile checks
// outside Unity only. Signatures follow the public Unity scripting reference; bodies are dummies.
#pragma warning disable CS1591
using System;
using System.Collections.Generic;

namespace UnityEngine.Rendering
{
    public enum IndexFormat { UInt16 = 0, UInt32 = 1 }
    public enum VertexAttribute { Position = 0, Normal, Tangent, Color, TexCoord0, TexCoord1, TexCoord2, TexCoord3,
                                  TexCoord4, TexCoord5, TexCoord6, TexCoord7, BlendWeight, BlendIndices }
}

namespace UnityEngine
{
    public enum MeshTopology { Triangles = 0, Quads = 2, Lines = 3, LineStrip = 4, Points = 5 }
    public struct Vector2 { public float x, y; public Vector2(float x, float y) { this.x = x; this.y = y; } }
    public struct Vector3 { public float x, y, z; }
    public struct Vector4 { public float x, y, z, w; public float this[int i] { get { return i == 0 ? x : i == 1 ? y : i == 2 ? z : w; } } }
    public struct Color { public float r, g, b, a; }
    public struct Bounds { public Vector3 center; public Vector3 extents; }
    public struct Matrix4x4 { public float this[int row, int column] { get { return 0f; } } }
    public class Object { public string name; }
    public class Component : Object { public Transform transform; public GameObject gameObject; }
    public class Transform : Component { public Matrix4x4 localToWorldMatrix; }
    public class MeshFilter : Component { public Mesh sharedMesh; }
    public class MeshRenderer : Component { }
    public class SkinnedMeshRenderer : Component { public Mesh sharedMesh; }
    public class GameObject : Object
    {
        public Transform transform;
        public T GetComponent<T>() where T : Component { return null; }
        public Component[] GetComponents<T>() where T : Component { return new Component[0]; }
    }
    public class Mesh : Object
    {
        public bool isReadable;
        public Rendering.IndexFormat indexFormat;
        public int subMeshCount;
        public Bounds bounds;
        public void GetVertices(List<Vector3> v) { }
        public void GetNormals(List<Vector3> v) { }
        public void GetTangents(List<Vector4> v) { }
        public void GetColors(List<Color> v) { }
        public void GetUVs(int channel, List<Vector4> uvs) { }
        public bool HasVertexAttribute(Rendering.VertexAttribute a) { return false; }
        public int GetVertexAttributeDimension(Rendering.VertexAttribute a) { return 0; }
        public MeshTopology GetTopology(int submesh) { return MeshTopology.Triangles; }
        public int[] GetIndices(int submesh, bool applyBaseVertex) { return new int[0]; }
        public uint GetBaseVertex(int submesh) { return 0; }
    }
    public static class Debug
    {
        public static void Log(object m) { }
        public static void LogWarning(object m) { }
        public static void LogException(Exception e) { }
    }
    public static class Application { public static string unityVersion = "stub"; }
}

namespace UnityEditor
{
    [AttributeUsage(AttributeTargets.Method, AllowMultiple = true)]
    public sealed class MenuItem : Attribute
    {
        public MenuItem(string itemName) { }
        public MenuItem(string itemName, bool isValidateFunction) { }
    }
    public static class Selection
    {
        public static UnityEngine.Object activeObject;
        public static UnityEngine.Object[] objects = new UnityEngine.Object[0];
    }
    public static class EditorUtility
    {
        public static bool DisplayDialog(string title, string message, string ok) { return true; }
        public static string SaveFolderPanel(string title, string folder, string defaultName) { return ""; }
    }
    public static class EditorPrefs
    {
        public static string GetString(string key, string defaultValue) { return defaultValue; }
        public static void SetString(string key, string value) { }
    }
    public static class AssetDatabase { public static string GetAssetPath(UnityEngine.Object o) { return ""; } }
    public static class EditorJsonUtility { public static string ToJson(object o, bool prettyPrint) { return "{}"; } }
}
