// Writes the canonical Unity-convention cube (same data as tools/rdm/rdm.py unity_cube())
// through RdmWriter, so the Python reference reader can verify C#/spec agreement:
//   dotnet run --project tools/unity/tests/RdmWriterTest -- out.rdm
//   python3 -I tools/rdm/rdm.py check-fixture out.rdm
using System;
using System.IO;
using RemesherDesk.Intake;

internal static class Program
{
    private static int Main(string[] args)
    {
        if (args.Length != 1) { Console.Error.WriteLine("usage: RdmWriterTest OUT.rdm"); return 2; }
        float[] p = { -.5f, -.5f, -.5f, .5f, -.5f, -.5f, .5f, .5f, -.5f, -.5f, .5f, -.5f,
                      -.5f, -.5f, .5f, .5f, -.5f, .5f, .5f, .5f, .5f, -.5f, .5f, .5f };
        int[] quads = { 0, 3, 2, 1, 4, 5, 6, 7, 0, 1, 5, 4, 3, 7, 6, 2, 0, 4, 7, 3, 1, 2, 6, 5 };
        float[] uv = new float[16];
        for (int i = 0; i < 8; i++) { uv[2 * i] = 0f; uv[2 * i + 1] = 1f; }
        var w = new RdmWriter();
        w.AddMeta("{\"format\":\"rdm\",\"version\":\"1.0\",\"convention\":\"unity_lh_yup_cw\",\"name\":" +
                  RdmWriter.Str("cube \"quoted\"\n") + ",\"producer\":\"RdmWriterTest\",\"index_format\":\"UInt16\"," +
                  "\"vertex_count\":8,\"submesh_count\":1,\"pi\":" + RdmWriter.Num(3.14159274f) + "}");
        w.AddFloats("VPOS", p, 3);
        w.AddUv(0, 2, uv);
        w.AddSubmesh(RdmWriter.TopoQuads, 0, quads, null);
        w.AddJsonChunk("COMP", "[{\"type\":\"Example\",\"json\":{\"a\":1}}]");
        File.WriteAllBytes(args[0], w.ToBytes());
        Console.WriteLine("wrote " + args[0] + " sha256=" + RdmWriter.Sha256Hex(File.ReadAllBytes(args[0])));
        return 0;
    }
}
