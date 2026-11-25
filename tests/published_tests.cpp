#include <atomic>
#include <chrono>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "realtime/published.h"

using namespace sb;

namespace {

constexpr std::uint32_t alive = 0xC0FFEE;

struct Value {
  explicit Value(int n) : n(n), twice(2 * n) {}
  ~Value() { magic = 0; }
  std::uint32_t magic = alive;
  int n, twice;
};

} // namespace

TEST_CASE("readers never see a published value freed") {
  ReaderRegistry registry;
  Published<Value> published(registry);
  std::atomic<bool> stop = false;
  std::atomic<int> bad = 0, reads = 0;
  int last = 0;

  std::vector<std::unique_ptr<ReaderEpoch>> epochs;
  for (int i = 0; i < 3; ++i) {
    epochs.push_back(std::make_unique<ReaderEpoch>());
    registry.add(epochs.back().get());
  }

  {
    std::vector<std::jthread> readers;
    for (auto &epoch : epochs)
      readers.emplace_back([&, e = epoch.get()] {
        while (!stop) {
          ReaderEpoch::Reading reading(*e);
          if (const Value *v = published.read(reading)) {
            if (v->magic != alive || v->twice != 2 * v->n) ++bad;
            ++reads;
          }
        }
      });

    // Keep publishing until the readers have overlapped plenty of writes,
    // however late their threads start.
    const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    int i = 0;
    for (; (i < 5000 || reads < 10000) && std::chrono::steady_clock::now() < give_up; ++i)
      published.publish(std::make_shared<const Value>(i));
    last = i - 1;
    stop = true;
  }

  CHECK(reads >= 10000);
  CHECK(bad == 0);
  for (auto &epoch : epochs) registry.remove(epoch.get());
  published.collect();
  CHECK(!published.has_retired());
  CHECK(published.get()->n == last);
}

TEST_CASE("an old value waits for a reader that's mid-read") {
  ReaderRegistry registry;
  Published<Value> published(registry);
  ReaderEpoch epoch;
  registry.add(&epoch);

  published.publish(std::make_shared<const Value>(1));
  std::weak_ptr<const Value> first = published.get();
  {
    ReaderEpoch::Reading reading(epoch);
    CHECK(published.read(reading)->n == 1);
    published.publish(std::make_shared<const Value>(2));
    CHECK(!first.expired());
  }
  published.collect();
  CHECK(first.expired());
}
