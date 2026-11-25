#!/usr/bin/env bash
# Edit the upstream patches as ordinary git commits.
#
#   scripts/patches.sh checkout <dir>   clone upstream at the pinned rev into <dir>
#                                       and apply patches/*.patch as commits
#   scripts/patches.sh export <dir>     write the commits in <dir> back to patches/
set -euo pipefail

repo=$(cd "$(dirname "$0")/.." && pwd)
upstream_url=https://gitlab.com/then-try-this/samplebrain.git
upstream_rev=$(sed -n 's/.*samplebrain?rev=\([0-9a-f]*\).*/\1/p' "$repo/flake.nix")

cmd=${1:?usage: patches.sh checkout|export <dir>}
dir=${2:?usage: patches.sh checkout|export <dir>}

case $cmd in
  checkout)
    git clone --quiet "$upstream_url" "$dir"
    git -C "$dir" checkout --quiet -b patched "$upstream_rev"
    shopt -s nullglob
    patches=("$repo"/patches/*.patch)
    if ((${#patches[@]})); then git -C "$dir" am --quiet "${patches[@]}"; fi
    ;;
  export)
    rm -f "$repo"/patches/*.patch
    git -C "$dir" format-patch --quiet --no-signature --zero-commit \
      -o "$repo/patches" "$upstream_rev"..HEAD
    ;;
  *)
    echo "unknown command: $cmd" >&2
    exit 1
    ;;
esac
