#include "units.hpp"
#include <udunits2.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>

Units::Units() {
    // Silence udunits' chatty stderr diagnostics.
    ut_set_error_message_handler(ut_ignore);

    const char *xml = std::getenv("UDUNITS2_XML_PATH");
    ut_system *sys = ut_read_xml(xml);  // xml==NULL -> env or compiled default
    if (!sys) {
        // Fall back to the common install locations across distributions,
        // package managers (Homebrew, MacPorts) and conda environments.
        std::vector<std::string> candidates;
        for (const char *env : {"CONDA_PREFIX", "PREFIX"}) {
            if (const char *p = std::getenv(env))
                candidates.push_back(std::string(p) + "/share/udunits/udunits2.xml");
        }
        candidates.insert(candidates.end(), {
            "/usr/share/udunits/udunits2.xml",
            "/usr/local/share/udunits/udunits2.xml",
            "/usr/share/xml/udunits/udunits2.xml",
            "/opt/homebrew/share/udunits/udunits2.xml",  // Homebrew (Apple Silicon)
            "/usr/local/Cellar/udunits/share/udunits/udunits2.xml",
            "/opt/local/share/udunits/udunits2.xml",      // MacPorts
        });
        for (const auto &c : candidates) {
            sys = ut_read_xml(c.c_str());
            if (sys) break;
        }
    }
    sys_ = sys;
    if (sys_) {
        ut_unit *ref = ut_parse((ut_system *)sys_,
                                "seconds since 1970-01-01 00:00:00 UTC", UT_ASCII);
        ref_ = ref;
    }
}

Units::~Units() {
    if (ref_) ut_free((ut_unit *)ref_);
    if (sys_) ut_free_system((ut_system *)sys_);
}

std::string Units::pretty(const std::string &spec) const {
    // Show units exactly as written in the netCDF "units" attribute. udunits'
    // canonical formatter reorders the factors (e.g. "kg m-2" -> "m-2 kg"),
    // so the original spec is passed through verbatim.
    return spec;
}

bool Units::is_time(const std::string &spec) const {
    if (spec.empty()) return false;
    std::string lo = spec;
    std::transform(lo.begin(), lo.end(), lo.begin(), ::tolower);
    if (lo.find("since") == std::string::npos) return false;
    if (!sys_ || !ref_) return true; // textual heuristic is enough
    ut_unit *u = ut_parse((ut_system *)sys_, spec.c_str(), UT_ASCII);
    if (!u) return true;
    bool conv = ut_are_convertible(u, (ut_unit *)ref_) != 0;
    ut_free(u);
    return conv;
}

// ---- Proleptic Gregorian day arithmetic (Howard Hinnant's algorithms) ------

static long days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}

static void civil_from_days(long z, int &y, unsigned &m, unsigned &d) {
    z += 719468;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = (int)yoe + (int)(era * 400);
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp + (mp < 10 ? 3 : -9);
    y += (m <= 2);
}

static bool is_leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
static int days_in_year(int y) { return is_leap(y) ? 366 : 365; }
static int days_in_month(int y, int m) {
    static const int md[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m == 2 && is_leap(y)) return 29;
    return md[(m - 1) % 12];
}

static std::string fmt_date(int y, int mo, int d, int h, int mi, double s) {
    char buf[64];
    if (h == 0 && mi == 0 && s < 0.5)
        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", y, mo, d);
    else
        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02.0f",
                      y, mo, d, h, mi, s);
    return buf;
}

std::string Units::format_time(const std::string &spec, const std::string &calendar,
                               double value) const {
    std::string cal = calendar;
    std::transform(cal.begin(), cal.end(), cal.begin(), ::tolower);

    const bool standard = cal.empty() || cal == "standard" || cal == "gregorian" ||
                          cal == "proleptic_gregorian" || cal == "julian";

    // "years since"/"months since" need calendar-aware handling: udunits treats
    // a year as a fixed 365.2422-day duration, which drifts from the civil
    // calendar (e.g. index 499 of "years since 1601-01-01" -> 2099-12-31
    // 20:34:21 rather than 2100-01-01). Advance whole calendar years/months.
    if (standard) {
        std::string out = format_time_year_month(spec, value);
        if (!out.empty()) return out;
    }

    if (sys_ && ref_ && standard) {
        ut_unit *u = ut_parse((ut_system *)sys_, spec.c_str(), UT_ASCII);
        if (u) {
            std::string out;
            if (ut_are_convertible(u, (ut_unit *)ref_)) {
                cv_converter *cv = ut_get_converter(u, (ut_unit *)ref_);
                if (cv) {
                    double secs = cv_convert_double(cv, value);
                    cv_free(cv);
                    time_t tt = (time_t)std::llround(secs);
                    struct tm tmv;
                    if (gmtime_r(&tt, &tmv)) {
                        out = fmt_date(tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                                       tmv.tm_hour, tmv.tm_min, (double)tmv.tm_sec);
                    }
                }
            }
            ut_free(u);
            if (!out.empty()) return out;
        }
    }

    if (!standard) {
        std::string out = format_time_calendar(spec, cal, value);
        if (!out.empty()) return out;
    }

    char buf[64];
    std::snprintf(buf, sizeof(buf), "%g", value);
    return buf;
}

// Advance whole calendar years/months from the reference date. Any fractional
// part of `value` is applied as a fraction of the length of the year/month it
// lands in, so integer indices map to exact civil dates. Returns "" unless the
// unit's period is years or months.
std::string Units::format_time_year_month(const std::string &spec,
                                          double value) const {
    std::string lo = spec;
    std::transform(lo.begin(), lo.end(), lo.begin(), ::tolower);
    size_t sp = lo.find("since");
    if (sp == std::string::npos) return {};

    std::string period = lo.substr(0, sp);
    size_t a = period.find_first_not_of(" \t");
    size_t b = period.find_last_not_of(" \t");
    if (a == std::string::npos) return {};
    period = period.substr(a, b - a + 1);

    const bool is_year = period.rfind("year", 0) == 0 || period == "yr" ||
                         period == "yrs" || period == "y";
    const bool is_month = period.rfind("month", 0) == 0 || period == "mon" ||
                          period == "mons";
    if (!is_year && !is_month) return {};

    std::string ref = spec.substr(sp + 5);
    int y0, mo0, d0, h0 = 0, mi0 = 0; double s0 = 0.0;
    int n = std::sscanf(ref.c_str(), " %d-%d-%d %d:%d:%lf",
                        &y0, &mo0, &d0, &h0, &mi0, &s0);
    if (n < 3)
        n = std::sscanf(ref.c_str(), " %d-%d-%dT%d:%d:%lf",
                        &y0, &mo0, &d0, &h0, &mi0, &s0);
    if (n < 3) return {};

    int ny, nm; double frac;
    if (is_year) {
        long whole = (long)std::floor(value);
        frac = value - (double)whole;
        ny = y0 + (int)whole; nm = mo0;
    } else {
        long whole = (long)std::floor(value);
        frac = value - (double)whole;
        long tot = (long)(mo0 - 1) + whole;             // months from year 0's Jan
        long yy = tot / 12, mm = tot % 12;
        if (mm < 0) { mm += 12; yy -= 1; }
        ny = y0 + (int)yy; nm = (int)mm + 1;
    }

    double period_days = is_year ? days_in_year(ny) : days_in_month(ny, nm);
    double total_sec = (double)days_from_civil(ny, (unsigned)nm, (unsigned)d0) * 86400.0
                       + h0 * 3600.0 + mi0 * 60.0 + s0
                       + frac * period_days * 86400.0;

    long day = (long)std::floor(total_sec / 86400.0);
    double rem = total_sec - (double)day * 86400.0;
    int hh = (int)(rem / 3600.0); rem -= hh * 3600.0;
    int mm = (int)(rem / 60.0); rem -= mm * 60.0;

    int Y; unsigned Mo, D;
    civil_from_days(day, Y, Mo, D);
    return fmt_date(Y, (int)Mo, (int)D, hh, mm, rem);
}

// ---- Manual arithmetic for non-standard CF calendars -----------------------

static bool parse_time_unit(const std::string &spec, double &sec_per_unit,
                            int &y0, int &mo0, int &d0, int &h0, int &mi0, double &s0) {
    // Expected: "<period> since <YYYY-MM-DD[ HH:MM:SS]>"
    std::string lo = spec;
    std::transform(lo.begin(), lo.end(), lo.begin(), ::tolower);
    size_t sp = lo.find("since");
    if (sp == std::string::npos) return false;

    std::string period = lo.substr(0, sp);
    // trim
    auto trim = [](std::string &x) {
        size_t a = x.find_first_not_of(" \t");
        size_t b = x.find_last_not_of(" \t");
        if (a == std::string::npos) { x.clear(); return; }
        x = x.substr(a, b - a + 1);
    };
    trim(period);
    if (period.rfind("sec", 0) == 0 || period == "s") sec_per_unit = 1.0;
    else if (period.rfind("min", 0) == 0) sec_per_unit = 60.0;
    else if (period.rfind("hour", 0) == 0 || period == "hr" || period == "hrs" || period == "h")
        sec_per_unit = 3600.0;
    else if (period.rfind("day", 0) == 0 || period == "d") sec_per_unit = 86400.0;
    else return false; // months/years undefined for these calendars

    std::string ref = spec.substr(sp + 5);
    h0 = mi0 = 0; s0 = 0.0;
    int n = std::sscanf(ref.c_str(), " %d-%d-%d %d:%d:%lf",
                        &y0, &mo0, &d0, &h0, &mi0, &s0);
    if (n < 3) {
        n = std::sscanf(ref.c_str(), " %d-%d-%dT%d:%d:%lf",
                        &y0, &mo0, &d0, &h0, &mi0, &s0);
    }
    return n >= 3;
}

std::string Units::format_time_calendar(const std::string &spec,
                                        const std::string &cal, double value) const {
    double spu; int y0, mo0, d0, h0, mi0; double s0;
    if (!parse_time_unit(spec, spu, y0, mo0, d0, h0, mi0, s0)) return {};

    int mlen[12];
    int days_per_year;
    bool real_months = false;
    if (cal == "360_day" || cal == "360") {
        for (int i = 0; i < 12; ++i) mlen[i] = 30;
        days_per_year = 360;
    } else if (cal == "noleap" || cal == "365_day" || cal == "365") {
        int m[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        for (int i = 0; i < 12; ++i) mlen[i] = m[i];
        days_per_year = 365; real_months = true;
    } else if (cal == "all_leap" || cal == "366_day" || cal == "366") {
        int m[12] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        for (int i = 0; i < 12; ++i) mlen[i] = m[i];
        days_per_year = 366; real_months = true;
    } else {
        return {};
    }
    (void)real_months;

    // Total seconds from the reference instant.
    double total_sec = value * spu + s0 + mi0 * 60.0 + h0 * 3600.0;
    long total_days = (long)std::floor(total_sec / 86400.0);
    double rem = total_sec - (double)total_days * 86400.0;
    int hh = (int)(rem / 3600.0); rem -= hh * 3600.0;
    int mm = (int)(rem / 60.0); rem -= mm * 60.0;
    double ss = rem;

    // Day index within reference year (0-based from Jan 1 of that calendar).
    long doy = d0 - 1;
    for (int m = 0; m < mo0 - 1; ++m) doy += mlen[m];

    long abs_day = (long)(y0) * days_per_year + doy + total_days;
    int year = (int)(abs_day / days_per_year);
    long day_in_year = abs_day - (long)year * days_per_year;
    while (day_in_year < 0) { year--; day_in_year += days_per_year; }

    int month = 0;
    while (month < 12 && day_in_year >= mlen[month]) { day_in_year -= mlen[month]; month++; }
    int day = (int)day_in_year + 1;

    return fmt_date(year, month + 1, day, hh, mm, ss);
}
