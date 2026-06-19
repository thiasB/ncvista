#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

// A colormap is a list of RGB control points, linearly interpolated in [0,1].
struct ColorStop {
    double t;
    double r, g, b; // 0..1
};

struct Palette {
    std::string name;
    std::vector<ColorStop> stops;

    // Sample the colormap at position t in [0,1] -> 8-bit RGB.
    void sample(double t, uint8_t &r, uint8_t &g, uint8_t &b) const;
};

// Registry of built-in, perceptually reasonable colormaps.
const std::vector<Palette> &builtin_colormaps();
