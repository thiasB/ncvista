#include "coastline.hpp"
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

std::string default_coastline_path() {
    if (const char *env = std::getenv("NCVISTA_COAST"))
        if (exists(env)) return env;

    std::string dir = exe_dir();
    std::vector<std::string> cands;
    if (!dir.empty()) {
        cands.push_back(dir + "/coastlines.bin");
        cands.push_back(dir + "/../share/ncvista/coastlines.bin");
    }
#ifdef NCVISTA_COAST_INSTALL
    cands.push_back(NCVISTA_COAST_INSTALL);
#endif
#ifdef NCVISTA_COAST_SRC
    cands.push_back(NCVISTA_COAST_SRC);
#endif
    for (const auto &c : cands)
        if (exists(c)) return c;
    return {};
}
