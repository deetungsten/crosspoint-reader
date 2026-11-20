#pragma once

#include <string>

namespace kobo {

// A catalog row needs a transfer unless it was removed or its recorded local
// file is still on SD. The caller re-checks the file at download time because
// the catalog mapping can be stale.
inline bool needsDownload(const bool removed, const bool hasLocalPath, const bool localFileExists) {
  return !removed && !(hasLocalPath && localFileExists);
}

// Keeps a second book with the same "Author - Title.epub" name from replacing
// the first; the content-id suffix is what the path link uses to tell them
// apart.
inline std::string withContentSuffix(std::string path, const std::string& contentId) {
  const std::string suffix = " [" + contentId.substr(0, 8) + "]";
  const size_t dot = path.rfind('.');
  if (dot == std::string::npos) return path + suffix;
  return path.substr(0, dot) + suffix + path.substr(dot);
}

}  // namespace kobo
