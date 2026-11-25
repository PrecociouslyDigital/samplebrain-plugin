# Samplebrain plugin

[Samplebrain](https://gitlab.com/then-try-this/samplebrain) plays a *target* sound using blocks
from a *brain* of other sounds. This is Samplebrain as an ARA plugin: the target is a clip on your
DAW's timeline, and the plugin renders it ahead of time and plays the result back from a cache.

It builds as VST3 and AU for Apple Silicon. REAPER is the host it's tested in.

## Building

Everything comes from the Nix flake, including upstream Samplebrain, which it pins and patches
with `patches/*.patch`.

```sh
nix build .#plugin      # VST3 and AU bundles in ./result
nix flake check         # engine tests
nix develop             # a shell with the toolchain, then:
cmake --preset dev && cmake --build --preset dev   # installs into ~/Library/Audio/Plug-Ins
```

The `asan`, `tsan` and `rtsan` presets run the engine tests under AddressSanitizer,
ThreadSanitizer and RealtimeSanitizer. The plugin's playback callback is checked when it
compiles instead: clang rejects it if it calls anything not known to be nonblocking.

`tests/host/host_tests.sh` renders projects in REAPER with the installed plugin and checks
that they null where they should, for example after saving and reopening.

To change the upstream patches, check them out as commits, edit, and export them again:

```sh
scripts/patches.sh checkout ../samplebrain-patched
# commit or rebase in ../samplebrain-patched
scripts/patches.sh export ../samplebrain-patched
```

## Layout

- `engine/core` is the engine: `analyse_brain`, `analyse_target` and `render`, whose results
  depend only on their arguments. `settings.h` lists every parameter once.
- `engine/cache` holds the memo and disk caches that let undo play an old render straight away.
- `plugin` is the JUCE ARA plugin around the engine.
- `tests` are property tests on the engine, plus the REAPER tests in `tests/host`.
- `bench` times each engine stage. Results are in [docs/benchmarks.md](docs/benchmarks.md).

## Licence

The source here is GPL-2.0-or-later, like upstream Samplebrain. Built binaries link JUCE 8,
whose free licence is the AGPLv3, so the binaries are AGPLv3.
