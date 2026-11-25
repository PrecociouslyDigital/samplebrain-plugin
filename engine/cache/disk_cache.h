#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string_view>

namespace sb {

// A folder of files named by key. A file's modification time is when it
// was last used, and trim() deletes the least recently used first.
class DiskCache {
public:
  explicit DiskCache(std::filesystem::path dir);

  // The file for name, marked as just used, if it's there.
  std::optional<std::filesystem::path> find(std::string_view name) const;

  // Calls write with a temporary path, then moves the file into place, so
  // readers never see half a file. Returns whether write succeeded.
  bool store(std::string_view name,
             const std::function<bool(const std::filesystem::path &)> &write) const;

  // Deletes least recently used files until the folder fits in max_bytes.
  void trim(std::uintmax_t max_bytes) const;

  const std::filesystem::path &dir() const { return dir_; }

private:
  std::filesystem::path dir_;
};

// ~/Library/Caches/samplebrain-plugin
std::filesystem::path user_cache_dir();

// Brains and renders past this are trimmed at startup.
inline constexpr std::uintmax_t disk_cache_budget = 10ull << 30;

} // namespace sb
