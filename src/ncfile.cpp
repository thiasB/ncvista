#include "ncfile.hpp"
#include <netcdf.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <set>
#include <string>

NcFile::~NcFile() { close(); }

void NcFile::close() {
    if (isopen_ && ncid_ >= 0) nc_close(ncid_);
    ncid_ = -1;
    isopen_ = false;
    dims_.clear();
    vars_.clear();
    displayable_.clear();
}

std::string NcFile::read_text_att(int varid, const char *name) const {
    int type;
    size_t len = 0;
    if (nc_inq_att(ncid_, varid, name, &type, &len) != NC_NOERR) return {};
    if (type != NC_CHAR || len == 0) return {};
    std::string s(len, '\0');
    if (nc_get_att_text(ncid_, varid, name, &s[0]) != NC_NOERR) return {};
    // Trim trailing NULs.
    size_t z = s.find('\0');
    if (z != std::string::npos) s.resize(z);
    return s;
}

bool NcFile::open(const std::string &path, std::string &err) {
    close();
    int rc = nc_open(path.c_str(), NC_NOWRITE, &ncid_);
    if (rc != NC_NOERR) {
        err = std::string("nc_open: ") + nc_strerror(rc);
        ncid_ = -1;
        return false;
    }
    isopen_ = true;
    path_ = path;

    int ndims = 0, nvars = 0, ngatts = 0, unlimdim = -1;
    nc_inq(ncid_, &ndims, &nvars, &ngatts, &unlimdim);

    dims_.resize(ndims);
    for (int d = 0; d < ndims; ++d) {
        char nm[NC_MAX_NAME + 1] = {0};
        size_t len = 0;
        nc_inq_dim(ncid_, d, nm, &len);
        dims_[d] = NcDim{d, nm, len};
    }

    vars_.reserve(nvars);
    for (int v = 0; v < nvars; ++v) {
        char nm[NC_MAX_NAME + 1] = {0};
        nc_type xtype;
        int vndims = 0, vnatts = 0;
        int dimids[NC_MAX_VAR_DIMS];
        nc_inq_var(ncid_, v, nm, &xtype, &vndims, dimids, &vnatts);

        NcVar var;
        var.id = v;
        var.name = nm;
        var.ndims = vndims;
        var.dimids.assign(dimids, dimids + vndims);
        var.units = read_text_att(v, "units");
        var.long_name = read_text_att(v, "long_name");
        if (var.long_name.empty()) var.long_name = read_text_att(v, "standard_name");

        switch (xtype) {
            case NC_BYTE: case NC_UBYTE: case NC_SHORT: case NC_USHORT:
            case NC_INT: case NC_UINT: case NC_INT64: case NC_UINT64:
            case NC_FLOAT: case NC_DOUBLE:
                var.numeric = true; break;
            default:
                var.numeric = false; break;
        }

        double fv;
        if (nc_get_att_double(ncid_, v, "_FillValue", &fv) == NC_NOERR) {
            var.has_fill = true; var.fill = fv;
        } else if (nc_get_att_double(ncid_, v, "missing_value", &fv) == NC_NOERR) {
            var.has_fill = true; var.fill = fv;
        }

        // CF packing attributes. The netCDF C library does not apply these, so
        // we unpack manually on read: physical = stored * scale_factor + add_offset.
        double sf, ao;
        if (nc_get_att_double(ncid_, v, "scale_factor", &sf) == NC_NOERR) {
            var.scale = sf; var.packed = true;
        }
        if (nc_get_att_double(ncid_, v, "add_offset", &ao) == NC_NOERR) {
            var.offset = ao; var.packed = true;
        }

        vars_.push_back(std::move(var));
    }

    // Identify "supporting" variables: cell bounds, auxiliary coordinates,
    // grid mappings, etc. — anything that is not the science field itself.
    std::set<std::string> referenced;
    auto add_tokens = [&](const std::string &s, bool drop_keys) {
        std::string tok;
        for (size_t i = 0; i <= s.size(); ++i) {
            char c = (i < s.size()) ? s[i] : ' ';
            if (c == ' ' || c == '\t' || c == ',' || c == '\n') {
                if (!tok.empty()) {
                    if (!(drop_keys && tok.back() == ':')) referenced.insert(tok);
                    tok.clear();
                }
            } else {
                tok.push_back(c);
            }
        }
    };
    for (const auto &var : vars_) {
        add_tokens(read_text_att(var.id, "bounds"), false);
        add_tokens(read_text_att(var.id, "coordinates"), false);
        add_tokens(read_text_att(var.id, "grid_mapping"), false);
        add_tokens(read_text_att(var.id, "ancillary_variables"), false);
        add_tokens(read_text_att(var.id, "cell_measures"), true); // "area: areacella"
    }

    auto ends_with = [](const std::string &s, const char *suf) {
        size_t n = std::strlen(suf);
        return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
    };
    static const char *bounds_dims[] = {"bnds", "bounds", "nv", "nbnds",
                                        "nvertices", "vertices"};
    for (auto &var : vars_) {
        bool aux = referenced.count(var.name) > 0;
        if (ends_with(var.name, "_bnds") || ends_with(var.name, "_bounds"))
            aux = true;
        for (int d : var.dimids) {
            std::string dn = dim(d).name;
            for (const char *b : bounds_dims)
                if (dn == b) { aux = true; break; }
        }
        // A variable named exactly like a dimension is a coordinate variable
        // (e.g. depth, lev, time, lat) — supporting, not a science field, so a
        // field that uses it (e.g. soilmoisture over depth) is listed first.
        for (const auto &d : dims_)
            if (d.name == var.name) { aux = true; break; }
        var.aux = aux;
    }

    // Displayable list: real data variables first, supporting ones last
    // (each group keeps file order — a stable two-pass build). 1-D numeric
    // variables are included too and shown as a line plot.
    for (size_t i = 0; i < vars_.size(); ++i)
        if (vars_[i].numeric && vars_[i].ndims >= 1 && !vars_[i].aux)
            displayable_.push_back((int)i);
    for (size_t i = 0; i < vars_.size(); ++i)
        if (vars_[i].numeric && vars_[i].ndims >= 1 && vars_[i].aux)
            displayable_.push_back((int)i);

    return true;
}

const NcDim &NcFile::dim(int dimid) const {
    static NcDim none;
    for (const auto &d : dims_) if (d.id == dimid) return d;
    return none;
}

int NcFile::coord_varid(int dimid) const {
    const NcDim &d = dim(dimid);
    if (d.name.empty()) return -1;
    int vid = -1;
    if (nc_inq_varid(ncid_, d.name.c_str(), &vid) == NC_NOERR) return vid;
    return -1;
}

std::vector<double> NcFile::coord_values(int dimid) const {
    int vid = coord_varid(dimid);
    if (vid < 0) return {};
    const NcDim &d = dim(dimid);
    int vndims = 0;
    nc_inq_varndims(ncid_, vid, &vndims);
    if (vndims != 1) return {};
    std::vector<double> out(d.len);
    if (nc_get_var_double(ncid_, vid, out.data()) != NC_NOERR) return {};
    return out;
}

std::string NcFile::coord_units(int dimid) const {
    int vid = coord_varid(dimid);
    if (vid < 0) return {};
    return read_text_att(vid, "units");
}

std::string NcFile::coord_calendar(int dimid) const {
    int vid = coord_varid(dimid);
    if (vid < 0) return {};
    return read_text_att(vid, "calendar");
}

std::string NcFile::coord_attr(int dimid, const char *name) const {
    int vid = coord_varid(dimid);
    if (vid < 0) return {};
    return read_text_att(vid, name);
}

// Replace embedded control whitespace (newlines, carriage returns, tabs) with
// single spaces so multi-line attribute values such as `history` render as one
// flowing line: the metadata view lays out one line per entry and soft-wraps to
// the window, so a literal newline would otherwise overlap the following row.
static std::string flatten_ws(std::string s) {
    for (char &c : s)
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    return s;
}

// Format the value of one attribute (any numeric or text type) as a string.
static std::string format_att_value(int ncid, int varid, const char *name) {
    nc_type type;
    size_t len = 0;
    if (nc_inq_att(ncid, varid, name, &type, &len) != NC_NOERR) return {};

    if (type == NC_CHAR) {
        std::string s(len, '\0');
        if (nc_get_att_text(ncid, varid, name, &s[0]) != NC_NOERR) return {};
        size_t z = s.find('\0');
        if (z != std::string::npos) s.resize(z);
        return "\"" + flatten_ws(s) + "\"";
    }
    if (type == NC_STRING) {
        std::vector<char *> strs(len, nullptr);
        if (nc_get_att_string(ncid, varid, name, strs.data()) != NC_NOERR) return {};
        std::string out;
        for (size_t i = 0; i < len; ++i) {
            if (i) out += ", ";
            out += "\"";
            out += strs[i] ? flatten_ws(strs[i]) : "";
            out += "\"";
        }
        nc_free_string(len, strs.data());
        return out;
    }

    // Numeric: read as double and print compactly.
    std::vector<double> vals(len ? len : 1);
    if (nc_get_att_double(ncid, varid, name, vals.data()) != NC_NOERR) return {};
    std::string out;
    char buf[64];
    for (size_t i = 0; i < len; ++i) {
        if (i) out += ", ";
        double v = vals[i];
        if (v == (long long)v && std::fabs(v) < 1e15)
            std::snprintf(buf, sizeof(buf), "%lld", (long long)v);
        else
            std::snprintf(buf, sizeof(buf), "%g", v);
        out += buf;
    }
    return out;
}

static const char *type_name(nc_type t) {
    switch (t) {
        case NC_BYTE: return "byte";       case NC_UBYTE: return "ubyte";
        case NC_CHAR: return "char";       case NC_SHORT: return "short";
        case NC_USHORT: return "ushort";   case NC_INT: return "int";
        case NC_UINT: return "uint";       case NC_INT64: return "int64";
        case NC_UINT64: return "uint64";   case NC_FLOAT: return "float";
        case NC_DOUBLE: return "double";   case NC_STRING: return "string";
        default: return "?";
    }
}

std::vector<NcFile::MetaLine> NcFile::metadata_lines() const {
    std::vector<MetaLine> out;
    if (!isopen_) return out;

    out.push_back({1, "FILE"});
    out.push_back({0, "  " + path_});
    out.push_back({0, ""});

    // Dimensions.
    out.push_back({1, "DIMENSIONS"});
    for (const auto &d : dims_) {
        out.push_back({4, "  " + d.name + " = " + std::to_string(d.len)});
    }
    out.push_back({0, ""});

    // Variables and their attributes.
    out.push_back({1, "VARIABLES"});
    for (const auto &v : vars_) {
        nc_type xtype = NC_NAT;
        int vndims = 0, vnatts = 0;
        int dimids[NC_MAX_VAR_DIMS];
        nc_inq_var(ncid_, v.id, nullptr, &xtype, &vndims, dimids, &vnatts);

        std::string dimstr;
        for (int i = 0; i < vndims; ++i) {
            if (i) dimstr += ", ";
            dimstr += dim(dimids[i]).name;
        }
        out.push_back({2, "  " + std::string(type_name(xtype)) + " " + v.name +
                              "(" + dimstr + ")"});
        for (int a = 0; a < vnatts; ++a) {
            char an[NC_MAX_NAME + 1] = {0};
            if (nc_inq_attname(ncid_, v.id, a, an) != NC_NOERR) continue;
            out.push_back({3, "      :" + std::string(an) + " = " +
                                  format_att_value(ncid_, v.id, an)});
        }
    }
    out.push_back({0, ""});

    // Global attributes, shown after the variable definitions.
    int ndims = 0, nvars = 0, ngatts = 0, unlim = -1;
    nc_inq(ncid_, &ndims, &nvars, &ngatts, &unlim);
    out.push_back({1, "GLOBAL ATTRIBUTES"});
    if (ngatts == 0) out.push_back({0, "  (none)"});
    for (int a = 0; a < ngatts; ++a) {
        char an[NC_MAX_NAME + 1] = {0};
        if (nc_inq_attname(ncid_, NC_GLOBAL, a, an) != NC_NOERR) continue;
        out.push_back({3, "  :" + std::string(an) + " = " +
                              format_att_value(ncid_, NC_GLOBAL, an)});
    }
    return out;
}

bool NcFile::var_minmax(const NcVar &v, double &lo, double &hi) const {
    if (!v.numeric || v.ndims < 1) return false;

    std::vector<size_t> len(v.ndims);
    for (int i = 0; i < v.ndims; ++i) {
        len[i] = dim(v.dimids[i]).len;
        if (len[i] == 0) return false;
    }

    // Sub-sample with a uniform per-dimension stride so the read stays cheap
    // even for very large variables (cap on the number of sampled points).
    const size_t CAP = 4'000'000;
    auto sampled = [&](size_t s) {
        size_t c = 1;
        for (int i = 0; i < v.ndims; ++i) c *= (len[i] + s - 1) / s;
        return c;
    };
    size_t stride = 1;
    while (sampled(stride) > CAP) ++stride;

    std::vector<size_t> start(v.ndims, 0), count(v.ndims);
    std::vector<ptrdiff_t> strd(v.ndims, (ptrdiff_t)stride);
    size_t n = 1;
    for (int i = 0; i < v.ndims; ++i) {
        count[i] = (len[i] + stride - 1) / stride;
        n *= count[i];
    }

    std::vector<double> buf(n);
    int rc = (stride == 1)
                 ? nc_get_var_double(ncid_, v.id, buf.data())
                 : nc_get_vars_double(ncid_, v.id, start.data(), count.data(),
                                      strd.data(), buf.data());
    if (rc != NC_NOERR) return false;

    double mn = std::numeric_limits<double>::infinity();
    double mx = -std::numeric_limits<double>::infinity();
    for (double val : buf) {
        if (!std::isfinite(val)) continue;
        if (v.has_fill &&
            (val == v.fill || std::fabs(val - v.fill) <= 1e-6 * std::fabs(v.fill)))
            continue;
        if (std::fabs(val) >= 9.0e36) continue;
        if (v.packed) val = val * v.scale + v.offset;   // unpack to physical units
        if (val < mn) mn = val;
        if (val > mx) mx = val;
    }
    if (mn > mx) return false;
    lo = mn; hi = mx;
    return true;
}

std::vector<double> NcFile::read_series(const NcVar &v,
                                        const std::vector<size_t> &fixed,
                                        int series_pos, size_t yidx,
                                        size_t xidx, int yi, int xi) const {
    if (v.ndims < 1 || series_pos < 0 || series_pos >= v.ndims) return {};

    std::vector<size_t> start(v.ndims, 0), count(v.ndims, 1);
    for (int p = 0; p < v.ndims; ++p) {
        size_t len = dim(v.dimids[p]).len;
        if (len == 0) return {};
        if (p == series_pos) {
            start[p] = 0; count[p] = len;
        } else if (p == yi) {
            start[p] = std::min(yidx, len - 1); count[p] = 1;
        } else if (p == xi) {
            start[p] = std::min(xidx, len - 1); count[p] = 1;
        } else {
            size_t idx = (p < (int)fixed.size()) ? fixed[p] : 0;
            start[p] = std::min(idx, len - 1); count[p] = 1;
        }
    }

    size_t n = dim(v.dimids[series_pos]).len;
    std::vector<double> out(n);
    if (nc_get_vara_double(ncid_, v.id, start.data(), count.data(), out.data()) !=
        NC_NOERR)
        return {};

    for (double &val : out) {
        if (!std::isfinite(val)) { val = std::numeric_limits<double>::quiet_NaN(); continue; }
        if (v.has_fill &&
            (val == v.fill || std::fabs(val - v.fill) <= 1e-6 * std::fabs(v.fill))) {
            val = std::numeric_limits<double>::quiet_NaN(); continue;
        }
        if (std::fabs(val) >= 9.0e36) { val = std::numeric_limits<double>::quiet_NaN(); continue; }
        if (v.packed) val = val * v.scale + v.offset;   // unpack to physical units
    }
    return out;
}

Slice NcFile::read_slice(const NcVar &v, const std::vector<size_t> &fixed,
                         int yi, int xi) const {
    Slice s;
    if (v.ndims < 2 || yi < 0 || xi < 0 || yi >= v.ndims || xi >= v.ndims ||
        yi == xi)
        return s;

    std::vector<size_t> start(v.ndims, 0), count(v.ndims, 1);
    for (int p = 0; p < v.ndims; ++p) {
        if (p == yi || p == xi) {
            start[p] = 0;
            count[p] = dim(v.dimids[p]).len;
        } else {
            size_t idx = (p < (int)fixed.size()) ? fixed[p] : 0;
            size_t len = dim(v.dimids[p]).len;
            if (len == 0) return s;
            if (idx >= len) idx = len - 1;
            start[p] = idx;
            count[p] = 1;
        }
    }

    s.ny = (int)count[yi];
    s.nx = (int)count[xi];
    if (s.ny <= 0 || s.nx <= 0) return s;
    s.data.assign((size_t)s.ny * s.nx, 0.0);

    // The hyperslab read returns elements in the variable's storage order: of
    // the two display dims, the one with the smaller position varies slower
    // (the outer index); every fixed dim has count 1 and so contributes only a
    // unit stride. When y already precedes x in storage the buffer is the
    // desired row-major (y outer, x inner) layout and can be filled directly;
    // otherwise the two display axes are transposed on the way in.
    const bool y_outer = (yi < xi);
    if (y_outer) {
        int rc = nc_get_vara_double(ncid_, v.id, start.data(), count.data(),
                                    s.data.data());
        if (rc != NC_NOERR) { s.data.clear(); return s; }
    } else {
        std::vector<double> buf((size_t)s.ny * s.nx);
        int rc = nc_get_vara_double(ncid_, v.id, start.data(), count.data(),
                                    buf.data());
        if (rc != NC_NOERR) { s.data.clear(); return s; }
        // buf is x-outer, y-inner: buf[ix*ny + iy].
        for (int iy = 0; iy < s.ny; ++iy)
            for (int ix = 0; ix < s.nx; ++ix)
                s.data[(size_t)iy * s.nx + ix] = buf[(size_t)ix * s.ny + iy];
    }

    double lo = std::numeric_limits<double>::infinity();
    double hi = -std::numeric_limits<double>::infinity();
    const double fill = v.fill;
    const bool hasfill = v.has_fill;
    for (double &val : s.data) {
        if (!std::isfinite(val)) { val = std::numeric_limits<double>::quiet_NaN(); continue; }
        if (hasfill && (val == fill || std::fabs(val - fill) <= 1e-6 * std::fabs(fill))) {
            val = std::numeric_limits<double>::quiet_NaN();
            continue;
        }
        // Heuristic for unflagged sentinel fills.
        if (std::fabs(val) >= 9.0e36) {
            val = std::numeric_limits<double>::quiet_NaN();
            continue;
        }
        if (v.packed) val = val * v.scale + v.offset;   // unpack to physical units
        if (val < lo) lo = val;
        if (val > hi) hi = val;
    }
    if (lo <= hi) {
        s.dmin = lo; s.dmax = hi; s.valid = true;
    } else {
        s.dmin = 0; s.dmax = 1; s.valid = true; // all-missing slice
    }
    return s;
}
