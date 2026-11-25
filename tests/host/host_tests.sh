#!/usr/bin/env bash
# Renders projects in REAPER with the installed plugin and checks the results.
# The plugin needs SB_TEST_HOOKS, which the dev preset turns on.
#
#   tests/host/host_tests.sh <stereo clip.wav at 44.1k> <second clip.wav> <brain sample>...
#
# REAPER runs with its own resource folder, a copy of your settings and
# licence, so these runs don't touch your recent projects. There's no GUI to
# click, so the brain comes from SAMPLEBRAIN_TEST_SAMPLES, which the plugin
# builds a brain from when a project has none. Settings like stereo are
# changed by editing the saved project's ARA archive.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
clip=$1 second=$2
shift 2
SAMPLEBRAIN_TEST_SAMPLES=$(IFS=:; echo "$*")
export SAMPLEBRAIN_TEST_SAMPLES

work=$(mktemp -d -t samplebrain-host)
config=$work/reaper-config
mkdir -p "$config"
for f in reaper.ini reaper-license.rk reaper-install-rev.txt reaper-reginfo2.ini; do
  cp "$HOME/Library/Application Support/REAPER/$f" "$config/" 2>/dev/null || true
done
cp "$clip" "$work/clip44.wav"
cp "$second" "$work/clip_b.wav"
afconvert -f WAVE -d LEF32@48000 "$work/clip44.wav" "$work/clip48.wav"
echo "working in $work"

failures=0
reaper() {
  (cd "$work" && perl -e 'alarm 900; exec @ARGV' /Applications/REAPER.app/Contents/MacOS/REAPER \
    -cfgfile "$config/reaper.ini" -nosplash -newinst "$@" > /dev/null 2>&1)
}
make_project() { # name clip rate
  SB_DIR=$work SB_NAME=$1 SB_CLIP=$work/$2 SB_RATE=$3 SB_DELAY=${SB_DELAY:-4} reaper -new "$here/make_project.lua"
}
render() { # project.rpp out.wav [rpp_edit args...]
  local rpp=$1 wav=$2
  shift 2
  python3 -I "$here/rpp_edit.py" "$work/$rpp" "$work/edited.rpp" --wav "$work/$wav" "$@"
  reaper -renderproject "$work/edited.rpp"
}
wavstat() { python3 -I "$here/wavstat.py" "$@"; }
check() { # description command...
  local description=$1
  shift
  if "$@"; then echo "pass: $description"; else echo "FAIL: $description"; failures=$((failures + 1)); fi
}
residual() { wavstat "$work/$1" "$work/$2" ${3:-} | sed -n 's/.*residual peak \([0-9.]*\).*/\1/p'; }
sides() { wavstat "$work/$1" | sed -n 's/.*left minus right peak \([0-9.]*\).*/\1/p'; }
number() { [[ $1 =~ ^[0-9]+(\.[0-9]*)?$ ]]; }
is_zero() { number "$1" && [ "$(python3 -c "print(float('$1') == 0)")" = True ]; }
not_zero() { number "$1" && ! is_zero "$1"; }
brains() { ls "$HOME/Library/Caches/samplebrain-plugin/brains" | wc -l | tr -d ' '; }

# REAPER takes a clip's sample access away soon after a scripted project
# opens, so a render still waiting for a new brain comes out silent. Build
# each rate's brain once first, with time to finish.
for rate in 44100 48000; do
  SB_DELAY=15 make_project "warm${rate:0:2}" "clip${rate:0:2}.wav" "$rate"
done

for rate in 44100 48000; do
  short=${rate:0:2}
  make_project "r$short" "clip$short.wav" "$rate"
  check "$rate: the clip renders" not_zero "$(wavstat "$work/r$short.wav" | sed -n 's/.*peak \([0-9.]*\)$/\1/p' | head -1)"
  check "$rate: stereo off plays the same on both sides" is_zero "$(sides "r$short.wav")"
  render "r$short.rpp" "reopened$short.wav"
  check "$rate: reopening and rendering again nulls" is_zero "$(residual "reopened$short.wav" "r$short.wav")"
done

before=$(brains)
render r44.rpp stereo.wav --set 'clips.*.target.stereo=true'
check "stereo on plays different sides" not_zero "$(sides stereo.wav)"
render r44.rpp synaptic.wav --set 'clips.*.render.algorithm=2' --set 'clips.*.render.seed=7'
check "a render setting changes the render" not_zero "$(residual synaptic.wav r44.wav)"
check "a render setting doesn't build a brain" [ "$(brains)" = "$before" ]
render r44.rpp undo.wav
check "going back to the old settings plays the old render" is_zero "$(residual undo.wav r44.wav)"
render r44.rpp section.wav --range 1.0 2.0
check "a section plays the matching part of the render" is_zero "$(residual section.wav r44.wav 44100)"
render r44.rpp notes.wav --set 'clips.*.target_block.unit=1'
check "a target block in notes renders differently" not_zero "$(residual notes.wav r44.wav)"
render r44.rpp notes_slower.wav --set 'clips.*.target_block.unit=1' --tempo 90
check "a target block in notes follows the tempo" not_zero "$(residual notes_slower.wav notes.wav)"
render r44.rpp ms_slower.wav --tempo 90
check "a target block in ms ignores the tempo" is_zero "$(residual ms_slower.wav r44.wav)"
render r44.rpp brain_edit.wav --set 'brains.0.block.ms=45'
check "an unbuilt brain edit keeps playing the old brain" is_zero "$(residual brain_edit.wav r44.wav)"

SB_SECOND=$work/clip_b.wav make_project two clip44.wav 44100
check "two clips share one brain" [ "$(python3 - "$work/two.rpp" <<'EOF'
import base64, json, re, sys
text = open(sys.argv[1]).read()
blob = re.search(r"<ARA com\S+ \d+\s*<BIN\s*(.*?)>", text, re.S).group(1)
archive = json.loads(base64.b64decode("".join(blob.split())).rstrip(b"\0"))
print(len(archive["clips"]), len({c["brain"] for c in archive["clips"].values()}))
EOF
)" = "2 1" ]
render two.rpp two_reopened.wav
check "two clips reopen and null" is_zero "$(residual two_reopened.wav two.wav)"

echo "$failures failed"
exit $((failures > 0))
