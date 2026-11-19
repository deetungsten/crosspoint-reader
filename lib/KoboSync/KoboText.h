#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "../Utf8/Utf8.h"

// Kobo titles can arrive as UTF-8 typographic punctuation or as raw CP1252
// bytes from an older or misconfigured feed. The current built-in UI faces
// cover the UTF-8 forms, but the C1/CP1252 forms are absent. Fold both at the
// feed/metadata boundary so a shelf title never depends on the face or on the
// encoding calibre or a proxy produced.
namespace kobo {

namespace detail {

inline const char* punctuationReplacement(const uint32_t cp, size_t& len) {
  switch (cp) {
    case 0x2018:
    case 0x2019:
    case 0x201A:
    case 0x201B:
      len = 1;
      return "'";
    case 0x201C:
    case 0x201D:
    case 0x201E:
    case 0x201F:
      len = 1;
      return "\"";
    case 0x2039:
    case 0x203A:
      len = 1;
      return "'";
    case 0x2013:
    case 0x2014:
    case 0x2015:
      len = 1;
      return "-";
    case 0x2026:
      len = 3;
      return "...";
    case 0x00A0:
      len = 1;
      return " ";
    // The same characters arrived as CP1252 when a feed was not UTF-8.
    case 0x0085:
      len = 3;
      return "...";
    case 0x0091:
    case 0x0092:
      len = 1;
      return "'";
    case 0x0093:
    case 0x0094:
      len = 1;
      return "\"";
    case 0x0096:
    case 0x0097:
      len = 1;
      return "-";
    default:
      return nullptr;
  }
}

inline uint32_t cp1252Codepoint(const unsigned char byte) {
  switch (byte) {
    case 0x82:
      return 0x201A;
    case 0x84:
      return 0x201E;
    case 0x85:
      return 0x2026;
    case 0x8B:
      return 0x2039;
    case 0x91:
      return 0x2018;
    case 0x92:
      return 0x2019;
    case 0x93:
      return 0x201C;
    case 0x94:
      return 0x201D;
    case 0x96:
      return 0x2013;
    case 0x97:
      return 0x2014;
    case 0x9B:
      return 0x203A;
    case 0xA0:
      return 0x00A0;
    default:
      return 0;
  }
}

}  // namespace detail

// Copy at most `cap` bytes of `src` into `dst`, replacing the typographic
// punctuation above. `src` must be NUL-terminated; `len` bounds the copy.
// The result is not NUL-terminated; copyNormalizedField() does that for
// fixed-size fields.
inline size_t normalizeText(char* dst, const size_t cap, const char* src, const size_t len) {
  if (dst == nullptr || cap == 0 || src == nullptr) return 0;

  const char* p = src;
  const char* const end = src + len;
  size_t written = 0;

  while (p < end && *p != '\0') {
    const char* const start = p;
    const auto lead = static_cast<unsigned char>(*p);
    const char* replacement = nullptr;
    size_t replacementLen = 0;

    if (lead >= 0x80 && lead <= 0xA0) {
      // A raw CP1252 punctuation byte. Valid UTF-8 never starts a codepoint
      // with a continuation-range byte; 0xA0 is CP1252 NBSP.
      const uint32_t cp = detail::cp1252Codepoint(lead);
      if (cp != 0) replacement = detail::punctuationReplacement(cp, replacementLen);
      p++;
    } else if (lead < 0x80) {
      p++;
    } else {
      const auto* cursor = reinterpret_cast<const unsigned char*>(p);
      const uint32_t cp = utf8NextCodepoint(&cursor);
      p = reinterpret_cast<const char*>(cursor);
      if (cp != REPLACEMENT_GLYPH) replacement = detail::punctuationReplacement(cp, replacementLen);
    }

    if (replacement == nullptr) {
      const size_t rawLen = static_cast<size_t>(p - start);
      if (written + rawLen > cap) break;
      memcpy(dst + written, start, rawLen);
      written += rawLen;
    } else {
      if (written + replacementLen > cap) break;
      memcpy(dst + written, replacement, replacementLen);
      written += replacementLen;
    }
  }

  return written;
}

// NUL-terminated fixed-field variant used by the feed parser and metadata fetch.
inline void copyNormalizedField(char* dst, const size_t cap, const char* src, const size_t len) {
  if (dst == nullptr || cap == 0) return;
  const size_t written = normalizeText(dst, cap - 1, src, len);
  dst[written] = '\0';
}

}  // namespace kobo
