#pragma once

// Hands immutable values to audio threads without locks or frees there.
//
// The message thread publishes a new value by swapping a pointer. The old
// value is kept until every audio thread that might have been reading it
// has finished its callback, then freed on the message thread.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace sb {

// One per audio callback that reads published values. Its count is odd
// while a Reading is alive.
class ReaderEpoch {
public:
  class Reading {
  public:
    explicit Reading(ReaderEpoch &epoch) noexcept [[clang::nonblocking]]
        : epoch_(epoch) {
      epoch_.count_.fetch_add(1);
    }
    ~Reading() noexcept [[clang::nonblocking]] { epoch_.count_.fetch_add(1); }
    Reading(const Reading &) = delete;
    Reading &operator=(const Reading &) = delete;

  private:
    ReaderEpoch &epoch_;
  };

  std::uint64_t count() const { return count_.load(); }

private:
  std::atomic<std::uint64_t> count_ = 0;
};

// The readers that published values must wait for. Used on the message
// thread, which adds a reader before its callback can run and removes it
// once the callback can no longer run.
class ReaderRegistry {
public:
  void add(ReaderEpoch *reader) {
    std::lock_guard lock(mutex_);
    readers_.push_back(reader);
  }

  void remove(ReaderEpoch *reader) {
    std::lock_guard lock(mutex_);
    std::erase(readers_, reader);
  }

  // The readers that are mid-read now.
  using Snapshot = std::vector<std::pair<const ReaderEpoch *, std::uint64_t>>;
  Snapshot readers_reading() const {
    std::lock_guard lock(mutex_);
    Snapshot reading;
    for (const ReaderEpoch *r : readers_)
      if (std::uint64_t n = r->count(); n % 2 == 1) reading.emplace_back(r, n);
    return reading;
  }

  // Whether every reader in the snapshot has finished that read.
  bool finished(const Snapshot &snapshot) const {
    std::lock_guard lock(mutex_);
    return std::ranges::all_of(snapshot, [&](const auto &entry) {
      const auto [reader, count] = entry;
      return std::ranges::find(readers_, reader) == readers_.end() ||
             reader->count() != count;
    });
  }

private:
  mutable std::mutex mutex_;
  std::vector<const ReaderEpoch *> readers_;
};

template <class T> class Published {
public:
  explicit Published(ReaderRegistry &registry) : registry_(registry) {}
  Published(const Published &) = delete;
  Published &operator=(const Published &) = delete;

  // Message thread.
  void publish(std::shared_ptr<const T> value) {
    current_.store(value.get());
    retired_.push_back({std::exchange(owner_, std::move(value)),
                        registry_.readers_reading()});
    collect();
  }

  // Message thread: frees old values that no reader can still see.
  void collect() {
    std::erase_if(retired_, [&](const Retired &r) {
      return registry_.finished(r.readers);
    });
  }

  // Message thread.
  const std::shared_ptr<const T> &get() const { return owner_; }
  bool has_retired() const { return !retired_.empty(); }

  // Audio thread. Valid until the Reading ends.
  const T *read(const ReaderEpoch::Reading &) const noexcept
      [[clang::nonblocking]] {
    return current_.load();
  }

private:
  struct Retired {
    std::shared_ptr<const T> value;
    ReaderRegistry::Snapshot readers;
  };

  ReaderRegistry &registry_;
  std::atomic<const T *> current_ = nullptr;
  std::shared_ptr<const T> owner_;
  std::vector<Retired> retired_;
};

} // namespace sb
