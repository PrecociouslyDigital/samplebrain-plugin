#include "disk_cache.h"

#include <algorithm>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace sb {

DiskCache::DiskCache(fs::path dir) : dir_(std::move(dir)) {
  std::error_code ec;
  fs::create_directories(dir_, ec);
}

std::optional<fs::path> DiskCache::find(std::string_view name) const {
  fs::path path = dir_ / name;
  std::error_code ec;
  if (!fs::is_regular_file(path, ec)) return std::nullopt;
  fs::last_write_time(path, fs::file_time_type::clock::now(), ec);
  return path;
}

bool DiskCache::store(
    std::string_view name,
    const std::function<bool(const fs::path &)> &write) const {
  thread_local std::mt19937_64 rng{std::random_device{}()};
  const fs::path temp =
      dir_ / (std::string(name) + ".tmp" + std::to_string(rng()));
  std::error_code ec;
  if (!write(temp)) {
    fs::remove(temp, ec);
    return false;
  }
  fs::rename(temp, dir_ / name, ec);
  if (ec) fs::remove(temp, ec);
  return !ec;
}

void DiskCache::trim(std::uintmax_t max_bytes) const {
  struct File {
    fs::path path;
    fs::file_time_type used;
    std::uintmax_t size;
  };
  std::vector<File> files;
  std::uintmax_t total = 0;
  std::error_code ec;
  for (const auto &entry : fs::directory_iterator(dir_, ec)) {
    if (!entry.is_regular_file(ec)) continue;
    File f{entry.path(), entry.last_write_time(ec), entry.file_size(ec)};
    total += f.size;
    files.push_back(std::move(f));
  }
  std::ranges::sort(files, {}, &File::used);
  for (const File &f : files) {
    if (total <= max_bytes) break;
    if (fs::remove(f.path, ec)) total -= f.size;
  }
}

fs::path user_cache_dir() {
  const char *home = std::getenv("HOME");
  return fs::path(home ? home : "/tmp") / "Library/Caches/samplebrain-plugin";
}

} // namespace sb
