#include <atomic>
#include <fstream>
#include <map>
#include <random>
#include <thread>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_adapters.hpp>
#include <catch2/generators/catch_generators_random.hpp>

#include "cache/disk_cache.h"
#include "cache/memo_cache.h"
#include "fixtures.h"

using namespace sb;

namespace {
struct NoError {};
using Cache = MemoCache<int, long, NoError>;

long direct(int key) { return long(key) * key + 1; }
} // namespace

TEST_CASE("memo cache results equal computing the key directly") {
  const auto seed = GENERATE(take(50, random(0u, 1u << 30)));
  std::mt19937 rng(seed);
  Cache cache;
  std::map<int, std::vector<Cache::Value>> held;

  for (int step = 0; step < 300; ++step) {
    const int key = std::uniform_int_distribution(0, 9)(rng);
    if (std::bernoulli_distribution(0.3)(rng) && !held[key].empty()) {
      held[key].pop_back();
      continue;
    }
    auto result = cache.get(key, [&]() -> Cache::Result {
      return std::make_shared<const long>(direct(key));
    });
    REQUIRE(result);
    REQUIRE(**result == direct(key));
    if (!held[key].empty()) REQUIRE(*result == held[key].front());
    held[key].push_back(*result);
  }
}

TEST_CASE("memo cache keeps nothing once it's let go") {
  Cache cache;
  std::weak_ptr<const long> weak;
  {
    auto v = cache.get(1, [] { return Cache::Result(std::make_shared<const long>(1)); });
    weak = *v;
    CHECK(cache.find(1) == *v);
  }
  CHECK(weak.expired());
  CHECK(cache.find(1) == nullptr);
}

TEST_CASE("concurrent requests for one key compute it once") {
  Cache cache;
  std::atomic<int> computed = 0;
  std::vector<Cache::Value> results(8);
  {
    std::vector<std::jthread> threads;
    for (auto &r : results)
      threads.emplace_back([&] {
        r = *cache.get(5, [&] {
          ++computed;
          std::this_thread::sleep_for(std::chrono::milliseconds(20));
          return Cache::Result(std::make_shared<const long>(direct(5)));
        });
      });
  }
  CHECK(computed == 1);
  for (const auto &r : results) CHECK(r == results.front());
}

TEST_CASE("a failed computation is retried by the next request") {
  using Failing = MemoCache<int, long, int>;
  Failing cache;
  CHECK(!cache.get(1, [] { return Failing::Result(std::unexpected(7)); }));
  CHECK(cache.get(1, [] { return Failing::Result(std::make_shared<const long>(2)); }));
  CHECK_THROWS(cache.get(2, []() -> Failing::Result { throw std::runtime_error("x"); }));
  CHECK(cache.get(2, [] { return Failing::Result(std::make_shared<const long>(3)); }));
}

TEST_CASE("disk cache trims the least recently used files") {
  const auto dir = test::temp_path("disk-cache");
  std::filesystem::remove_all(dir);
  DiskCache disk(dir);
  auto write_bytes = [](std::size_t n) {
    return [n](const std::filesystem::path &p) {
      std::ofstream(p) << std::string(n, 'x');
      return true;
    };
  };
  using namespace std::chrono_literals;
  const auto now = std::filesystem::file_time_type::clock::now();
  for (int i = 0; i < 4; ++i) {
    REQUIRE(disk.store(std::to_string(i), write_bytes(100)));
    std::filesystem::last_write_time(dir / std::to_string(i), now - (10s - i * 1s));
  }
  // Using file 0 makes it the most recent.
  REQUIRE(disk.find("0"));
  disk.trim(250);
  CHECK(disk.find("0"));
  CHECK(!disk.find("1"));
  CHECK(!disk.find("2"));
  CHECK(disk.find("3"));
  std::filesystem::remove_all(dir);
}
