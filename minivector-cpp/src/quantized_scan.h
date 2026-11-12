#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>
#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

// Experimental, rebuildable cache only. Original floats remain authoritative.
namespace quantized_scan {
struct Stats {
    bool enabled = std::getenv("MVD_Q8_STATS") != nullptr;
    std::atomic<uint64_t> examined{0}, rejected{0};
    ~Stats() {
        if (enabled) std::fprintf(stderr, "Q8_STATS examined=%llu rejected=%llu\n",
            (unsigned long long)examined.load(), (unsigned long long)rejected.load());
    }
};
inline Stats stats;

inline float maximum_abs_scalar(const float* values, size_t count) {
    float maximum = 0;
    for (size_t i = 0; i < count; ++i) {
        if (!std::isfinite(values[i])) return std::numeric_limits<float>::infinity();
        maximum = std::max(maximum, std::abs(values[i]));
    }
    return maximum;
}

inline uint32_t scalar(const int8_t* a, const int8_t* b, size_t n) {
    uint32_t sum = 0;
    for (size_t i = 0; i < n; ++i) {
        int d = int(a[i]) - int(b[i]);
        sum += uint32_t(d * d);
    }
    return sum;
}
#if defined(__x86_64__) || defined(__i386__)
__attribute__((target("avx2"))) inline float maximum_abs_avx2(
    const float* values, size_t count) {
    const __m256i magnitude_mask = _mm256_set1_epi32(0x7fffffff);
    __m256i maximum_bits = _mm256_setzero_si256();
    size_t i = 0;
    for (; i + 8 <= count; i += 8) {
        const __m256i bits = _mm256_castps_si256(_mm256_loadu_ps(values + i));
        maximum_bits = _mm256_max_epu32(maximum_bits, _mm256_and_si256(bits, magnitude_mask));
    }
    alignas(32) uint32_t lanes[8];
    _mm256_store_si256(reinterpret_cast<__m256i*>(lanes), maximum_bits);
    // Nonnegative binary32 bit patterns order finite values by magnitude.
    // Infinity and every NaN sort above the largest finite float.
    const uint32_t bits = *std::max_element(lanes, lanes + 8);
    if (bits >= 0x7f800000) return std::numeric_limits<float>::infinity();
    float maximum;
    std::memcpy(&maximum, &bits, sizeof(maximum));
    return std::max(maximum, maximum_abs_scalar(values + i, count - i));
}

__attribute__((target("avx2"))) inline __m128i encode_four(
    const float* values, __m256d inverse_step) {
    const __m256d scaled = _mm256_mul_pd(_mm256_cvtps_pd(_mm_loadu_ps(values)), inverse_step);
    const __m256d half = _mm256_or_pd(_mm256_set1_pd(0.5),
                                    _mm256_and_pd(scaled, _mm256_set1_pd(-0.0)));
    // Scaling by a power of two preserves binary32 precision in binary64.
    // Rounding the sum in binary64 cannot move a non-tie across an integer.
    return _mm256_cvttpd_epi32(_mm256_add_pd(scaled, half));
}

__attribute__((target("avx2"))) inline size_t encode_avx2(
    const float* values, int8_t* codes, size_t count, double step) {
    const __m256d inverse_step = _mm256_set1_pd(1.0 / step);
    size_t i = 0;
    for (; i + 16 <= count; i += 16) {
        const __m128i first = _mm_packs_epi32(
            encode_four(values + i, inverse_step), encode_four(values + i + 4, inverse_step));
        const __m128i second = _mm_packs_epi32(
            encode_four(values + i + 8, inverse_step), encode_four(values + i + 12, inverse_step));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(codes + i), _mm_packs_epi16(first, second));
    }
    return i;
}

__attribute__((target("avx2"))) inline __m256i square16(const int8_t* a, const int8_t* b) {
    __m256i x = _mm256_cvtepi8_epi16(_mm_loadu_si128((const __m128i*)a));
    __m256i y = _mm256_cvtepi8_epi16(_mm_loadu_si128((const __m128i*)b));
    __m256i d = _mm256_sub_epi16(x, y);
    return _mm256_madd_epi16(d, d);
}
__attribute__((target("avx2"))) inline uint32_t avx2(
    const int8_t* a, const int8_t* b, size_t n) {
    __m256i sum = _mm256_setzero_si256();
    __m256i sum1 = sum, sum2 = sum, sum3 = sum;
    size_t i = 0;
    for (; i + 64 <= n; i += 64) {
        sum = _mm256_add_epi32(sum, square16(a+i,b+i));
        sum1 = _mm256_add_epi32(sum1, square16(a+i+16,b+i+16));
        sum2 = _mm256_add_epi32(sum2, square16(a+i+32,b+i+32));
        sum3 = _mm256_add_epi32(sum3, square16(a+i+48,b+i+48));
    }
    sum = _mm256_add_epi32(_mm256_add_epi32(sum,sum1), _mm256_add_epi32(sum2,sum3));
    for (; i + 16 <= n; i += 16) {
        sum = _mm256_add_epi32(sum, square16(a+i,b+i));
    }
    alignas(32) uint32_t lanes[8];
    _mm256_store_si256((__m256i*)lanes, sum);
    uint32_t result = scalar(a+i, b+i, n-i);
    for (uint32_t lane : lanes) result += lane;
    return result;
}
#endif

struct Cache {
    size_t dim = 0;
    double scale = 0;
    std::vector<int8_t> codes;
    void build(const std::vector<float>& values, size_t dimension, bool allow_avx2 = true) {
        if (dimension == 0 || dimension > 4096 || values.empty() ||
            values.size() % dimension) return;
#if defined(__x86_64__) || defined(__i386__)
        const bool use_avx2 = allow_avx2 && __builtin_cpu_supports("avx2");
        const double maximum = use_avx2 ? maximum_abs_avx2(values.data(), values.size())
                                        : maximum_abs_scalar(values.data(), values.size());
#else
        (void)allow_avx2;
        const double maximum = maximum_abs_scalar(values.data(), values.size());
#endif
        if (!std::isfinite(maximum)) return;
        int exponent = 0;
        std::frexp(maximum, &exponent);
        double step = maximum == 0 ? 1.0 : std::ldexp(1.0, exponent-7);
        if (maximum > 127.0 * step) step *= 2;
        std::vector<int8_t> encoded(values.size());
        size_t i = 0;
#if defined(__x86_64__) || defined(__i386__)
        if (use_avx2) i = encode_avx2(values.data(), encoded.data(), values.size(), step);
#endif
        for (; i < values.size(); ++i)
            encoded[i] = int8_t(std::round(double(values[i]) / step));
        dim = dimension;
        scale = step;
        codes = std::move(encoded);
    }
};

struct Query {
    const Cache* cache = nullptr;
    std::vector<int8_t> codes;
    bool simd = false;
    double previous = -1, cutoff = 0;
    Query(const Cache& data, const std::vector<float>& q, bool use_simd, bool asc) {
        if (!asc || !data.scale || data.dim != q.size()) return;
        codes.resize(q.size());
        for (size_t i = 0; i < q.size(); ++i) {
            double v = double(q[i]) / data.scale;
            if (!std::isfinite(v) || std::abs(v) > 127.0) { codes.clear(); return; }
            codes[i] = int8_t(std::round(v));
        }
        cache = &data;
        simd = use_simd;
    }
    bool rejects(size_t row, double worst) {
        if (!cache || !std::isfinite(worst)) return false;
        if (worst != previous) {
            // Each coordinate has <= scale/2 quantization error. Triangle
            // inequality: ||code(x)-code(q)|| <= d(x,q)/scale + sqrt(dim).
            // Power-of-two scaling of f32 inputs is exact in binary64;
            // round-to-integer is exact here (|code| <= 127). Dimension cap
            // keeps integer sum <= 4096*254^2 < 2^31. Inflate the threshold
            // for sqrt, arithmetic and the authoritative compensated score;
            // 1e-9 exceeds 32*epsilon*(4096+8). Never prune equality.
            double radius = (worst/cache->scale + std::sqrt(double(cache->dim))) * (1.0+1e-9);
            cutoff = radius * radius * (1.0+1e-9);
            previous = worst;
        }
        const int8_t* p = cache->codes.data() + row * cache->dim;
        uint32_t sq;
#if defined(__x86_64__) || defined(__i386__)
        sq = simd ? avx2(p, codes.data(), cache->dim) : scalar(p, codes.data(), cache->dim);
#else
        sq = scalar(p, codes.data(), cache->dim);
#endif
        bool reject = double(sq) > cutoff;
        if (stats.enabled) {
            stats.examined.fetch_add(1, std::memory_order_relaxed);
            if (reject) stats.rejected.fetch_add(1, std::memory_order_relaxed);
        }
        return reject;
    }
};
} // namespace quantized_scan
