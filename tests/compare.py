#!/usr/bin/env python3
"""Compare the C slicing code with the Python reference implementation.

The reference (tests/reference) is the Python slicer engine code this plugin
was ported from. Needs numpy and soundfile.

Usage: compare.py TEST_MODEL_BINARY WORK_DIR
"""

import os
import sys
import subprocess

import numpy as np
import soundfile as sf

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "reference"))
import zynthian_slicer_analysis as ref  # noqa: E402
import zynthian_slicer_sfz as ref_sfz  # noqa: E402


def make_drum_loop(path, sr, channels, hits, length_s, seed):
    """Write a percussive test file: decaying noise bursts on a quiet bed."""
    rng = np.random.default_rng(seed)
    n = int(sr * length_s)
    data = (rng.standard_normal((n, channels)) * 0.002).astype(np.float32)
    for t, amp in hits:
        start = int(t * sr)
        dur = min(n - start, int(0.12 * sr))
        if dur <= 0:
            continue
        env = amp * np.exp(-np.arange(dur) / (0.02 * sr))
        burst = rng.standard_normal((dur, channels)) * env[:, None]
        data[start:start + dur] += burst.astype(np.float32)
    data = np.clip(data, -1.0, 1.0)
    sf.write(path, data, sr, subtype="PCM_24" if path.endswith(".wav") else None)
    return n


def c_transients(binary, path, threshold, min_gap):
    out = subprocess.run([binary, "transients", path, str(threshold), str(min_gap)],
                         capture_output=True, text=True, check=True).stdout
    return [tuple(int(v) for v in line.split()) for line in out.splitlines() if line.strip()]


def main():
    binary, work = sys.argv[1], sys.argv[2]
    os.makedirs(work, exist_ok=True)
    failures = 0
    checks = 0

    cases = [
        ("loop44.wav", 44100, 2, [(0.0, 0.9), (0.5, 0.7), (1.0, 0.8), (1.25, 0.3), (1.5, 0.9), (1.75, 0.5)], 2.0),
        ("loop48m.wav", 48000, 1, [(0.1, 0.5), (0.35, 0.9), (0.6, 0.2), (0.9, 0.8), (1.6, 0.6)], 2.2),
        ("dense96.wav", 96000, 2, [(i * 0.07, 0.3 + 0.6 * ((i * 7) % 5) / 4) for i in range(40)], 3.0),
        ("loop22.flac", 22050, 2, [(0.2, 0.8), (0.8, 0.6), (1.4, 0.9)], 1.8),
    ]
    for name, sr, ch, hits, length, in [c[:5] for c in cases]:
        path = os.path.join(work, name)
        frames = make_drum_loop(path, sr, ch, hits, length, seed=len(name))
        frames = sf.info(path).frames
        for threshold in (0, 10, 30, 50, 80, 100):
            for min_gap in (10, 50, 200):
                expected = [(s["start"], s["end"]) for s in ref.detect_transients(path, frames, threshold, min_gap)]
                got = c_transients(binary, path, threshold, min_gap)
                checks += 1
                if expected != got:
                    failures += 1
                    print(f"MISMATCH transients {name} threshold={threshold} min_gap={min_gap}:")
                    print(f"  python ({len(expected)}): {expected[:8]}...")
                    print(f"  c      ({len(got)}): {got[:8]}...")

    # Even slices and normalisation are checked against Python through the
    # SFZ output, which lists every region
    for frames, count, base_note in ((96000, 8, 36), (100003, 7, 60), (44100, 127, 1), (50000, 16, 120)):
        slices = ref.normalize_slices(ref.make_even_slices(frames, count), frames, base_note)
        for i, sl in enumerate(slices):
            sl["mode"] = ref.PLAY_MODES[i % 3]
        expected = ref_sfz.build_sfz(slices, "sample.wav", gain_db=-2.5, polyphony=12,
                                     fade_seconds=64 / 48000.0, tune=-3)
        stdin = "".join(f"{s['start']} {s['end']} {s['mode']}\n" for s in slices)
        got = subprocess.run([binary, "sfz", str(base_note), "-2.5", "-3", "12", str(64 / 48000.0)],
                             input=stdin, capture_output=True, text=True).stdout
        checks += 1
        if expected != got:
            failures += 1
            print(f"MISMATCH sfz frames={frames} count={count} base_note={base_note}")
            for a, b in zip(expected.splitlines(), got.splitlines()):
                if a != b:
                    print(f"  python: {a!r}\n  c:      {b!r}")
                    break

    print(f"{checks} comparisons, {failures} mismatches")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
