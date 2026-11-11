#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

inline uint32_t scalar(const int8_t* a, const int8_t* b, size_t n) {
    uint32_t sum = 0;
    for (size_t i = 0; i < n; ++i) {
        int d = int(a[i]) - int(b[i]);
        sum += uint32_t(d * d);
    }
    return sum;
}
#if defined(__x86_64__) || defined(__i386__)
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
    void build(const std::vector<float>& values, size_t dimension) {
        if (dimension == 0 || dimension > 4096 || values.empty() ||
            values.size() % dimension) return;
        double maximum = 0;
        for (float x : values) {
            if (!std::isfinite(x)) return;
            maximum = std::max(maximum, std::abs(double(x)));
        }
        int exponent = 0;
        std::frexp(maximum, &exponent);
        double step = maximum == 0 ? 1.0 : std::ldexp(1.0, exponent-7);
        if (maximum > 127.0 * step) step *= 2;
        std::vector<int8_t> encoded(values.size());
        for (size_t i = 0; i < values.size(); ++i)
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
