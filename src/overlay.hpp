#pragma once
#include <string>
#include <utility>
#include <vector>

// World coastlines as geographic polylines (Natural Earth 50m), loaded from a
// binary file shipped alongside the program. Coordinates are (lon, lat) in
// degrees, lon in [-180, 180].
struct Coastlines {
    std::vector<std::vector<std::pair<float, float>>> lines;
    bool loaded = false;
};

// Load coastlines from `path`. Returns false (and leaves out.loaded == false)
// if the file is missing or malformed.
bool load_coastlines(const std::string &path, Coastlines &out);

// Best-effort resolution of the data files: an environment override, then
// locations relative to the executable, then the compiled-in install/source
// defaults. Borders use the same binary format as coastlines.
std::string default_coastline_path();
std::string default_borders_path();
