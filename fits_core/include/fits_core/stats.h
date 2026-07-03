#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

// Order-statistic helpers shared across the analysis / PSF / background / stretch
// paths, which had each re-rolled their own nth_element median. Two flavours,
// because the callers genuinely need both:
//   * median_inplace / quantile_inplace  -- partial-sort the caller's vector
//     (fine when it's a throwaway local);
//   * median_copy                         -- takes a copy, so a vector the
//     caller reuses afterwards is left untouched (the background code relies on
//     this).
// Behaviour matches the previous per-file helpers exactly (lower-middle element
// for even n, k = q*(n-1) for quantiles), so no numeric output shifts.
namespace fitsx {

template <typename T>
T median_inplace(std::vector<T>& v) {
    if (v.empty()) return T(0);
    auto m = v.begin() + v.size() / 2;
    std::nth_element(v.begin(), m, v.end());
    return *m;
}

template <typename T>
T median_copy(std::vector<T> v) {   // by value on purpose -- see header note
    return median_inplace(v);
}

template <typename T>
T quantile_inplace(std::vector<T>& v, double q) {
    if (v.empty()) return T(0);
    const size_t k = std::min(v.size() - 1,
                              static_cast<size_t>(q * (v.size() - 1)));
    std::nth_element(v.begin(), v.begin() + k, v.end());
    return v[k];
}

// Robust sigma from the median absolute deviation (1.4826 * MAD), floored at
// 1e-9 so downstream divisions stay finite. Takes a copy (needs the values
// intact to build the deviations after computing the median).
inline double mad_sigma(std::vector<double> v) {
    if (v.empty()) return 1e-9;
    const double m = median_copy(v);
    std::vector<double> d(v.size());
    for (size_t i = 0; i < v.size(); ++i) d[i] = std::abs(v[i] - m);
    const double s = 1.4826 * median_inplace(d);
    return s > 0 ? s : 1e-9;
}

}  // namespace fitsx
