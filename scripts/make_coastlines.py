#!/usr/bin/env python3
"""Generate coastlines.bin for ncvista from a Natural Earth coastline shapefile.

The viewer reads a simple little-endian binary format:
    int32  n_polylines
    repeat n_polylines:
        int32 n_points
        repeat n_points: float32 lon, float32 lat   (lon in [-180, 180])

Usage:
    # 1) obtain the shapefile (e.g. Natural Earth 110m physical coastline):
    #    ne_110m_coastline.shp/.shx/.dbf
    # 2) python3 make_coastlines.py ne_110m_coastline.shp coastlines.bin

Uses a shapefile reader if available (cartopy or pyshp); otherwise falls back to
a small built-in parser that needs only the Python standard library.
"""
import struct
import sys


def _read_shp_stdlib(shp_path):
    """Minimal Natural Earth .shp reader: PolyLine (3) / Polygon (5) geometry.

    The ESRI shapefile main file is a 100-byte header followed by records, each
    with an 8-byte (big-endian) header and little-endian content. Only the
    polyline/polygon parts are needed for a coastline; attributes (.dbf) are
    ignored.
    """
    with open(shp_path, "rb") as f:
        data = f.read()
    n = len(data)
    pos = 100  # skip the main header
    lines = []
    while pos + 8 <= n:
        content_len = struct.unpack(">i", data[pos + 4:pos + 8])[0] * 2
        pos += 8
        content = data[pos:pos + content_len]
        pos += content_len
        if len(content) < 44:
            continue
        stype = struct.unpack("<i", content[0:4])[0]
        if stype not in (3, 5):     # 0 = null, others unused here
            continue
        off = 4 + 32                # shape type + bounding box
        num_parts, num_points = struct.unpack("<ii", content[off:off + 8])
        off += 8
        parts = list(struct.unpack("<%di" % num_parts, content[off:off + 4 * num_parts]))
        off += 4 * num_parts
        coords = struct.unpack("<%dd" % (2 * num_points), content[off:off + 16 * num_points])
        pts = [(coords[2 * i], coords[2 * i + 1]) for i in range(num_points)]
        bounds = parts + [num_points]
        for i in range(len(bounds) - 1):
            seg = pts[bounds[i]:bounds[i + 1]]
            if len(seg) >= 2:
                lines.append(seg)
    return lines


def read_lines(shp_path):
    try:
        from cartopy.io.shapereader import Reader
    except Exception:
        Reader = None
    if Reader is not None:
        lines = []
        for geom in Reader(shp_path).geometries():
            if geom.geom_type == "LineString":
                lines.append(list(geom.coords))
            elif geom.geom_type == "MultiLineString":
                lines.extend(list(ls.coords) for ls in geom.geoms)
        return lines

    try:
        import shapefile  # pyshp
    except Exception:
        return _read_shp_stdlib(shp_path)
    sf = shapefile.Reader(shp_path)
    lines = []
    for shape in sf.shapes():
        pts = shape.points
        parts = list(shape.parts) + [len(pts)]
        for i in range(len(parts) - 1):
            lines.append(pts[parts[i]:parts[i + 1]])
    return lines


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    shp, out = sys.argv[1], sys.argv[2]
    lines = [[(float(x), float(y)) for x, y in ln] for ln in read_lines(shp)]
    lines = [ln for ln in lines if len(ln) >= 2]
    npts = sum(len(ln) for ln in lines)
    print(f"polylines: {len(lines)}  points: {npts}")
    with open(out, "wb") as f:
        f.write(struct.pack("<i", len(lines)))
        for ln in lines:
            f.write(struct.pack("<i", len(ln)))
            for x, y in ln:
                f.write(struct.pack("<ff", x, y))
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
