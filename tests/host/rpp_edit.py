"""Edits a REAPER project's Samplebrain ARA archive and render settings.

usage: rpp_edit.py in.rpp out.rpp [--wav out.wav] [--range START END]
                   [--tempo BPM] [--set path=json ...]

A path is dot-separated, with * matching every key or item, for example
clips.*.target.stereo=true or brains.0.settings.block_size=2000.
"""
import argparse
import base64
import json
import re


def assign(node, path, value):
    head, rest = path[0], path[1:]
    if isinstance(node, list):
        keys = range(len(node)) if head == "*" else [int(head)]
    else:
        keys = list(node) if head == "*" else [head]
    for k in keys:
        if rest:
            assign(node[k], rest, value)
        else:
            node[k] = value


def edit_archive(lines, sets):
    out, i = [], 0
    while i < len(lines):
        line = lines[i]
        out.append(line)
        if re.match(r"\s*<ARA(SRC)? com\.precociouslydigital\.samplebrain", line) and "<BIN" in lines[i + 1]:
            out.append(lines[i + 1])
            j = i + 2
            chunks = []
            while lines[j].strip() != ">":
                chunks.append(lines[j].strip())
                j += 1
            raw = base64.b64decode("".join(chunks))
            archive = json.loads(raw.rstrip(b"\0").decode())
            for path, value in sets:
                assign(archive, path.split("."), value)
            encoded = base64.b64encode(json.dumps(archive).encode() + b"\0").decode()
            indent = re.match(r"\s*", lines[i + 2]).group(0)
            for k in range(0, len(encoded), 128):
                out.append(indent + encoded[k:k + 128] + "\n")
            i = j
            continue
        i += 1
    return out


def main():
    p = argparse.ArgumentParser()
    p.add_argument("src")
    p.add_argument("dst")
    p.add_argument("--wav")
    p.add_argument("--range", nargs=2, type=float)
    p.add_argument("--tempo", type=float)
    p.add_argument("--set", action="append", default=[])
    args = p.parse_args()
    sets = [(s.split("=", 1)[0], json.loads(s.split("=", 1)[1])) for s in args.set]

    with open(args.src) as f:
        lines = f.readlines()
    lines = edit_archive(lines, sets)
    for n, line in enumerate(lines):
        if args.wav and line.strip().startswith("RENDER_FILE "):
            lines[n] = re.sub(r'RENDER_FILE ".*"', 'RENDER_FILE "%s"' % args.wav, line)
        if args.tempo and line.strip().startswith("TEMPO "):
            parts = line.split()
            parts[1] = repr(args.tempo)
            lines[n] = re.match(r"\s*", line).group(0) + " ".join(parts) + "\n"
        if args.range and line.strip().startswith("RENDER_RANGE "):
            parts = line.split()
            parts[1:4] = ["0", repr(args.range[0]), repr(args.range[1])]
            lines[n] = re.match(r"\s*", line).group(0) + " ".join(parts) + "\n"
    with open(args.dst, "w") as f:
        f.writelines(lines)


main()
