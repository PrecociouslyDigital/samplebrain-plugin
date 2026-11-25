"""Compares WAV renders: peak, channel difference, and null tests.

usage: wavstat.py a.wav [b.wav [offset into b]]
"""
import struct
import sys


def read(path):
    with open(path, "rb") as f:
        data = f.read()
    # Walk RIFF chunks ourselves so float WAVs work too.
    pos = 12
    fmt = None
    frames = None
    while pos < len(data):
        cid = data[pos:pos + 4]
        size = struct.unpack("<I", data[pos + 4:pos + 8])[0]
        body = data[pos + 8:pos + 8 + size]
        if cid == b"fmt ":
            tag, ch, rate, _, _, bits = struct.unpack("<HHIIHH", body[:16])
            fmt = (tag, ch, rate, bits)
        elif cid == b"data":
            frames = body
        pos += 8 + size + (size & 1)
    tag, ch, rate, bits = fmt
    width = bits // 8
    n = len(frames) // width
    if bits == 32 and tag in (3, 0xFFFE):
        samples = list(struct.unpack("<%df" % n, frames[:n * 4]))
    elif bits == 24:
        samples = []
        for i in range(n):
            b = frames[i * 3:i * 3 + 3]
            v = int.from_bytes(b, "little", signed=True)
            samples.append(v / 8388608.0)
    elif bits == 16:
        samples = [v / 32768.0 for v in struct.unpack("<%dh" % n, frames[:n * 2])]
    else:
        raise SystemExit("unsupported format %r" % (fmt,))
    chans = [samples[c::ch] for c in range(ch)]
    return rate, chans


def peak(xs):
    return max((abs(x) for x in xs), default=0.0)


def main():
    rate, a = read(sys.argv[1])
    print("%s: %d Hz, %d ch, %d frames, peak %.4f" % (sys.argv[1], rate, len(a), len(a[0]), max(peak(c) for c in a)))
    if len(a) == 2:
        print("  left minus right peak %.6f" % peak([l - r for l, r in zip(a[0], a[1])]))
    if len(sys.argv) > 2:
        rate_b, b = read(sys.argv[2])
        # Optionally compare a against b starting this many frames into b.
        offset = int(sys.argv[3]) if len(sys.argv) > 3 else 0
        n = min(len(a[0]), len(b[0]) - offset)
        diff = max(peak([x - y for x, y in zip(ca[:n], cb[offset:offset + n])]) for ca, cb in zip(a, b))
        print("  null against %s from frame %d: residual peak %.6f over %d frames" % (sys.argv[2], offset, diff, n))


main()
