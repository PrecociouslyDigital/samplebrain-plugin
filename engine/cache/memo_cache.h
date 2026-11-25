#pragma once

#include <expected>
#include <future>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <variant>

namespace sb {

// Shares computed values by key while anything holds them.
//
// Asking for a key that's in use returns that same object. Asking for one
// that's being computed waits for that computation. Otherwise the caller
// computes it. Nothing is kept once its last holder lets go.
template <class Key, class T, class E> class MemoCache {
public:
  using Value = std::shared_ptr<const T>;
  using Result = std::expected<Value, E>;

  // make() returns a Result. Errors aren't remembered, and neither are
  // exceptions, which reach every caller waiting on that computation.
  template <class Make> Result get(const Key &key, Make &&make) {
    std::unique_lock lock(mutex_);
    std::erase_if(entries_, [](const auto &e) { return is_dead(e.second); });
    if (auto it = entries_.find(key); it != entries_.end()) {
      if (auto *pending = std::get_if<Pending>(&it->second)) {
        Pending wait = *pending;
        lock.unlock();
        return wait.get();
      }
      if (Value live = std::get<Live>(it->second).lock()) return live;
    }
    std::promise<Result> promise;
    entries_.insert_or_assign(key, promise.get_future().share());
    lock.unlock();

    try {
      Result result = make();
      lock.lock();
      if (result)
        entries_.insert_or_assign(key, Live(*result));
      else
        entries_.erase(key);
      lock.unlock();
      promise.set_value(result);
      return result;
    } catch (...) {
      if (!lock.owns_lock()) lock.lock();
      entries_.erase(key);
      lock.unlock();
      promise.set_exception(std::current_exception());
      throw;
    }
  }

  // The value for key, if anything holds it.
  Value find(const Key &key) const {
    std::lock_guard lock(mutex_);
    auto it = entries_.find(key);
    if (it == entries_.end()) return nullptr;
    auto *live = std::get_if<Live>(&it->second);
    return live ? live->lock() : nullptr;
  }

private:
  using Pending = std::shared_future<Result>;
  using Live = std::weak_ptr<const T>;
  using Entry = std::variant<Pending, Live>;

  static bool is_dead(const Entry &e) {
    auto *live = std::get_if<Live>(&e);
    return live && live->expired();
  }

  mutable std::mutex mutex_;
  std::unordered_map<Key, Entry> entries_;
};

} // namespace sb
