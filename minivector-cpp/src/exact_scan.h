#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <array>
#include <limits>

// Partner contribution on top of the team's v25 engine. Screening uses direct
// differences, never norm(a)+norm(b)-2*dot(a,b). Only a conservative bound may
// reject a row; every retained candidate has a compensated f64 distance.
namespace exact_scan {

namespace detail {

// A finite binary32 is an integer multiple of 2^-149. Its squared distance
// is therefore an integer times 2^-298. Ten limbs also cover SIZE_MAX terms.
struct Integer {
    std::array<uint64_t, 10> words{};
    void add_word(uint64_t value, unsigned index) {
        while (value && index < words.size()) {
            uint64_t old = words[index];
            words[index++] += value;
            value = words[index - 1] < old;
        }
    }
    void sub_word(uint64_t value, unsigned index) {
        while (value && index < words.size()) {
            uint64_t old = words[index];
            words[index++] -= value;
            value = old < value;
        }
    }
    void term(uint64_t value, unsigned shift, bool subtract = false) {
        unsigned index = shift / 64, bits = shift % 64;
        uint64_t low = value << bits;
        uint64_t high = bits ? value >> (64 - bits) : 0;
        if (subtract) {
            sub_word(low, index);
            sub_word(high, index + 1);
        } else {
            add_word(low, index);
            add_word(high, index + 1);
        }
    }
    int compare(const Integer& other) const {
        for (size_t i = words.size(); i-- > 0;) {
            if (words[i] != other.words[i]) return words[i] < other.words[i] ? -1 : 1;
        }
        return 0;
    }
    Integer shifted(unsigned bits) const {
        Integer result;
        for (unsigned i = 0; i < words.size(); ++i) {
            if (words[i]) result.term(words[i], i * 64 + bits);
        }
        return result;
    }
    long double approximate_squared() const {
        long double value = 0;
        for (size_t i = words.size(); i-- > 0;)
            value = std::ldexp(value, 64) + words[i];
        return std::ldexp(value, -298);
    }
};

struct FloatParts {
    uint64_t mantissa;
    unsigned shift;
    bool negative;
    explicit FloatParts(float value) {
        uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        unsigned exponent = (bits >> 23) & 255;
        mantissa = (bits & 0x7fffffu) | (exponent ? 0x800000u : 0);
        shift = exponent ? exponent - 1 : 0;
        negative = (bits >> 31) != 0;
    }
};

// Compare the exact square with the squared midpoint between lower and its
// next double. All distances between distinct binary32 values are normal f64.
inline int compare_midpoint(const Integer& sum, double lower) {
    uint64_t bits;
    std::memcpy(&bits, &lower, sizeof(bits));
    uint64_t mantissa = (bits & ((uint64_t{1} << 52) - 1)) | (uint64_t{1} << 52);
    int exponent = static_cast<int>((bits >> 52) & 2047) - 1023 - 52;
    uint64_t middle = 2 * mantissa + 1;
    // 54-bit product, expressed portably as two 64-bit limbs.
    uint64_t lo = static_cast<uint32_t>(middle), hi = middle >> 32;
    uint64_t cross = 2 * lo * hi;
    Integer square;
    square.term(lo * lo, 0);
    square.term(cross, 32);
    square.term(hi * hi, 64);
    int shift = 2 * exponent - 2 + 298;
    return shift >= 0 ? sum.compare(square.shifted(static_cast<unsigned>(shift)))
                      : sum.shifted(static_cast<unsigned>(-shift)).compare(square);
}

template <class Load>
double exact_distance(const Load& load, const float* b, size_t n) {
    Integer sum;
    for (size_t i = 0; i < n; ++i) {
        FloatParts x(load(i)), y(b[i]);
        sum.term(x.mantissa * x.mantissa, 2 * x.shift);
        sum.term(y.mantissa * y.mantissa, 2 * y.shift);
        sum.term(x.mantissa * y.mantissa, x.shift + y.shift + 1,
                 x.negative == y.negative);
    }
    double value = static_cast<double>(std::sqrt(sum.approximate_squared()));
    if (value == 0) return 0;
    for (;;) {
        uint64_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        bool odd = (bits & 1) != 0;
        double below = std::nextafter(value, 0.0);
        int low = compare_midpoint(sum, below);
        if (low < 0 || (low == 0 && odd)) { value = below; continue; }
        int high = compare_midpoint(sum, value);
        if (high > 0 || (high == 0 && odd)) {
            value = std::nextafter(value, std::numeric_limits<double>::infinity());
            continue;
        }
        return value;
    }
}

template <class Load>
double distance(const Load& load, const float* b, size_t n) {
    long double sum = 0, correction = 0;
    for (size_t i = 0; i < n; ++i) {
        long double d = static_cast<long double>(load(i)) - b[i];
        long double term = d * d, next = sum + term;
        correction += sum >= term ? (sum - next) + term : (term - next) + sum;
        sum = next;
    }
    sum += correction;
    if (!std::isfinite(sum)) return static_cast<double>(std::sqrt(sum));
    if (sum == 0) return 0;
    double value = static_cast<double>(std::sqrt(sum));
    // For <=2^20 nonnegative terms, compensated summation leaves O(n*u^2)
    // error; subtraction/product rounding contributes O(u). 32*epsilon also
    // covers rounding both squared midpoints. If this interval touches a
    // rounding boundary, do not guess: compare exact integer squares instead.
    if (std::numeric_limits<long double>::digits >= 64 && n <= (size_t{1} << 20)) {
        long double below = std::nextafter(value, 0.0);
        long double above = std::nextafter(value, std::numeric_limits<double>::infinity());
        long double low = (below + value) * 0.5L;
        long double high = (above + value) * 0.5L;
        long double error = sum * (32 * std::numeric_limits<long double>::epsilon());
        if (sum - low * low > error && high * high - sum > error) return value;
    }
    return exact_distance(load, b, n);
}
}  // namespace detail

inline double distance(const float* a, const float* b, size_t n) {
    return detail::distance([a](size_t i) { return a[i]; }, b, n);
}

inline double distance_raw(const uint8_t* a, const float* b, size_t n) {
    return detail::distance([a](size_t i) {
        float value;
        std::memcpy(&value, a + i * sizeof(float), sizeof(value));
        return value;
    }, b, n);
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
