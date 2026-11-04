#!/usr/bin/env python3
"""Reference reader/writer for the RDM mesh container (docs/rdm-format.md).

Stdlib only. This file is the executable form of the spec: if this file and
docs/rdm-format.md disagree, fix one of them in the same commit.

Usage:
  python3 -I tools/rdm/rdm.py info   FILE.rdm          # print header/meta/counts, validate
  python3 -I tools/rdm/rdm.py to-obj FILE.rdm OUT.obj  # convert to right-handed OBJ
  python3 -I tools/rdm/rdm.py selftest                 # write/read/validate synthetic meshes
  python3 -I tools/rdm/rdm.py write-fixture OUT.rdm    # write the canonical test cube (Unity convention)
  python3 -I tools/rdm/rdm.py check-fixture IN.rdm     # verify another writer produced that cube
"""
from __future__ import annotations

import json
import struct
import sys
import tempfile
import os

MAGIC = b"RDMF"
MAJOR, MINOR = 1, 0
HEADER = struct.Struct("<4sHHII")      # magic, major, minor, flags, chunk_count  (16 bytes)
CHUNK = struct.Struct("<4sIQ")         # tag, reserved(0), payload_len            (16 bytes)
ALIGN = 8

CONV_UNITY = "unity_lh_yup_cw"
CONV_RH = "rh_yup_ccw"

TOPO_TRIANGLES, TOPO_QUADS, TOPO_LINES, TOPO_LINESTRIP, TOPO_POINTS, TOPO_POLYGONS = 1, 2, 3, 4, 5, 16
TOPO_NAMES = {1: "triangles", 2: "quads", 3: "lines", 4: "linestrip", 5: "points", 16: "polygons"}
SUBM_HDR = struct.Struct("<IiII")      # topology, base_vertex, index_count, face_count (16 bytes)

VERTEX_CHUNKS = {b"VPOS": 3, b"VNRM": 3, b"VTAN": 4, b"VCOL": 4}
UV_TAGS = [("UV%d" % i).encode().ljust(4) for i in range(8)]  # b"UV0 " .. b"UV7 "


class RdmError(Exception):
    pass


# ---------------------------------------------------------------- reading
def _floats(buf: bytes, n: int) -> list:
    if len(buf) != 4 * n:
        raise RdmError("float payload size %d != 4*%d" % (len(buf), n))
    return list(struct.unpack("<%df" % n, buf))


def read(path: str) -> dict:
    with open(path, "rb") as f:
        data = f.read()
    return parse(data)


def parse(data: bytes) -> dict:
    if len(data) < HEADER.size:
        raise RdmError("file too short for header")
    magic, major, minor, flags, nchunks = HEADER.unpack_from(data, 0)
    if magic != MAGIC:
        raise RdmError("bad magic %r" % magic)
    if major != MAJOR:
        raise RdmError("unsupported major version %d" % major)
    off = HEADER.size
    mesh = {"version": (major, minor), "flags": flags, "submeshes": [], "uvs": {}, "unknown_chunks": []}
    meta = None
    for ci in range(nchunks):
        if off + CHUNK.size > len(data):
            raise RdmError("chunk %d header past end of file" % ci)
        tag, reserved, plen = CHUNK.unpack_from(data, off)
        off += CHUNK.size
        if plen > len(data) - off:
            raise RdmError("chunk %r length %d past end of file" % (tag, plen))
        payload = data[off:off + plen]
        off += plen
        off += (-plen) % ALIGN
        if ci == 0 and tag != b"META":
            raise RdmError("first chunk must be META, got %r" % tag)
        if tag == b"META":
            if meta is not None:
                raise RdmError("duplicate META")
            meta = json.loads(payload.decode("utf-8"))
            if not isinstance(meta, dict):
                raise RdmError("META is not a JSON object")
            mesh["meta"] = meta
        elif tag in VERTEX_CHUNKS:
            n = int(meta["vertex_count"]) * VERTEX_CHUNKS[tag]
            mesh[tag.decode()] = _floats(payload, n)
        elif tag in UV_TAGS:
            if len(payload) < 8:
                raise RdmError("UV chunk too short")
            dim, _zero = struct.unpack_from("<II", payload, 0)
            if dim not in (2, 3, 4):
                raise RdmError("UV dimension %d invalid" % dim)
            mesh["uvs"][UV_TAGS.index(tag)] = (dim, _floats(payload[8:], int(meta["vertex_count"]) * dim))
        elif tag == b"SUBM":
            if len(payload) < SUBM_HDR.size:
                raise RdmError("SUBM too short")
            topo, base, icount, fcount = SUBM_HDR.unpack_from(payload, 0)
            need = SUBM_HDR.size + 4 * icount + (4 * fcount if topo == TOPO_POLYGONS else 0)
            if len(payload) != need:
                raise RdmError("SUBM size %d != expected %d" % (len(payload), need))
            idx = list(struct.unpack_from("<%dI" % icount, payload, SUBM_HDR.size))
            sizes = None
            if topo == TOPO_POLYGONS:
                sizes = list(struct.unpack_from("<%dI" % fcount, payload, SUBM_HDR.size + 4 * icount))
            mesh["submeshes"].append({"topology": topo, "base_vertex": base, "indices": idx, "face_sizes": sizes})
        elif tag == b"COMP":
            mesh["components"] = json.loads(payload.decode("utf-8"))
        else:
            mesh["unknown_chunks"].append(tag)
    if meta is None:
        raise RdmError("no META chunk")
    if "VPOS" not in mesh:
        raise RdmError("no VPOS chunk")
    validate(mesh)
    return mesh


def validate(mesh: dict) -> None:
    meta = mesh["meta"]
    for k in ("format", "version", "convention", "vertex_count", "submesh_count"):
        if k not in meta:
            raise RdmError("META missing key %r" % k)
    if meta["format"] != "rdm":
        raise RdmError("META.format != 'rdm'")
    if meta["convention"] not in (CONV_UNITY, CONV_RH):
        raise RdmError("unknown convention %r" % meta["convention"])
    nv = int(meta["vertex_count"])
    if len(mesh["submeshes"]) != int(meta["submesh_count"]):
        raise RdmError("submesh_count %s != SUBM chunks %d" % (meta["submesh_count"], len(mesh["submeshes"])))
    for si, sm in enumerate(mesh["submeshes"]):
        idx, topo = sm["indices"], sm["topology"]
        if topo not in TOPO_NAMES:
            raise RdmError("submesh %d: unknown topology %d" % (si, topo))
        if any(i >= nv for i in idx):
            raise RdmError("submesh %d: index out of range (vertex_count=%d)" % (si, nv))
        per = {TOPO_TRIANGLES: 3, TOPO_QUADS: 4, TOPO_LINES: 2}.get(topo)
        if per and len(idx) % per:
            raise RdmError("submesh %d: %d indices not divisible by %d" % (si, len(idx), per))
        if topo == TOPO_POLYGONS:
            if any(s < 3 for s in sm["face_sizes"]) or sum(sm["face_sizes"]) != len(idx):
                raise RdmError("submesh %d: invalid face_sizes" % si)
    for v in mesh["VPOS"]:
        if v != v or v in (float("inf"), float("-inf")):
            raise RdmError("non-finite vertex position")


# ---------------------------------------------------------------- writing
def _chunk(tag: bytes, payload: bytes) -> bytes:
    assert len(tag) == 4
    return CHUNK.pack(tag, 0, len(payload)) + payload + b"\0" * ((-len(payload)) % ALIGN)


def serialize(mesh: dict) -> bytes:
    meta = dict(mesh["meta"])
    meta["vertex_count"] = len(mesh["VPOS"]) // 3
    meta["submesh_count"] = len(mesh["submeshes"])
    chunks = [_chunk(b"META", json.dumps(meta, sort_keys=True).encode("utf-8"))]
    for tag in (b"VPOS", b"VNRM", b"VTAN", b"VCOL"):
        if tag.decode() in mesh:
            vals = mesh[tag.decode()]
            chunks.append(_chunk(tag, struct.pack("<%df" % len(vals), *vals)))
    for ch, (dim, vals) in sorted(mesh.get("uvs", {}).items()):
        chunks.append(_chunk(UV_TAGS[ch], struct.pack("<II", dim, 0) + struct.pack("<%df" % len(vals), *vals)))
    for sm in mesh["submeshes"]:
        idx = sm["indices"]
        sizes = sm.get("face_sizes") or []
        p = SUBM_HDR.pack(sm["topology"], sm.get("base_vertex", 0), len(idx), len(sizes))
        p += struct.pack("<%dI" % len(idx), *idx) + struct.pack("<%dI" % len(sizes), *sizes)
        chunks.append(_chunk(b"SUBM", p))
    if "components" in mesh:
        chunks.append(_chunk(b"COMP", json.dumps(mesh["components"]).encode("utf-8")))
    return HEADER.pack(MAGIC, MAJOR, MINOR, 0, len(chunks)) + b"".join(chunks)


def write(path: str, mesh: dict) -> None:
    with open(path, "wb") as f:
        f.write(serialize(mesh))


# ---------------------------------------------------------------- geometry helpers
def faces(mesh: dict):
    """Yield faces (lists of vertex indices) of all polygonal submeshes, in file order."""
    for sm in mesh["submeshes"]:
        idx, topo = sm["indices"], sm["topology"]
        if topo in (TOPO_TRIANGLES, TOPO_QUADS):
            k = 3 if topo == TOPO_TRIANGLES else 4
            for i in range(0, len(idx), k):
                yield idx[i:i + k]
        elif topo == TOPO_POLYGONS:
            o = 0
            for s in sm["face_sizes"]:
                yield idx[o:o + s]
                o += s


def reverse_face(f):
    """Reverse winding while keeping the first vertex first: [a,b,c,d] -> [a,d,c,b]."""
    return [f[0]] + list(reversed(f[1:]))


def to_rh(mesh: dict) -> dict:
    """Unity (left-handed, Y-up, clockwise front) -> right-handed, Y-up, CCW front.
    Negate X of positions/normals/tangents (tangent.w also flips) and reverse winding.
    Both steps are exact in IEEE float (negation and reordering only)."""
    if mesh["meta"]["convention"] == CONV_RH:
        return mesh
    out = {k: v for k, v in mesh.items()}
    out["meta"] = dict(mesh["meta"], convention=CONV_RH)
    p = list(mesh["VPOS"])
    p[0::3] = [-x for x in p[0::3]]
    out["VPOS"] = p
    if "VNRM" in mesh:
        n = list(mesh["VNRM"]); n[0::3] = [-x for x in n[0::3]]; out["VNRM"] = n
    if "VTAN" in mesh:
        t = list(mesh["VTAN"]); t[0::4] = [-x for x in t[0::4]]; t[3::4] = [-w for w in t[3::4]]; out["VTAN"] = t
    subs = []
    for sm in mesh["submeshes"]:
        sm2 = dict(sm)
        topo = sm["topology"]
        if topo in (TOPO_TRIANGLES, TOPO_QUADS, TOPO_POLYGONS):
            sizes = sm["face_sizes"] if topo == TOPO_POLYGONS else None
            k = {TOPO_TRIANGLES: 3, TOPO_QUADS: 4}.get(topo)
            fl, o, new = sizes or [k] * (len(sm["indices"]) // k), 0, []
            for s in fl:
                new += reverse_face(sm["indices"][o:o + s])
                o += s
            sm2["indices"] = new
        subs.append(sm2)
    out["submeshes"] = subs
    return out


def signed_volume(mesh: dict) -> float:
    """Signed volume via fan triangulation; > 0 for a closed mesh whose numeric
    cross(b-a, c-a) points outward. Note: that holds for BOTH raw Unity data and
    correctly converted RH data; it fails if only one of {negate X, reverse winding}
    is applied. Use it to catch half-conversions, not to detect the convention."""
    P = mesh["VPOS"]
    vol = 0.0
    for f in faces(mesh):
        a = P[3 * f[0]:3 * f[0] + 3]
        for i in range(1, len(f) - 1):
            b = P[3 * f[i]:3 * f[i] + 3]
            c = P[3 * f[i + 1]:3 * f[i + 1] + 3]
            vol += (a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0])
                    + a[2] * (b[0] * c[1] - b[1] * c[0])) / 6.0
    return vol


def write_obj(mesh: dict, path: str) -> None:
    m = to_rh(mesh)
    P = m["VPOS"]
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("# converted by tools/rdm/rdm.py from %s convention\n" % mesh["meta"]["convention"])
        for i in range(0, len(P), 3):
            f.write("v %.9g %.9g %.9g\n" % (P[i], P[i + 1], P[i + 2]))
        for fc in faces(m):
            f.write("f " + " ".join(str(i + 1) for i in fc) + "\n")


# ---------------------------------------------------------------- fixtures / selftest
def unity_cube(topology=TOPO_QUADS) -> dict:
    """Unit cube centred at origin, 8 shared vertices, in Unity convention:
    front faces are clockwise when viewed from outside (numeric cross points outward)."""
    P = [-.5, -.5, -.5, .5, -.5, -.5, .5, .5, -.5, -.5, .5, -.5,
         -.5, -.5, .5, .5, -.5, .5, .5, .5, .5, -.5, .5, .5]
    # Each quad listed so cross(v1-v0, v2-v0) points outward (Unity CW-front).
    Q = [[0, 3, 2, 1], [4, 5, 6, 7], [0, 1, 5, 4], [3, 7, 6, 2], [0, 4, 7, 3], [1, 2, 6, 5]]
    if topology == TOPO_QUADS:
        sub = {"topology": TOPO_QUADS, "base_vertex": 0, "indices": [i for q in Q for i in q], "face_sizes": None}
    else:
        tris = [i for q in Q for i in (q[0], q[1], q[2], q[0], q[2], q[3])]
        sub = {"topology": TOPO_TRIANGLES, "base_vertex": 0, "indices": tris, "face_sizes": None}
    return {"meta": {"format": "rdm", "version": "1.0", "convention": CONV_UNITY, "name": "cube",
                     "producer": "tools/rdm/rdm.py", "index_format": "UInt16"},
            "VPOS": P, "submeshes": [sub], "uvs": {0: (2, [0.0, 1.0] * 8)}}


def selftest() -> int:
    fails = []

    def check(cond, msg):
        if not cond:
            fails.append(msg)

    with tempfile.TemporaryDirectory() as d:
        for topo in (TOPO_QUADS, TOPO_TRIANGLES):
            m = unity_cube(topo)
            p = os.path.join(d, "c.rdm")
            write(p, m)
            r = read(p)
            check(r["VPOS"] == m["VPOS"], "positions round-trip")
            check(r["submeshes"][0]["indices"] == m["submeshes"][0]["indices"], "indices round-trip")
            check(r["uvs"][0] == m["uvs"][0], "uv round-trip")
            check(len(open(p, "rb").read()) % ALIGN == 0, "file size 8-aligned")
            v_unity = signed_volume(r)
            rh = to_rh(r)
            check(abs(v_unity - 1.0) < 1e-6, "unity cube volume %r" % v_unity)
            check(abs(signed_volume(rh) - 1.0) < 1e-6, "rh cube volume")
            # half-conversion must be detected
            half = dict(rh, submeshes=r["submeshes"])
            check(signed_volume(half) < 0, "negate-X-only must give negative volume")
            # exactness: converting twice (by symmetry) restores the original bits
            back = to_rh(dict(rh, meta=dict(rh["meta"], convention=CONV_UNITY)))
            check(back["VPOS"] == r["VPOS"] and back["submeshes"][0]["indices"] == r["submeshes"][0]["indices"],
                  "double conversion is identity")
            check(rh["submeshes"][0]["indices"][0] == r["submeshes"][0]["indices"][0], "first vertex kept")
        # polygon submesh + unknown chunk skipping + validation errors
        m = unity_cube()
        m["submeshes"] = [{"topology": TOPO_POLYGONS, "base_vertex": 0, "indices": [0, 1, 2, 3, 4], "face_sizes": [5]}]
        data = serialize(m)
        extra = _chunk(b"ZZZZ", b"abc")
        hdr = HEADER.pack(MAGIC, MAJOR, MINOR, 0, HEADER.unpack_from(data, 0)[4] + 1)
        r = parse(hdr + data[HEADER.size:] + extra)
        check(r["unknown_chunks"] == [b"ZZZZ"], "unknown chunk skipped")
        check(r["submeshes"][0]["face_sizes"] == [5], "polygon face sizes")
        bad = unity_cube()
        bad["submeshes"][0]["indices"][0] = 99
        try:
            parse(serialize(bad))
            check(False, "out-of-range index must fail")
        except RdmError:
            pass
        for cut in (3, 20, 40):
            try:
                parse(serialize(unity_cube())[:cut])
                check(False, "truncated file (%d) must fail" % cut)
            except (RdmError, struct.error, ValueError, KeyError):
                pass
    for f in fails:
        print("FAIL:", f)
    print("rdm.py selftest: %s" % ("ok" if not fails else "%d failure(s)" % len(fails)))
    return 1 if fails else 0


def check_fixture(path: str) -> int:
    """Verify a file written by another implementation (e.g. tools/unity/tests/RdmWriterTest)
    contains exactly the canonical cube of unity_cube()."""
    r, ref = read(path), unity_cube()
    errs = []
    if r["VPOS"] != ref["VPOS"]:
        errs.append("VPOS differs")
    if r["submeshes"][0]["indices"] != ref["submeshes"][0]["indices"]:
        errs.append("indices differ")
    if r["submeshes"][0]["topology"] != TOPO_QUADS:
        errs.append("topology")
    if r["uvs"].get(0) != ref["uvs"][0]:
        errs.append("UV0 differs")
    if r["meta"]["convention"] != CONV_UNITY:
        errs.append("convention")
    if abs(signed_volume(r) - 1.0) > 1e-6:
        errs.append("volume")
    if len(open(path, "rb").read()) % ALIGN:
        errs.append("size not 8-aligned")
    print("check-fixture:", "ok" if not errs else ", ".join(errs))
    return 1 if errs else 0


def info(path: str) -> int:
    m = read(path)
    meta = m["meta"]
    print("version   %d.%d" % m["version"])
    print("convention", meta["convention"])
    print("vertices  ", meta["vertex_count"])
    for k in ("VNRM", "VTAN", "VCOL"):
        print("%-10s %s" % (k, "yes" if k in m else "no"))
    print("uv channels", sorted(m["uvs"]))
    for i, sm in enumerate(m["submeshes"]):
        nf = sum(1 for _ in faces({"submeshes": [sm]}))
        print("submesh %d: %s, %d indices, %d faces, base_vertex %d"
              % (i, TOPO_NAMES[sm["topology"]], len(sm["indices"]), nf, sm["base_vertex"]))
    print("signed volume (closed meshes only) %.6g" % signed_volume(m))
    if m["unknown_chunks"]:
        print("unknown chunks", m["unknown_chunks"])
    print("meta", json.dumps(meta, indent=1, sort_keys=True))
    return 0


def main(argv) -> int:
    if len(argv) >= 2 and argv[1] == "selftest":
        return selftest()
    if len(argv) == 3 and argv[1] == "info":
        return info(argv[2])
    if len(argv) == 4 and argv[1] == "to-obj":
        write_obj(read(argv[2]), argv[3])
        return 0
    if len(argv) == 3 and argv[1] == "check-fixture":
        return check_fixture(argv[2])
    if len(argv) == 3 and argv[1] == "write-fixture":
        write(argv[2], unity_cube())
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
