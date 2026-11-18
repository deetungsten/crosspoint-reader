#include "KoboSyncConfig.h"

#if CROSSPOINT_KOBO_SYNC

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "KoboSyncToken.h"

namespace {

constexpr char B64_ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int b64Value(const char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

bool findKey(const std::string& json, const char* key, size_t& valueStart) {
  const std::string needle = std::string("\"") + key + "\"";
  size_t pos = json.find(needle);
  if (pos == std::string::npos) return false;
  pos = json.find(':', pos + needle.size());
  if (pos == std::string::npos) return false;
  pos++;
  while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n' || json[pos] == '\r')) pos++;
  valueStart = pos;
  return true;
}

bool parseNumber(const std::string& json, const char* key, double& out) {
  size_t pos = 0;
  if (!findKey(json, key, pos) || pos >= json.size()) return false;
  const char* begin = json.c_str() + pos;
  char* end = nullptr;
  const double value = strtod(begin, &end);
  if (end == begin || !std::isfinite(value)) return false;
  out = value;
  return true;
}

bool parseString(const std::string& json, const char* key, std::string& out, const size_t maxLen) {
  size_t pos = 0;
  if (!findKey(json, key, pos) || pos >= json.size() || json[pos] != '"') return false;
  pos++;
  std::string result;
  result.reserve(64);
  bool escaped = false;
  for (; pos < json.size(); ++pos) {
    const char c = json[pos];
    if (escaped) {
      switch (c) {
        case '"':
        case '\\':
        case '/':
          result.push_back(c);
          break;
        case 'n':
          result.push_back('\n');
          break;
        case 'r':
          result.push_back('\r');
          break;
        case 't':
          result.push_back('\t');
          break;
        default:
          // The store token is base64 and dots; no other escapes are expected.
          result.push_back(c);
          break;
      }
      escaped = false;
      continue;
    }
    if (c == '\\') {
      escaped = true;
      continue;
    }
    if (c == '"') {
      if (result.size() > maxLen) return false;
      out = std::move(result);
      return true;
    }
    result.push_back(c);
  }
  return false;
}

void appendNumber(std::string& out, const double value) {
  char buf[40];
  const double rounded = std::round(value);
  if (std::fabs(value - rounded) < 0.0005) {
    snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(rounded));
  } else {
    snprintf(buf, sizeof(buf), "%.6f", value);
  }
  out += buf;
}

void appendEscaped(std::string& out, const std::string& value) {
  out.push_back('"');
  for (const char c : value) {
    if (c == '"' || c == '\\') out.push_back('\\');
    out.push_back(c);
  }
  out.push_back('"');
}

}  // namespace

namespace kobo_base64 {

std::string encode(const std::string& input) {
  std::string out;
  out.reserve((input.size() + 2) / 3 * 4);
  size_t i = 0;
  while (i + 2 < input.size()) {
    const uint32_t triple = (static_cast<uint8_t>(input[i]) << 16) | (static_cast<uint8_t>(input[i + 1]) << 8) |
                            static_cast<uint8_t>(input[i + 2]);
    out.push_back(B64_ALPHABET[(triple >> 18) & 0x3F]);
    out.push_back(B64_ALPHABET[(triple >> 12) & 0x3F]);
    out.push_back(B64_ALPHABET[(triple >> 6) & 0x3F]);
    out.push_back(B64_ALPHABET[triple & 0x3F]);
    i += 3;
  }
  const size_t remaining = input.size() - i;
  if (remaining == 1) {
    const uint32_t triple = static_cast<uint8_t>(input[i]) << 16;
    out.push_back(B64_ALPHABET[(triple >> 18) & 0x3F]);
    out.push_back(B64_ALPHABET[(triple >> 12) & 0x3F]);
    out += "==";
  } else if (remaining == 2) {
    const uint32_t triple = (static_cast<uint8_t>(input[i]) << 16) | (static_cast<uint8_t>(input[i + 1]) << 8);
    out.push_back(B64_ALPHABET[(triple >> 18) & 0x3F]);
    out.push_back(B64_ALPHABET[(triple >> 12) & 0x3F]);
    out.push_back(B64_ALPHABET[(triple >> 6) & 0x3F]);
    out.push_back('=');
  }
  return out;
}

bool decode(const std::string& input, std::string& out) {
  out.clear();
  out.reserve(input.size() / 4 * 3 + 3);
  uint32_t buffer = 0;
  int bits = 0;
  for (const char c : input) {
    if (c == '=' || c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
    const int value = b64Value(c);
    if (value < 0) return false;
    buffer = (buffer << 6) | static_cast<uint32_t>(value);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<char>((buffer >> bits) & 0xFF));
    }
  }
  return true;
}

}  // namespace kobo_base64

KoboSyncToken KoboSyncToken::fromHeader(const std::string& value) {
  KoboSyncToken token;
  if (value.empty() || value.size() > MAX_ENCODED_LEN) return token;
  token.encoded = value;
  // Official Kobo store tokens are "<b64>.<b64>" and are replayed untouched.
  if (value.find('.') != std::string::npos) return token;

  std::string json;
  if (!kobo_base64::decode(value, json) || json.empty()) return token;
  if (json.find("\"version\"") == std::string::npos || json.find("\"data\"") == std::string::npos) return token;

  token.booksLastCreated = 0;
  token.booksLastModified = 0;
  token.archiveLastModified = 0;
  token.readingStateLastModified = 0;
  token.tagsLastModified = 0;
  const bool numbers = parseNumber(json, "books_last_created", token.booksLastCreated) &&
                       parseNumber(json, "books_last_modified", token.booksLastModified) &&
                       parseNumber(json, "archive_last_modified", token.archiveLastModified) &&
                       parseNumber(json, "reading_state_last_modified", token.readingStateLastModified) &&
                       parseNumber(json, "tags_last_modified", token.tagsLastModified);
  if (!numbers) return token;
  parseString(json, "raw_kobo_store_token", token.rawKoboStoreToken, MAX_RAW_STORE_TOKEN);
  token.decoded = true;
  return token;
}

std::string KoboSyncToken::build() const {
  std::string json;
  json.reserve(320);
  json += "{\"version\": \"1-1-0\", \"data\": {\"raw_kobo_store_token\": ";
  appendEscaped(json, rawKoboStoreToken);
  json += ", \"books_last_modified\": ";
  appendNumber(json, booksLastModified);
  json += ", \"books_last_created\": ";
  appendNumber(json, booksLastCreated);
  json += ", \"archive_last_modified\": ";
  appendNumber(json, archiveLastModified);
  json += ", \"reading_state_last_modified\": ";
  appendNumber(json, readingStateLastModified);
  json += ", \"tags_last_modified\": ";
  appendNumber(json, tagsLastModified);
  json += "}}";
  return kobo_base64::encode(json);
}

bool KoboSyncToken::valid() const {
  if (encoded.empty() || encoded.size() > MAX_ENCODED_LEN) return false;
  return decoded || encoded.find('.') != std::string::npos;
}

#endif  // CROSSPOINT_KOBO_SYNC
