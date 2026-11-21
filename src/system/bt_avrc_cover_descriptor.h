#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Lightweight, allocation-free BIP image-properties parser, used only after the
// properties OBEX operation finishes. Select JPEG close to 460x460 if the phone
// advertises that representation, otherwise its bounded-size native JPEG.
// Never assume that every phone supports an arbitrary requested pixel size.
namespace bt_avrc_cover_descriptor {

struct Selection {
    uint16_t width = 0;
    uint16_t height = 0;
    bool variant = false;
};

inline bool attribute(const char *begin, const char *end, const char *key,
                      char *out, size_t cap)
{
    if (!begin || !end || !key || !out || cap == 0U) return false;
    const size_t klen = strlen(key);
    for (const char *p = begin; p + klen + 2 < end; ++p) {
        if (memcmp(p, key, klen) != 0 || p[klen] != '=') continue;
        const char quote = p[klen + 1];
        if (quote != '"' && quote != '\'') continue;
        const char *v = p + klen + 2;
        const char *finish = v;
        while (finish < end && *finish != quote) ++finish;
        const size_t n = static_cast<size_t>(finish - v);
        if (finish >= end || n == 0U || n >= cap) return false;
        memcpy(out, v, n);
        out[n] = '\0';
        return true;
    }
    return false;
}

inline bool parse_pixels(const char *s, unsigned *w1, unsigned *h1,
                         unsigned *w2, unsigned *h2)
{
    if (!s || !w1 || !h1 || !w2 || !h2) return false;
    char end = 0;
    const int n = sscanf(s, "%u*%u-%u*%u%c", w1, h1, w2, h2, &end);
    if (n == 4) {
        if (*w1 > *w2 || *h1 > *h2) return false;
    } else if (n != 2) {
        const int exact = sscanf(s, "%u*%u%c", w1, h1, &end);
        if (exact != 2) return false;
        *w2 = *w1; *h2 = *h1;
    } else {
        // Parsing a fixed pixel pair with the range format returns 2.
        *w2 = *w1; *h2 = *h1;
    }
    return *w1 >= 64 && *h1 >= 64 && *w2 <= 4096 && *h2 <= 4096;
}

inline bool select(const char *xml, size_t length, size_t max_jpeg_bytes,
                   Selection *result)
{
    if (!xml || !result || length == 0U || length > 2048U) return false;
    if (!strstr(xml, "<image-properties")) return false;

    Selection native = {};
    bool have_native = false;
    const char *end = xml + length;
    for (int group = 0; group < 2; ++group) {
        const char *tag = group == 0 ? "<variant" : "<native";
        for (const char *p = xml; (p = strstr(p, tag)) != nullptr; ++p) {
            if (p >= end) break;
            const char *q = strchr(p, '>');
            if (!q || q >= end) break;
            char encoding[20] = {}, pixel[64] = {}, size[24] = {};
            if (!attribute(p, q, "encoding", encoding, sizeof(encoding)) ||
                strcmp(encoding, "JPEG") != 0 ||
                !attribute(p, q, "pixel", pixel, sizeof(pixel))) continue;
            unsigned a = 0, b = 0, c = 0, d = 0;
            if (!parse_pixels(pixel, &a, &b, &c, &d)) continue;
            if (group == 0) {
                const bool fits460 = a <= 460 && b <= 460 &&
                                     c >= 460 && d >= 460;
                if (fits460) {
                    *result = {460, 460, true};
                    return true;
                }
                // A fixed representation close to screen size can also work.
                if (a == c && b == d && a >= 320 && b >= 320 &&
                    a <= 600 && b <= 600) {
                    *result = {static_cast<uint16_t>(a),
                               static_cast<uint16_t>(b), true};
                    return true;
                }
            } else if (a == c && b == d) {
                // If native JPEG bytes exceed the PSRAM budget, never request
                // it without a usable resized variant; use thumbnail instead.
                bool size_ok = false;
                if (attribute(p, q, "size", size, sizeof(size))) {
                    char *tail = nullptr;
                    const unsigned long n = strtoul(size, &tail, 10);
                    size_ok = tail && *tail == '\0' && n >= 32UL &&
                              n <= max_jpeg_bytes;
                }
                if (size_ok && a >= 260 && b >= 260) {
                    native = {static_cast<uint16_t>(a),
                              static_cast<uint16_t>(b), false};
                    have_native = true;
                }
            }
        }
    }
    if (have_native) *result = native;
    return have_native;
}

inline size_t make_xml(const Selection &chosen, char *out, size_t cap)
{
    if (!out || cap == 0U || chosen.width == 0 || chosen.height == 0) return 0U;
    const int n = snprintf(out, cap,
        "<image-descriptor version=\"1.0\"><image encoding=\"JPEG\" pixel=\"%u*%u\"/></image-descriptor>",
        static_cast<unsigned>(chosen.width),
        static_cast<unsigned>(chosen.height));
    return n > 0 && static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : 0U;
}

} // namespace bt_avrc_cover_descriptor
