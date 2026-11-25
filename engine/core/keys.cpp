#include "keys.h"

#include <format>
#include <type_traits>

#include <xxhash.h>

namespace sb {

namespace {

// Bump to invalidate every cached brain and render, for example after a
// change to the engine that changes its output.
constexpr std::uint32_t engine_version = 1;

class Hasher {
public:
  explicit Hasher(std::string_view domain) {
    XXH3_128bits_reset(state_);
    add(engine_version);
    add(domain);
  }
  ~Hasher() { XXH3_freeState(state_); }
  Hasher(const Hasher &) = delete;
  Hasher &operator=(const Hasher &) = delete;

  void bytes(const void *data, std::size_t size) {
    XXH3_128bits_update(state_, data, size);
  }

  template <class T>
    requires std::is_arithmetic_v<T>
  void add(T value) {
    bytes(&value, sizeof value);
  }

  void add(std::string_view s) {
    add(s.size());
    bytes(s.data(), s.size());
  }

  template <class Tag> void add(const Key<Tag> &k) {
    add(k.high);
    add(k.low);
  }

  template <Settings S> void add(const S &settings) {
    visit_settings(settings, [&](const ParamInfo &info, const auto &field) {
      add(info.id);
      add(field);
    });
  }

  template <class K> K finish() {
    XXH128_hash_t h = XXH3_128bits_digest(state_);
    return K{h.high64, h.low64};
  }

private:
  XXH3_state_t *state_ = XXH3_createState();
};

} // namespace

template <class Tag> std::string Key<Tag>::hex() const {
  return std::format("{:016x}{:016x}", high, low);
}

template struct Key<AudioHashTag>;
template struct Key<BrainKeyTag>;
template struct Key<TargetKeyTag>;
template struct Key<RenderKeyTag>;

AudioHash hash_audio(std::span<const float> samples) {
  XXH128_hash_t h = XXH3_128bits(samples.data(), samples.size_bytes());
  return AudioHash{h.high64, h.low64};
}

BrainKey brain_key(std::span<const AudioHash> sounds,
                   const BrainSettings &settings, unsigned rate) {
  Hasher h("brain");
  h.add(sounds.size());
  for (const auto &s : sounds) h.add(s);
  h.add(settings);
  h.add(rate);
  return h.finish<BrainKey>();
}

TargetKey target_key(std::span<const AudioHash> channels,
                     const TargetSettings &settings, unsigned rate) {
  Hasher h("target");
  h.add(channels.size());
  for (const auto &c : channels) h.add(c);
  h.add(settings);
  h.add(rate);
  return h.finish<TargetKey>();
}

RenderKey render_key(const BrainKey &brain, const TargetKey &target,
                     const RenderSettings &settings) {
  Hasher h("render");
  h.add(brain);
  h.add(target);
  h.add(settings);
  h.add(settings.sound_enabled.size());
  for (bool on : settings.sound_enabled) h.add(on);
  return h.finish<RenderKey>();
}

} // namespace sb
