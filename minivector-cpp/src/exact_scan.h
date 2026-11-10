#pragma once

#include <cmath>
#include <cstddef>
#include <limits>

// Partner contribution on top of the team's v25 engine. Screening uses direct
// differences, never norm(a)+norm(b)-2*dot(a,b). Only a conservative bound may
// reject a row; every retained candidate has a compensated f64 distance.
namespace exact_scan {

inline double distance(const float* a, const float* b, size_t n) {
    double sum = 0.0, correction = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double d = static_cast<double>(a[i]) - static_cast<double>(b[i]);
        const double term = d * d;
        const double next = sum + term;
        correction += sum >= term ? (sum - next) + term : (term - next) + sum;
        sum = next;
    }
    return static_cast<double>(std::sqrt(static_cast<long double>(sum) + correction));
}

struct Bound {
    double relative;
    double absolute;
    explicit Bound(size_t dim)
        : relative(8.0 * std::numeric_limits<float>::epsilon() * (dim + 8.0)),
          absolute(16.0 * std::numeric_limits<float>::min() * (dim + 1.0)) {}

    bool rejects(float approximate_squared, double worst_distance, bool ascending) const {
        // Fall back for overflow and impractically large error bounds. The
        // absolute allowance covers underflow/FTZ; the relative allowance
        // covers subtraction, multiplication, reduction and squared threshold.
        if (!std::isfinite(approximate_squared) || relative >= 0.25) return false;
        const double threshold = worst_distance * worst_distance;
        const double error = threshold * relative + absolute;
        return ascending ? approximate_squared > threshold + error
                         : approximate_squared < threshold - error;
    }
};

}  // namespace exact_scan
