#pragma once
#include <string>
#include <vector>

// Lightweight wrapper around the netCDF C API, exposing just what the viewer
// needs: dimensions, displayable variables, coordinate variables and 2-D slices.

struct NcDim {
    int id = -1;
    std::string name;
    size_t len = 0;
};

struct NcVar {
    int id = -1;
    std::string name;
    int ndims = 0;
    std::vector<int> dimids;     // dimension ids in storage order
    std::string units;           // raw "units" attribute (may be empty)
    std::string long_name;       // "long_name" or "standard_name" attribute
    bool has_fill = false;
    double fill = 0.0;           // in packed/stored units (compared before unpacking)
    bool numeric = false;        // can be read as double and plotted
    bool aux = false;            // supporting var (bounds, coords, grid_mapping…)
    // CF packing: physical = stored * scale + offset (identity when absent).
    bool packed = false;
    double scale = 1.0;
    double offset = 0.0;
};

// A 2-D slice ready for rendering. data has ny*nx values, row-major (y outer).
struct Slice {
    int ny = 0, nx = 0;
    std::vector<double> data;    // NaN marks missing/fill
    double dmin = 0.0, dmax = 0.0;
    bool valid = false;
};

class NcFile {
public:
    NcFile() = default;
    ~NcFile();

    bool open(const std::string &path, std::string &err);
    void close();

    const std::string &path() const { return path_; }
    const std::vector<NcDim> &dims() const { return dims_; }
    const std::vector<NcVar> &vars() const { return vars_; }

    // Indices (into vars_) of variables with >= 2 dimensions.
    const std::vector<int> &displayable() const { return displayable_; }

    const NcDim &dim(int dimid) const;

    // Read the 2-D field for variable v. Dimension positions `yi` and `xi`
    // (within v.dimids) map to the display rows (y) and columns (x); every
    // other dim is fixed at fixed_index[dimid_position]. The result is always
    // packed row-major (y outer, x inner) regardless of the dims' storage
    // order, so yi may be greater than xi (e.g. an X-before-Y layout).
    // `fixed` is indexed by position within v.dimids (size == v.ndims).
    Slice read_slice(const NcVar &v, const std::vector<size_t> &fixed,
                     int yi, int xi) const;

    // Quick estimate of the variable's global value range across the whole
    // file (sub-sampled with a stride for large variables). Ignores fill /
    // missing values. Returns false if nothing valid could be read.
    bool var_minmax(const NcVar &v, double &lo, double &hi) const;

    // Read the 1-D series of variable v along dimension position `series_pos`,
    // holding the y (row) and x (column) display dims — at positions `yi`/`xi`
    // within v.dimids — at yidx/xidx, and every other dimension at its value in
    // `fixed`. Pass yi/xi == -1 when there are no display dims (1-D variable).
    // NaN marks fill/missing values. Returns an empty vector on error.
    std::vector<double> read_series(const NcVar &v, const std::vector<size_t> &fixed,
                                    int series_pos, size_t yidx, size_t xidx,
                                    int yi, int xi) const;

    // Return values of the 1-D coordinate variable that shares a dimension's
    // name (e.g. "lat", "time"). Empty if none exists or unreadable.
    std::vector<double> coord_values(int dimid) const;

    // The units string of the coordinate variable for a dimension (if any).
    std::string coord_units(int dimid) const;
    // The calendar attribute of the coordinate variable for a dimension.
    std::string coord_calendar(int dimid) const;
    // An arbitrary text attribute of the coordinate variable for a dimension
    // (e.g. "axis", "standard_name"); empty if absent or non-textual.
    std::string coord_attr(int dimid, const char *name) const;

    // A line is one row of the metadata view; `kind` drives colouring:
    //   0 plain, 1 section header, 2 variable name, 3 attribute name, 4 dimension
    // `cont` marks a soft-wrapped continuation of the previous logical line
    // (set by the viewer when wrapping, never by metadata_lines()).
    struct MetaLine { int kind; std::string text; bool cont = false; };
    std::vector<MetaLine> metadata_lines() const;

    int ncid() const { return ncid_; }

private:
    std::string read_text_att(int varid, const char *name) const;
    int coord_varid(int dimid) const; // varid of coord var sharing dim name, or -1

    int ncid_ = -1;
    bool isopen_ = false;
    std::string path_;
    std::vector<NcDim> dims_;
    std::vector<NcVar> vars_;
    std::vector<int> displayable_;
};
