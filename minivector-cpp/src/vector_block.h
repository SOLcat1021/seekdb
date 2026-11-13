#pragma once

#include "util.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

namespace vector_block {

constexpr size_t dictionary_size = 15;
constexpr uint8_t escape = 15;

// A packed vector stores three literal bytes per coordinate, two high-byte
// dictionary codes per byte, then the literal high bytes of escaped codes.
inline size_t packed_prefix(size_t dimensions) {
    return 3 * dimensions + (dimensions + 1) / 2;
}

struct View {
    const uint8_t* data = nullptr;
    const uint8_t* dictionary = nullptr;
    size_t size = 0;
};

#if defined(__x86_64__) || defined(__i386__)
__attribute__((target("avx2"))) inline __m256 unpack_eight(
    const uint8_t* low, __m128i high) {
    const auto lanes = _mm_setr_epi8(0, 1, 2, -1, 3, 4, 5, -1, 6, 7, 8, -1, 9, 10, 11, -1);
    const auto first = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(low)), lanes);
    const auto second = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(low + 12)), lanes);
    const auto payload = _mm256_set_m128i(second, first);
    const auto exponents = _mm256_slli_epi32(_mm256_cvtepu8_epi32(high), 24);
    return _mm256_castsi256_ps(_mm256_or_si256(payload, exponents));
}

// Screen dictionary-only rows in registers. The caller supplies the same
// conservative cutoff as the ordinary binary32 distance screen.
__attribute__((target("avx2,fma"))) inline bool screen_dense(
    View packed, const float* query, size_t dimensions, double cutoff) {
    const auto mask = _mm_set1_epi8(15);
    const auto dictionary = _mm_loadu_si128(reinterpret_cast<const __m128i*>(packed.dictionary));
    const auto* codes = packed.data + 3 * dimensions;
    auto first = _mm256_setzero_ps();
    auto second = first;
    for (size_t i = 0; i < dimensions; i += 16) {
        const auto nibbles = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(codes + i / 2));
        const auto indices = _mm_unpacklo_epi8(_mm_and_si128(nibbles, mask),
                                              _mm_and_si128(_mm_srli_epi16(nibbles, 4), mask));
        if (_mm_movemask_epi8(_mm_cmpeq_epi8(indices, mask))) return false;
        const auto high = _mm_shuffle_epi8(dictionary, indices);
        const auto x = _mm256_sub_ps(unpack_eight(packed.data + 3 * i, high), _mm256_loadu_ps(query + i));
        const auto y = _mm256_sub_ps(unpack_eight(packed.data + 3 * i + 24, _mm_srli_si128(high, 8)),
                                     _mm256_loadu_ps(query + i + 8));
        first = _mm256_fmadd_ps(x, x, first);
        second = _mm256_fmadd_ps(y, y, second);
    }
    const auto lanes = _mm256_add_ps(first, second);
    auto sum = _mm_add_ps(_mm256_castps256_ps128(lanes), _mm256_extractf128_ps(lanes, 1));
    sum = _mm_hadd_ps(sum, sum);
    sum = _mm_hadd_ps(sum, sum);
    const float squared = _mm_cvtss_f32(sum);
    return std::isfinite(squared) && squared > cutoff;
}

__attribute__((target("ssse3"))) inline size_t decode_dense(View packed, size_t dimensions,
                                                             uint8_t* output) {
    const auto mask = _mm_set1_epi8(15);
    const auto lanes = _mm_setr_epi8(0, 1, 2, -1, 3, 4, 5, -1, 6, 7, 8, -1, 9, 10, 11, -1);
    // A valid packed vector has data immediately after the 15-byte dictionary.
    const auto dictionary = _mm_loadu_si128(reinterpret_cast<const __m128i*>(packed.dictionary));
    const auto* codes = packed.data + 3 * dimensions;
    size_t i = 0;
    for (; i + 16 <= dimensions; i += 16) {
        const auto nibbles = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(codes + i / 2));
        const auto indices = _mm_unpacklo_epi8(_mm_and_si128(nibbles, mask),
            _mm_and_si128(_mm_srli_epi16(nibbles, 4), mask));
        if (_mm_movemask_epi8(_mm_cmpeq_epi8(indices, mask))) break;
        auto high = _mm_shuffle_epi8(dictionary, indices);
        for (size_t group = 0; group < 4; ++group) {
            const auto* low = packed.data + 3 * i + 12 * group;
            const auto payload = _mm_shuffle_epi8(
                _mm_loadu_si128(reinterpret_cast<const __m128i*>(low)), lanes);
            const auto exponents = _mm_slli_epi32(_mm_unpacklo_epi16(
                _mm_unpacklo_epi8(high, _mm_setzero_si128()), _mm_setzero_si128()), 24);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(output + 4 * i + 16 * group),
                             _mm_or_si128(payload, exponents));
            high = _mm_srli_si128(high, 4);
        }
    }
    return i;
}
#endif

inline void decode(View packed, size_t dimensions, uint8_t* output) {
    const size_t prefix = packed_prefix(dimensions);
    if (packed.size < prefix || packed.size - prefix > dimensions) {
        fail_internal("storage corrupted: invalid packed vector size");
    }
    const uint8_t* codes = packed.data + 3 * dimensions;
    const uint8_t* literal = packed.data + prefix;
    const uint8_t* end = packed.data + packed.size;
    size_t start = 0;
#if defined(__x86_64__) || defined(__i386__)
    static const bool has_ssse3 = __builtin_cpu_supports("ssse3");
    if (literal == end && has_ssse3 && dimensions >= 16) {
        start = decode_dense(packed, dimensions, output);
        output += 4 * start;
        codes += start / 2;
    }
#endif
    for (size_t i = start; i < dimensions; i += 2) {
        const uint8_t pair = *codes++;
        const size_t count = dimensions - i >= 2 ? 2 : 1;
        for (size_t lane = 0; lane < count; ++lane) {
            const uint8_t code = (pair >> (4 * lane)) & 15u;
            if (code == escape && literal == end) {
                fail_internal("storage corrupted: truncated vector escape");
            }
            const uint8_t high = code == escape ? *literal++ : packed.dictionary[code];
            std::memcpy(output, packed.data + 3 * (i + lane), 3);
            output[3] = high;
            output += 4;
        }
    }
    if (literal != end) {
        fail_internal("storage corrupted: trailing vector escapes");
    }
}

}  // namespace vector_block
