#include "colormap.hpp"
#include <algorithm>
#include <cmath>

void Palette::sample(double t, uint8_t &r, uint8_t &g, uint8_t &b) const {
    if (!std::isfinite(t)) t = 0.0;
    t = std::clamp(t, 0.0, 1.0);
    if (stops.empty()) { r = g = b = 0; return; }
    if (t <= stops.front().t) {
        r = (uint8_t)std::lround(stops.front().r * 255.0);
        g = (uint8_t)std::lround(stops.front().g * 255.0);
        b = (uint8_t)std::lround(stops.front().b * 255.0);
        return;
    }
    if (t >= stops.back().t) {
        r = (uint8_t)std::lround(stops.back().r * 255.0);
        g = (uint8_t)std::lround(stops.back().g * 255.0);
        b = (uint8_t)std::lround(stops.back().b * 255.0);
        return;
    }
    for (size_t i = 1; i < stops.size(); ++i) {
        if (t <= stops[i].t) {
            const ColorStop &a = stops[i - 1];
            const ColorStop &c = stops[i];
            double span = c.t - a.t;
            double f = span > 0 ? (t - a.t) / span : 0.0;
            r = (uint8_t)std::lround((a.r + f * (c.r - a.r)) * 255.0);
            g = (uint8_t)std::lround((a.g + f * (c.g - a.g)) * 255.0);
            b = (uint8_t)std::lround((a.b + f * (c.b - a.b)) * 255.0);
            return;
        }
    }
    r = g = b = 0;
}

// Helper to build an evenly-spaced colormap from a list of RGB triples (0..1).
static Palette make(const std::string &name,
                     std::initializer_list<std::array<double, 3>> cols) {
    Palette cm;
    cm.name = name;
    size_t n = cols.size();
    size_t i = 0;
    for (const auto &c : cols) {
        double t = (n <= 1) ? 0.0 : (double)i / (double)(n - 1);
        cm.stops.push_back({t, c[0], c[1], c[2]});
        ++i;
    }
    return cm;
}

const std::vector<Palette> &builtin_colormaps() {
    static const std::vector<Palette> maps = {
        // Viridis (sampled control points) — perceptually uniform, default.
        make("viridis",
             {{0.267, 0.005, 0.329}, {0.283, 0.141, 0.458}, {0.254, 0.265, 0.530},
              {0.207, 0.372, 0.553}, {0.164, 0.471, 0.558}, {0.128, 0.567, 0.551},
              {0.135, 0.659, 0.518}, {0.267, 0.749, 0.441}, {0.478, 0.821, 0.318},
              {0.741, 0.873, 0.150}, {0.993, 0.906, 0.144}}),
        // Inferno
        make("inferno",
             {{0.001, 0.000, 0.014}, {0.087, 0.044, 0.224}, {0.258, 0.039, 0.406},
              {0.417, 0.090, 0.433}, {0.578, 0.148, 0.404}, {0.735, 0.215, 0.330},
              {0.865, 0.317, 0.226}, {0.954, 0.469, 0.099}, {0.988, 0.645, 0.040},
              {0.964, 0.843, 0.273}, {0.988, 0.998, 0.645}}),
        // Cividis — colour-blind friendly
        make("cividis",
             {{0.000, 0.135, 0.305}, {0.000, 0.204, 0.404}, {0.196, 0.275, 0.400},
              {0.314, 0.345, 0.420}, {0.416, 0.420, 0.443}, {0.518, 0.494, 0.451},
              {0.624, 0.572, 0.439}, {0.737, 0.654, 0.404}, {0.855, 0.741, 0.341},
              {0.969, 0.835, 0.232}}),
        // Diverging blue-white-red (good for anomalies)
        make("blue-red",
             {{0.020, 0.188, 0.380}, {0.129, 0.400, 0.674}, {0.420, 0.676, 0.819},
              {0.776, 0.859, 0.937}, {0.969, 0.969, 0.969}, {0.992, 0.859, 0.780},
              {0.957, 0.647, 0.510}, {0.839, 0.376, 0.302}, {0.698, 0.094, 0.168}}),
        // Thermal / "magma-ish"
        make("thermal",
             {{0.015, 0.013, 0.137}, {0.205, 0.071, 0.392}, {0.435, 0.118, 0.504},
              {0.659, 0.196, 0.490}, {0.878, 0.297, 0.376}, {0.984, 0.500, 0.369},
              {0.996, 0.733, 0.506}, {0.987, 0.918, 0.745}}),
        // Ocean (deep blue to teal to pale)
        make("ocean",
             {{0.012, 0.078, 0.247}, {0.027, 0.235, 0.451}, {0.027, 0.420, 0.553},
              {0.180, 0.600, 0.588}, {0.451, 0.745, 0.557}, {0.749, 0.867, 0.580},
              {0.969, 0.965, 0.706}}),
        // Grayscale
        make("grayscale", {{0.0, 0.0, 0.0}, {1.0, 1.0, 1.0}}),
    };
    return maps;
}
