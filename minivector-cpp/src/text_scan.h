#pragma once

#include <cstddef>
#if defined(__SSE2__)
#include <emmintrin.h>
#endif

// Copy ordinary string bytes in spans; existing parsers still handle escapes,
// doubled SQL quotes, JSON control characters and Unicode escapes.
inline size_t plain_text_span(const char* text, size_t length, char quote, bool json) {
    size_t i = 0;
#if defined(__SSE2__)
    const __m128i q = _mm_set1_epi8(quote);
    const __m128i slash = _mm_set1_epi8('\\');
    for (; i + 16 <= length; i += 16) {
        const __m128i bytes = _mm_loadu_si128(reinterpret_cast<const __m128i*>(text + i));
        __m128i stop = _mm_or_si128(_mm_cmpeq_epi8(bytes, q), _mm_cmpeq_epi8(bytes, slash));
        if (json) stop = _mm_or_si128(stop, _mm_cmplt_epi8(bytes, _mm_set1_epi8(32)));
        const unsigned mask = static_cast<unsigned>(_mm_movemask_epi8(stop));
        if (mask) return i + static_cast<size_t>(__builtin_ctz(mask));
    }
#endif
    while (i < length && text[i] != quote && text[i] != '\\' &&
           (!json || static_cast<unsigned char>(text[i]) >= 32)) ++i;
    return i;
}
