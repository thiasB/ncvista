#include "overlay.hpp"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

bool load_coastlines(const std::string &path, Coastlines &out) {
    out.lines.clear();
    out.loaded = false;
    if (path.empty()) return false;

    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return false;

    int32_t nlines = 0;
    if (std::fread(&nlines, sizeof(int32_t), 1, f) != 1 || nlines <= 0 ||
        nlines > 5'000'000) {
        std::fclose(f);
        return false;
    }
    out.lines.reserve(nlines);
    for (int32_t i = 0; i < nlines; ++i) {
        int32_t npts = 0;
        if (std::fread(&npts, sizeof(int32_t), 1, f) != 1 || npts < 0 ||
            npts > 50'000'000) {
            std::fclose(f);
            out.lines.clear();
            return false;
        }
        std::vector<std::pair<float, float>> line;
        line.resize(npts);
        for (int32_t k = 0; k < npts; ++k) {
            float xy[2];
            if (std::fread(xy, sizeof(float), 2, f) != 2) {
                std::fclose(f);
                out.lines.clear();
                return false;
            }
            line[k] = {xy[0], xy[1]};
        }
        out.lines.push_back(std::move(line));
    }
    std::fclose(f);
    out.loaded = true;
    return true;
}

static std::string exe_dir() {
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return {};
    buf[n] = '\0';
    std::string p(buf);
    size_t sl = p.find_last_of('/');
    return sl == std::string::npos ? std::string() : p.substr(0, sl);
}

static bool exists(const std::string &p) {
    if (p.empty()) return false;
    return access(p.c_str(), R_OK) == 0;
}

// Resolve a data file by basename: an env override, then paths relative to the
// executable, then the compiled-in install/source defaults.
static std::string resolve_data(const char *env, const char *base,
                                const char *install, const char *src) {
    if (env)
        if (const char *e = std::getenv(env))
            if (exists(e)) return e;

    std::string dir = exe_dir();
    std::vector<std::string> cands;
    if (!dir.empty()) {
        cands.push_back(dir + "/" + base);
        cands.push_back(dir + "/../share/ncvista/" + base);
    }
    if (install && *install) cands.push_back(install);
    if (src && *src) cands.push_back(src);
    for (const auto &c : cands)
        if (exists(c)) return c;
    return {};
}

std::string default_coastline_path() {
    return resolve_data("NCVISTA_COAST", "coastlines.bin",
#ifdef NCVISTA_COAST_INSTALL
                        NCVISTA_COAST_INSTALL,
#else
                        nullptr,
#endif
#ifdef NCVISTA_COAST_SRC
                        NCVISTA_COAST_SRC
#else
                        nullptr
#endif
    );
}

std::string default_borders_path() {
    return resolve_data("NCVISTA_BORDERS", "borders.bin",
#ifdef NCVISTA_BORDERS_INSTALL
                        NCVISTA_BORDERS_INSTALL,
#else
                        nullptr,
#endif
#ifdef NCVISTA_BORDERS_SRC
                        NCVISTA_BORDERS_SRC
#else
                        nullptr
#endif
    );
}
