// RemesherDesk — Unity Editor mesh dumper (intake tool, read-only).
//
// Install: copy this file into  Assets/RemesherDeskTools/Editor/MeshDump.cs  of the Unity
// project (any folder named "Editor" works). Remove the folder when intake is done.
//
// Use:  select a GameObject with a MeshFilter / SkinnedMeshRenderer (or a Mesh asset in the
//       Project window), then  Tools > RemesherDesk > Dump Selected Mesh(es).
//       Typical golden-data workflow: dump the INPUT object, run Quadremesh, dump the OUTPUT
//       object, choosing a folder per case and side, e.g. qr_dumps/case01_before and
//       qr_dumps/case01_after (the folder name becomes the dump's label).
//
// Output per mesh: <label>__<object>__<mesh>.rdm (binary, see docs/rdm-format.md) and a
// human-readable sidecar <same>.rdm.json (metadata + SHA-256 of the .rdm) so you can review
// what you send. The script never modifies scenes or assets.
//
// What it records: raw Mesh arrays exactly as Unity stores them (left-handed, Y-up, clockwise
// front faces — NO axis or winding conversion), every submesh's topology and indices (base
// vertex applied), index format, the object's localToWorld matrix, and EditorJsonUtility dumps
// of the other components on the object (this captures the remesher's inspector parameters
// if it is a component; nothing is known about the plugin here).
//
// Language level: C# 7.3 (works on Unity 2019.4+). The RdmWriter class has no Unity
// dependency and is unit-tested outside Unity (tools/unity/tests).

using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Security.Cryptography;
using System.Text;

namespace RemesherDesk.Intake
{
    /// <summary>Pure-C# writer for the RDM container (docs/rdm-format.md). Little-endian always
    /// (BinaryWriter is little-endian on every platform).</summary>
    public sealed class RdmWriter
    {
        public const uint TopoTriangles = 1, TopoQuads = 2, TopoLines = 3, TopoLineStrip = 4, TopoPoints = 5, TopoPolygons = 16;

        private struct Chunk { public string Tag; public byte[] Payload; }
        private readonly List<Chunk> _chunks = new List<Chunk>();
        private int _vertexCount = -1;
        private int _submeshCount;

        /// <summary>META must be added first; it is a complete JSON object (UTF-8, no BOM).
        /// The caller is responsible for "vertex_count" and "submesh_count" matching the data.</summary>
        public void AddMeta(string json)
        {
            if (_chunks.Count != 0) throw new InvalidOperationException("META must be the first chunk");
            _chunks.Add(new Chunk { Tag = "META", Payload = new UTF8Encoding(false).GetBytes(json) });
        }

        public void AddFloats(string tag, float[] values, int componentsPerVertex)
        {
            if (tag.Length != 4) throw new ArgumentException("tag must be 4 ASCII chars");
            if (values.Length % componentsPerVertex != 0) throw new ArgumentException("bad length for " + tag);
            int n = values.Length / componentsPerVertex;
            if (tag == "VPOS") _vertexCount = n;
            else if (_vertexCount >= 0 && n != _vertexCount) throw new ArgumentException(tag + " vertex count mismatch");
            using (var ms = new MemoryStream())
            using (var w = new BinaryWriter(ms))
            {
                foreach (float f in values) w.Write(f);
                w.Flush();
                _chunks.Add(new Chunk { Tag = tag, Payload = ms.ToArray() });
            }
        }

        public void AddUv(int channel, int dimension, float[] values)
        {
            if (channel < 0 || channel > 7) throw new ArgumentOutOfRangeException("channel");
            if (dimension < 2 || dimension > 4) throw new ArgumentOutOfRangeException("dimension");
            if (values.Length % dimension != 0) throw new ArgumentException("bad uv length");
            using (var ms = new MemoryStream())
            using (var w = new BinaryWriter(ms))
            {
                w.Write((uint)dimension);
                w.Write(0u);
                foreach (float f in values) w.Write(f);
                w.Flush();
                _chunks.Add(new Chunk { Tag = "UV" + channel.ToString(CultureInfo.InvariantCulture) + " ", Payload = ms.ToArray() });
            }
        }

        /// <summary>indices must already have the submesh base vertex applied.</summary>
        public void AddSubmesh(uint topology, int baseVertex, int[] indices, int[] faceSizes)
        {
            using (var ms = new MemoryStream())
            using (var w = new BinaryWriter(ms))
            {
                int faceCount = faceSizes == null ? 0 : faceSizes.Length;
                w.Write(topology);
                w.Write(baseVertex);
                w.Write((uint)indices.Length);
                w.Write((uint)faceCount);
                foreach (int i in indices)
                {
                    if (i < 0) throw new ArgumentException("negative index");
                    w.Write((uint)i);
                }
                if (faceSizes != null) foreach (int s in faceSizes) w.Write((uint)s);
                w.Flush();
                _chunks.Add(new Chunk { Tag = "SUBM", Payload = ms.ToArray() });
                _submeshCount++;
            }
        }

        public void AddJsonChunk(string tag, string json)
        {
            _chunks.Add(new Chunk { Tag = tag, Payload = new UTF8Encoding(false).GetBytes(json) });
        }

        public int SubmeshCount { get { return _submeshCount; } }

        public byte[] ToBytes()
        {
            if (_chunks.Count == 0 || _chunks[0].Tag != "META") throw new InvalidOperationException("missing META");
            using (var ms = new MemoryStream())
            using (var w = new BinaryWriter(ms))
            {
                w.Write(Encoding.ASCII.GetBytes("RDMF"));
                w.Write((ushort)1);   // major
                w.Write((ushort)0);   // minor
                w.Write(0u);          // flags
                w.Write((uint)_chunks.Count);
                foreach (var c in _chunks)
                {
                    byte[] tag = Encoding.ASCII.GetBytes(c.Tag);
                    if (tag.Length != 4) throw new InvalidOperationException("bad tag " + c.Tag);
                    w.Write(tag);
                    w.Write(0u);                       // reserved
                    w.Write((ulong)c.Payload.Length);
                    w.Write(c.Payload);
                    int pad = (8 - (c.Payload.Length % 8)) % 8;
                    for (int i = 0; i < pad; i++) w.Write((byte)0);
                }
                w.Flush();
                return ms.ToArray();
            }
        }

        // ---- tiny JSON helpers (no dependency on Unity's JsonUtility, which cannot emit dictionaries)
        public static string Str(string s)
        {
            if (s == null) return "null";
            var sb = new StringBuilder(s.Length + 2);
            sb.Append('"');
            foreach (char ch in s)
            {
                switch (ch)
                {
                    case '"': sb.Append("\\\""); break;
                    case '\\': sb.Append("\\\\"); break;
                    case '\n': sb.Append("\\n"); break;
                    case '\r': sb.Append("\\r"); break;
                    case '\t': sb.Append("\\t"); break;
                    default:
                        if (ch < 0x20) sb.Append("\\u").Append(((int)ch).ToString("x4", CultureInfo.InvariantCulture));
                        else sb.Append(ch);
                        break;
                }
            }
            sb.Append('"');
            return sb.ToString();
        }

        /// <summary>Round-trippable float formatting ("R"), invariant culture; non-finite -> null.</summary>
        public static string Num(float f)
        {
            if (float.IsNaN(f) || float.IsInfinity(f)) return "null";
            return f.ToString("R", CultureInfo.InvariantCulture);
        }

        public static string Sha256Hex(byte[] data)
        {
            using (var sha = SHA256.Create())
            {
                byte[] h = sha.ComputeHash(data);
                var sb = new StringBuilder(64);
                foreach (byte b in h) sb.Append(b.ToString("x2", CultureInfo.InvariantCulture));
                return sb.ToString();
            }
        }

        public static string SafeFileName(string s)
        {
            if (string.IsNullOrEmpty(s)) return "unnamed";
            var sb = new StringBuilder();
            foreach (char c in s)
                sb.Append(char.IsLetterOrDigit(c) || c == '-' || c == '_' || c == '.' ? c : '_');
            return sb.ToString();
        }
    }
}

#if UNITY_EDITOR || RDESK_UNITY_STUBS
namespace RemesherDesk.Intake
{
    using UnityEditor;
    using UnityEngine;
    using UnityEngine.Rendering;

    public static class MeshDump
    {
        private const string PrefFolder = "RemesherDesk.MeshDump.Folder";

        [MenuItem("Tools/RemesherDesk/Dump Selected Mesh(es)")]
        public static void DumpSelected()
        {
            var targets = CollectTargets();
            if (targets.Count == 0)
            {
                EditorUtility.DisplayDialog("RemesherDesk MeshDump",
                    "Select a GameObject with a MeshFilter or SkinnedMeshRenderer, or a Mesh asset.", "OK");
                return;
            }
            string folder = EditorUtility.SaveFolderPanel("RemesherDesk: folder for .rdm dumps",
                EditorPrefs.GetString(PrefFolder, ""), "");
            if (string.IsNullOrEmpty(folder)) return;
            EditorPrefs.SetString(PrefFolder, folder);

            // The label is the folder's name: make one folder per case and side,
            // e.g. .../qr_dumps/case01_before and .../qr_dumps/case01_after.
            string label = Path.GetFileName(folder.TrimEnd('/', '\\'));

            int ok = 0;
            var errors = new StringBuilder();
            foreach (var t in targets)
            {
                try
                {
                    string path = DumpOne(t.Mesh, t.Owner, folder, label);
                    Debug.Log("[RemesherDesk] wrote " + path);
                    ok++;
                }
                catch (Exception e)
                {
                    errors.AppendLine((t.Owner != null ? t.Owner.name : t.Mesh.name) + ": " + e.Message);
                    Debug.LogException(e);
                }
            }
            EditorUtility.DisplayDialog("RemesherDesk MeshDump",
                ok + " mesh(es) written to\n" + folder + (errors.Length > 0 ? "\n\nErrors:\n" + errors : ""), "OK");
        }

        [MenuItem("Tools/RemesherDesk/Dump Selected Mesh(es)", true)]
        public static bool DumpSelectedValidate()
        {
            return Selection.activeObject != null;
        }

        private struct Target { public Mesh Mesh; public GameObject Owner; }

        private static List<Target> CollectTargets()
        {
            var list = new List<Target>();
            foreach (var obj in Selection.objects)
            {
                var mesh = obj as Mesh;
                if (mesh != null) { list.Add(new Target { Mesh = mesh, Owner = null }); continue; }
                var go = obj as GameObject;
                if (go == null) continue;
                var mf = go.GetComponent<MeshFilter>();
                if (mf != null && mf.sharedMesh != null) { list.Add(new Target { Mesh = mf.sharedMesh, Owner = go }); continue; }
                var smr = go.GetComponent<SkinnedMeshRenderer>();
                if (smr != null && smr.sharedMesh != null) list.Add(new Target { Mesh = smr.sharedMesh, Owner = go });
            }
            return list;
        }

        public static string DumpOne(Mesh mesh, GameObject owner, string folder, string label)
        {
            if (!mesh.isReadable)
                Debug.LogWarning("[RemesherDesk] Mesh '" + mesh.name + "' is not marked Read/Write. In the Editor its " +
                                 "data is usually still accessible; if the dump fails, enable Read/Write in the model import settings.");

            var w = new RdmWriter();
            var verts = new List<Vector3>();
            mesh.GetVertices(verts);
            int n = verts.Count;
            if (n == 0) throw new Exception("mesh has no vertices (not readable?)");

            var meta = new StringBuilder();
            meta.Append('{');
            meta.Append("\"format\":\"rdm\",\"version\":\"1.0\",\"convention\":\"unity_lh_yup_cw\"");
            meta.Append(",\"producer\":\"tools/unity/MeshDump.cs\"");
            meta.Append(",\"label\":").Append(RdmWriter.Str(label));
            meta.Append(",\"name\":").Append(RdmWriter.Str(mesh.name));
            meta.Append(",\"object_name\":").Append(RdmWriter.Str(owner != null ? owner.name : null));
            meta.Append(",\"asset_path\":").Append(RdmWriter.Str(AssetDatabase.GetAssetPath(mesh)));
            meta.Append(",\"unity_version\":").Append(RdmWriter.Str(Application.unityVersion));
            meta.Append(",\"created_utc\":").Append(RdmWriter.Str(DateTime.UtcNow.ToString("o", CultureInfo.InvariantCulture)));
            meta.Append(",\"is_readable\":").Append(mesh.isReadable ? "true" : "false");
            meta.Append(",\"index_format\":").Append(RdmWriter.Str(mesh.indexFormat == IndexFormat.UInt32 ? "UInt32" : "UInt16"));
            meta.Append(",\"vertex_count\":").Append(n.ToString(CultureInfo.InvariantCulture));
            meta.Append(",\"submesh_count\":").Append(mesh.subMeshCount.ToString(CultureInfo.InvariantCulture));
            Bounds b = mesh.bounds;
            meta.Append(",\"bounds_center\":[").Append(RdmWriter.Num(b.center.x)).Append(',').Append(RdmWriter.Num(b.center.y)).Append(',').Append(RdmWriter.Num(b.center.z)).Append(']');
            meta.Append(",\"bounds_extents\":[").Append(RdmWriter.Num(b.extents.x)).Append(',').Append(RdmWriter.Num(b.extents.y)).Append(',').Append(RdmWriter.Num(b.extents.z)).Append(']');
            if (owner != null)
            {
                Matrix4x4 m = owner.transform.localToWorldMatrix;
                meta.Append(",\"local_to_world_row_major\":[");
                for (int r = 0; r < 4; r++)
                    for (int c = 0; c < 4; c++)
                        meta.Append(r + c == 0 ? "" : ",").Append(RdmWriter.Num(m[r, c]));
                meta.Append(']');
            }
            meta.Append('}');
            w.AddMeta(meta.ToString());

            w.AddFloats("VPOS", Flatten3(verts), 3);

            var normals = new List<Vector3>();
            mesh.GetNormals(normals);
            if (normals.Count == n) w.AddFloats("VNRM", Flatten3(normals), 3);

            var tangents = new List<Vector4>();
            mesh.GetTangents(tangents);
            if (tangents.Count == n) w.AddFloats("VTAN", Flatten4(tangents), 4);

            var colors = new List<Color>();
            mesh.GetColors(colors);
            if (colors.Count == n)
            {
                var cf = new float[n * 4];
                for (int i = 0; i < n; i++) { cf[4 * i] = colors[i].r; cf[4 * i + 1] = colors[i].g; cf[4 * i + 2] = colors[i].b; cf[4 * i + 3] = colors[i].a; }
                w.AddFloats("VCOL", cf, 4);
            }

            var uv = new List<Vector4>();
            for (int ch = 0; ch < 8; ch++)
            {
                int dim = UvDimension(mesh, ch);
                if (dim == 0) continue;
                uv.Clear();
                mesh.GetUVs(ch, uv);
                if (uv.Count != n) continue;
                var f = new float[n * dim];
                for (int i = 0; i < n; i++)
                    for (int k = 0; k < dim; k++) f[i * dim + k] = uv[i][k];
                w.AddUv(ch, dim, f);
            }

            for (int s = 0; s < mesh.subMeshCount; s++)
            {
                uint topo = MapTopology(mesh.GetTopology(s));
                int[] idx = mesh.GetIndices(s, true);   // base vertex applied
                w.AddSubmesh(topo, (int)mesh.GetBaseVertex(s), idx, null);
            }

            if (owner != null) w.AddJsonChunk("COMP", ComponentsJson(owner));

            byte[] bytes = w.ToBytes();
            string baseName = RdmWriter.SafeFileName(label) + "__" + RdmWriter.SafeFileName(owner != null ? owner.name : "asset")
                              + "__" + RdmWriter.SafeFileName(mesh.name);
            string path = Path.Combine(folder, baseName + ".rdm");
            File.WriteAllBytes(path, bytes);
            string sidecar = "{\"rdm_file\":" + RdmWriter.Str(baseName + ".rdm") + ",\"sha256\":\"" + RdmWriter.Sha256Hex(bytes)
                             + "\",\"bytes\":" + bytes.Length.ToString(CultureInfo.InvariantCulture) + ",\"meta\":" + meta + "}\n";
            File.WriteAllText(path + ".json", sidecar, new UTF8Encoding(false));
            return path;
        }

        private static int UvDimension(Mesh mesh, int channel)
        {
#if UNITY_2019_3_OR_NEWER || RDESK_UNITY_STUBS
            var attr = (VertexAttribute)((int)VertexAttribute.TexCoord0 + channel);
            return mesh.HasVertexAttribute(attr) ? mesh.GetVertexAttributeDimension(attr) : 0;
#else
            var tmp = new List<Vector4>();
            mesh.GetUVs(channel, tmp);
            return tmp.Count > 0 ? 4 : 0;
#endif
        }

        private static uint MapTopology(MeshTopology t)
        {
            switch (t)
            {
                case MeshTopology.Triangles: return RdmWriter.TopoTriangles;
                case MeshTopology.Quads: return RdmWriter.TopoQuads;
                case MeshTopology.Lines: return RdmWriter.TopoLines;
                case MeshTopology.LineStrip: return RdmWriter.TopoLineStrip;
                case MeshTopology.Points: return RdmWriter.TopoPoints;
                default: throw new Exception("unknown MeshTopology " + t);
            }
        }

        private static string ComponentsJson(GameObject go)
        {
            var sb = new StringBuilder("[");
            bool first = true;
            foreach (var c in go.GetComponents<Component>())
            {
                if (c == null) continue;  // missing script
                if (c is Transform || c is MeshFilter || c is MeshRenderer || c is SkinnedMeshRenderer) continue;
                string json;
                try { json = EditorJsonUtility.ToJson(c, true); }
                catch (Exception e) { json = "{\"error\":" + RdmWriter.Str(e.Message) + "}"; }
                if (string.IsNullOrEmpty(json)) json = "{}";
                sb.Append(first ? "" : ",").Append("{\"type\":").Append(RdmWriter.Str(c.GetType().FullName))
                  .Append(",\"json\":").Append(json).Append('}');
                first = false;
            }
            return sb.Append(']').ToString();
        }

        private static float[] Flatten3(List<Vector3> v)
        {
            var f = new float[v.Count * 3];
            for (int i = 0; i < v.Count; i++) { f[3 * i] = v[i].x; f[3 * i + 1] = v[i].y; f[3 * i + 2] = v[i].z; }
            return f;
        }

        private static float[] Flatten4(List<Vector4> v)
        {
            var f = new float[v.Count * 4];
            for (int i = 0; i < v.Count; i++) { f[4 * i] = v[i].x; f[4 * i + 1] = v[i].y; f[4 * i + 2] = v[i].z; f[4 * i + 3] = v[i].w; }
            return f;
        }
    }
}
#endif
