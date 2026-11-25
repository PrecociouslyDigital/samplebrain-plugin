# Engine benchmarks

Run with `bench <target> <brain files...>` from a release build, on an Apple M4, single
threaded. The brain was 3.6 minutes of mono vocal takes in 22 files. The target was a
3-minute song, also cut to its first 10 seconds. Every setting was its default: blocks of
1000 samples with 0.75 overlap, at 44.1 kHz.

| Step | Time |
|---|---|
| Build the brain (37,993 blocks and their synapses) | 88 s |
| Save the brain | 1.5 s |
| Load the brain | 2.7 s |
| Analyse a 10 s clip | 0.16 s |
| Analyse a 3 min clip | 2.9 s |
| Basic render, 10 s clip | 16 s |
| Basic render, 3 min clip | 290 s |
| Synaptic render, 10 s clip | 0.02 s |
| Synaptic render, 3 min clip | 0.42 s |
| Load a cached render, 3 min clip | 0.03 s |

What this means for the caches:

- Brains go to disk. Loading one takes 3 s instead of a minute and a half to build.
- Renders go to disk. Basic search takes about 1.6 seconds per second of clip, so undoing a
  change to a 3-minute clip would otherwise take five minutes.
- Targets stay in memory only. Analysing a 3-minute clip takes 3 seconds, which is quick
  next to any render that uses basic search.
- The brain file was 538 MB, mostly because every block keeps its audio and its 1000
  synapses. A 10 GB disk cache holds about 18 brains this size.
