#pragma once
#include <algorithm>
#include <cmath>

// Global map projections for longitude/latitude fields. All operate on the unit
// sphere with longitude/latitude in radians measured from the central meridian.
// Each projection is 2:1 in its natural coordinates; the raster is built by
// inverse-mapping output pixels, and vector overlays by forward-mapping points.
namespace proj {

enum Kind {
    EQUIRECT = 0, ROBINSON = 1, MOLLWEIDE = 2, HAMMER = 3,
    MERCATOR = 4, GALLPETERS = 5, COUNT = 6
};

inline const char *name(int k) {
    switch (k) {
        case MOLLWEIDE:  return "Mollweide";
        case HAMMER:     return "Hammer";
        case MERCATOR:   return "Mercator";
        case GALLPETERS: return "Gall-Peters";
        case ROBINSON:   return "Robinson";
        default:         return "Equirect";
    }
}

// Mercator latitude clamp (web-map convention) and the standard Robinson tables
// of parallel length (A) and equator distance (B), tabulated every 5°.
inline const double MERC_MAXLAT = 85.0 * M_PI / 180.0;
inline const double ROB_A[19] = {
    1.0000, 0.9986, 0.9954, 0.9900, 0.9822, 0.9730, 0.9600, 0.9427, 0.9216,
    0.8962, 0.8679, 0.8350, 0.7986, 0.7597, 0.7186, 0.6732, 0.6213, 0.5722,
    0.5322};
inline const double ROB_B[19] = {
    0.0000, 0.0620, 0.1240, 0.1860, 0.2480, 0.3100, 0.3720, 0.4340, 0.4958,
    0.5571, 0.6176, 0.6769, 0.7346, 0.7903, 0.8435, 0.8936, 0.9394, 0.9761,
    1.0000};
inline const double ROB_SX = 0.8487, ROB_SY = 1.3523;

// Half-extents of the projected bounding box: X in [-hx, hx], Y in [-hy, hy].
inline void extent(int k, double &hx, double &hy) {
    const double S2 = std::sqrt(2.0);
    switch (k) {
        case MOLLWEIDE:
        case HAMMER:     hx = 2 * S2; hy = S2; break;
        case MERCATOR:   hx = M_PI; hy = std::asinh(std::tan(MERC_MAXLAT)); break;
        case GALLPETERS: hx = M_PI / S2; hy = S2; break;          // standard par. 45°
        case ROBINSON:   hx = ROB_SX * M_PI; hy = ROB_SY; break;
        default:         hx = M_PI; hy = M_PI / 2; break;         // equirectangular
    }
}

// Forward: (lon, lat) radians -> projected (X, Y). Always defined here.
inline void forward(int k, double lon, double lat, double &X, double &Y) {
    const double S2 = std::sqrt(2.0);
    if (k == MOLLWEIDE) {
        double th = lat;
        if (std::fabs(lat) >= M_PI / 2 - 1e-9) {
            th = (lat > 0 ? M_PI / 2 : -M_PI / 2);
        } else {
            for (int i = 0; i < 30; ++i) {
                double f = 2 * th + std::sin(2 * th) - M_PI * std::sin(lat);
                double fp = 2 + 2 * std::cos(2 * th);
                double d = f / fp;
                th -= d;
                if (std::fabs(d) < 1e-11) break;
            }
        }
        X = (2 * S2 / M_PI) * lon * std::cos(th);
        Y = S2 * std::sin(th);
        return;
    }
    if (k == HAMMER) {
        double d = std::sqrt(1 + std::cos(lat) * std::cos(lon / 2));
        X = (2 * S2 * std::cos(lat) * std::sin(lon / 2)) / d;
        Y = (S2 * std::sin(lat)) / d;
        return;
    }
    if (k == MERCATOR) {
        X = lon;
        Y = std::asinh(std::tan(std::clamp(lat, -MERC_MAXLAT, MERC_MAXLAT)));
        return;
    }
    if (k == GALLPETERS) {                                // cylindrical equal-area
        X = lon / S2;                                     // cos 45° = 1/√2
        Y = S2 * std::sin(lat);
        return;
    }
    if (k == ROBINSON) {
        double d = std::fabs(lat) * 180.0 / M_PI / 5.0;   // table index (0..18)
        int i = std::min(17, (int)d);
        double f = d - i;
        double A = ROB_A[i] + f * (ROB_A[i + 1] - ROB_A[i]);
        double B = ROB_B[i] + f * (ROB_B[i + 1] - ROB_B[i]);
        X = ROB_SX * A * lon;
        Y = ROB_SY * B * (lat < 0 ? -1 : 1);
        return;
    }
    X = lon; Y = lat;                                     // equirectangular
}

// Inverse: projected (X, Y) -> (lon, lat) radians. Returns false outside the map.
inline bool inverse(int k, double X, double Y, double &lon, double &lat) {
    const double S2 = std::sqrt(2.0);
    if (k == MOLLWEIDE) {
        double t = Y / S2;
        if (t < -1 || t > 1) return false;
        double th = std::asin(t);
        double s = (2 * th + std::sin(2 * th)) / M_PI;
        if (s < -1 || s > 1) return false;
        lat = std::asin(s);
        double c = std::cos(th);
        lon = (std::fabs(c) < 1e-12) ? 0.0 : M_PI * X / (2 * S2 * c);
        return lon >= -M_PI - 1e-9 && lon <= M_PI + 1e-9;
    }
    if (k == HAMMER) {
        double a = X / 4.0, b = Y / 2.0;
        if (a * a + b * b > 0.5) return false;            // outside the map ellipse
        double z = std::sqrt(1 - a * a - b * b);
        lat = std::asin(std::clamp(z * Y, -1.0, 1.0));
        lon = 2 * std::atan2(z * X, 2 * (2 * z * z - 1));
        return true;
    }
    if (k == MERCATOR) {
        lat = std::atan(std::sinh(Y));
        lon = X;
        return lon >= -M_PI - 1e-9 && lon <= M_PI + 1e-9;
    }
    if (k == GALLPETERS) {
        double s = Y / S2;
        if (s < -1 || s > 1) return false;
        lat = std::asin(s);
        lon = X * S2;
        return lon >= -M_PI - 1e-9 && lon <= M_PI + 1e-9;
    }
    if (k == ROBINSON) {
        double Bt = std::fabs(Y) / ROB_SY;                // target equator distance
        if (Bt > 1) return false;
        int i = 0;
        while (i < 17 && ROB_B[i + 1] < Bt) ++i;
        double f = (Bt - ROB_B[i]) / (ROB_B[i + 1] - ROB_B[i]);
        double latdeg = 5.0 * (i + f);
        lat = latdeg * M_PI / 180.0 * (Y < 0 ? -1 : 1);
        double A = ROB_A[i] + f * (ROB_A[i + 1] - ROB_A[i]);
        lon = X / (ROB_SX * A);
        return lon >= -M_PI - 1e-9 && lon <= M_PI + 1e-9;
    }
    lon = X; lat = Y;                                     // equirectangular
    return lon >= -M_PI - 1e-9 && lon <= M_PI + 1e-9 &&
           lat >= -M_PI / 2 - 1e-9 && lat <= M_PI / 2 + 1e-9;
}

}  // namespace proj
