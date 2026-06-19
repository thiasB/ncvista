#pragma once
#include <string>

// Thin wrapper over udunits2. Used for:
//   * pretty-printing unit strings (canonical UTF-8 symbols),
//   * detecting time axes and converting their values to calendar dates.
class Units {
public:
    Units();
    ~Units();

    bool ok() const { return sys_ != nullptr; }

    // Canonical, human-readable form of a unit spec ("degree_Celsius" -> "°C").
    // Returns the input unchanged if it cannot be parsed.
    std::string pretty(const std::string &spec) const;

    // True if `spec` is a time coordinate ("<period> since <reference>").
    bool is_time(const std::string &spec) const;

    // Format the time coordinate value `value` (expressed in unit `spec`) as a
    // calendar string. `calendar` is the CF calendar attribute (may be empty).
    // Falls back to a numeric rendering if conversion is impossible.
    std::string format_time(const std::string &spec, const std::string &calendar,
                            double value) const;

private:
    void *sys_ = nullptr;       // ut_system*
    void *ref_ = nullptr;       // ut_unit* "seconds since 1970-01-01 00:00:00 UTC"

    // Non-standard calendars (360_day, noleap, all_leap) handled arithmetically.
    std::string format_time_calendar(const std::string &spec,
                                      const std::string &calendar,
                                      double value) const;
};
