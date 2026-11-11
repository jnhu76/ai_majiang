#!/usr/bin/env python3
"""Programmatic WAV quality analysis (standard library only, Python 3.12).

Reads every *.wav under --root, validates the RIFF/PCM header, and writes
audio-analysis.json plus audio-review.csv. The human-listen column is always
"pending": these numbers are signal measurements, never a listening verdict.

Usage:
  python -I benchmarks/p1_audio_analyze.py --root benchmarks/results/p1/audio/tts_real
  python -I benchmarks/p1_audio_analyze.py --root DIR --out DIR
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import pathlib
import struct
import sys

SCHEMA = 1
SILENCE_DBFS = -60.0
F0_MIN_HZ = 60.0
F0_MAX_HZ = 400.0
FRAME_S = 0.040
HOP_S = 0.020
DB_FLOOR = -120.0


def read_wav(path: pathlib.Path) -> tuple[int, int, list[int]]:
    data = path.read_bytes()
    if len(data) < 12 or data[0:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError("not a RIFF/WAVE file")
    pos = 12
    fmt = None
    pcm = None
    while pos + 8 <= len(data):
        chunk_id = data[pos:pos + 4]
        size = struct.unpack("<I", data[pos + 4:pos + 8])[0]
        body = data[pos + 8:pos + 8 + size]
        if len(body) < size:
            raise ValueError(f"truncated {chunk_id!r} chunk")
        if chunk_id == b"fmt ":
            if size < 16:
                raise ValueError("short fmt chunk")
            fmt = struct.unpack("<HHIIHH", body[:16])
        elif chunk_id == b"data":
            pcm = body
        pos += 8 + size + (size & 1)
    if fmt is None or pcm is None:
        raise ValueError("missing fmt or data chunk")
    audio_format, channels, sample_rate, _byte_rate, _block_align, bits = fmt
    if audio_format != 1:
        raise ValueError(f"unsupported format {audio_format} (expected PCM)")
    if bits != 16:
        raise ValueError(f"unsupported sample width {bits}")
    if sample_rate <= 0:
        raise ValueError("zero sample rate")
    usable = (len(pcm) // (2 * channels)) * 2 * channels
    samples = list(struct.unpack(f"<{usable // 2}h", pcm[:usable]))
    return sample_rate, channels, samples


def db(value: float) -> float:
    if value <= 0:
        return DB_FLOOR
    return max(DB_FLOOR, 20.0 * math.log10(value))


def channel_span(samples: list[int], channels: int, channel: int) -> list[int]:
    return samples[channel::channels]


def silence_span(mono: list[int], threshold: float) -> tuple[float, float, int]:
    if not mono:
        return 0.0, 0.0, 0
    first = next((i for i, value in enumerate(mono) if abs(value) > threshold), len(mono))
    last = next((i for i, value in enumerate(reversed(mono)) if abs(value) > threshold), len(mono))
    return float(first), float(last), len(mono) - first - last


def autocorr_f0(mono: list[int], sample_rate: int) -> float:
    frame = max(1, int(sample_rate * FRAME_S))
    hop = max(1, int(sample_rate * HOP_S))
    min_lag = max(1, int(sample_rate / F0_MAX_HZ))
    max_lag = min(frame - 1, int(sample_rate / F0_MIN_HZ))
    estimates: list[float] = []
    if max_lag <= min_lag or len(mono) < frame:
        return 0.0
    energy_floor = 32768.0 * (10.0 ** (-40.0 / 20.0))
    for start in range(0, len(mono) - frame + 1, hop):
        window = mono[start:start + frame]
        energy = math.sqrt(sum(value * value for value in window) / frame)
        if energy < energy_floor:
            continue
        mean = sum(window) / frame
        centered = [value - mean for value in window]
        norm0 = sum(value * value for value in centered)
        if norm0 <= 0:
            continue
        best_lag = 0
        best_score = 0.0
        for lag in range(min_lag, max_lag + 1):
            numerator = sum(centered[i] * centered[i + lag] for i in range(frame - lag))
            norm = math.sqrt(sum(value * value for value in centered[:frame - lag]) *
                             sum(value * value for value in centered[lag:]))
            if norm <= 0:
                continue
            score = numerator / norm
            if score > best_score:
                best_score = score
                best_lag = lag
        if best_lag and best_score >= 0.3:
            estimates.append(sample_rate / best_lag)
    if not estimates:
        return 0.0
    estimates.sort()
    return estimates[len(estimates) // 2]


def analyze_file(path: pathlib.Path) -> dict:
    sample_rate, channels, samples = read_wav(path)
    mono = channel_span(samples, channels, 0)
    if channels > 1:
        # Peak/clip statistics cover every channel; pitch uses the first channel.
        peak = max(abs(value) for value in samples) if samples else 0
        rms = math.sqrt(sum(value * value for value in samples) / len(samples)) if samples else 0.0
        clipped = sum(1 for value in samples if abs(value) >= 32767)
        frame_count = len(mono)
    else:
        peak = max(abs(value) for value in mono) if mono else 0
        rms = math.sqrt(sum(value * value for value in mono) / len(mono)) if mono else 0.0
        clipped = sum(1 for value in mono if abs(value) >= 32767)
        frame_count = len(mono)
    threshold = 32768.0 * (10.0 ** (SILENCE_DBFS / 20.0))
    head, tail, _voiced = silence_span(mono, threshold)
    silent = sum(1 for value in mono if abs(value) <= threshold)
    mean = sum(mono) / len(mono) if mono else 0.0
    return {
        "file": path.name,
        "sample_rate": sample_rate,
        "channels": channels,
        "duration_s": round(frame_count / sample_rate, 4) if sample_rate else 0.0,
        "peak_dbfs": round(db(peak / 32768.0), 3),
        "rms_dbfs": round(db(rms / 32768.0), 3),
        "dc_offset": round(mean / 32768.0, 6),
        "clipped_samples": clipped,
        "silence_pct": round(100.0 * silent / len(mono), 3) if mono else 0.0,
        "head_silence_s": round(head / sample_rate, 4) if sample_rate else 0.0,
        "tail_silence_s": round(tail / sample_rate, 4) if sample_rate else 0.0,
        "f0_mean_hz": round(autocorr_f0(mono, sample_rate), 2),
        "listen": "pending",
    }


def analyze_root(root: pathlib.Path) -> list[dict]:
    files = sorted(root.rglob("*.wav"))
    return [analyze_file(path) for path in files]


def repo_confined(raw: str, option: str) -> pathlib.Path:
    """Normalize a CLI path and confine it to this repository.

    The analyzer only ever reads and writes repo-local evidence trees, so a
    mistyped or hostile --root/--out must not read or write outside the repo
    (path traversal). Normalization resolves '..' and symlinks; '..' segments
    are rejected outright and the resolved path must stay inside the repo.
    Both documented invocations in REPORT.md stay inside.
    """
    if not raw or "\x00" in raw:
        raise SystemExit(f"empty or NUL-containing path for {option}")
    normalized = raw.replace("\\", "/")
    if ".." in normalized.split("/"):
        raise SystemExit(f"{option} must not contain '..' path segments: {raw}")
    repo = pathlib.Path(__file__).resolve().parent.parent
    resolved = pathlib.Path(raw).resolve()
    if not (resolved == repo or resolved.is_relative_to(repo)):
        raise SystemExit(f"{option} escapes the repository: {raw} -> {resolved}")
    return resolved


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Analyze PCM WAV quality (stdlib only).")
    parser.add_argument("--root", required=True, type=lambda v: repo_confined(v, "--root"),
                        help="directory containing WAV files")
    parser.add_argument("--out", type=lambda v: repo_confined(v, "--out"),
                        help="output directory (default: --root)")
    args = parser.parse_args(argv)
    root = args.root
    if not root.is_dir():
        raise SystemExit(f"not a directory: {root}")
    out = args.out if args.out else root
    out.mkdir(parents=True, exist_ok=True)
    files = analyze_root(root)
    payload = {
        "schema": SCHEMA,
        "root": str(root),
        "files": files,
        "summary": {
            "files": len(files),
            "total_clipped_samples": sum(item["clipped_samples"] for item in files),
            "sample_rates": sorted({item["sample_rate"] for item in files}),
            "listen": "pending",
        },
    }
    json_path = out / "audio-analysis.json"
    json_path.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    csv_path = out / "audio-review.csv"
    header = ["file", "sample_rate", "channels", "duration_s", "peak_dbfs", "rms_dbfs",
              "dc_offset", "clipped_samples", "silence_pct", "head_silence_s",
              "tail_silence_s", "f0_mean_hz", "listen"]
    with csv_path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=header, lineterminator="\n")
        writer.writeheader()
        for item in files:
            writer.writerow({key: item[key] for key in header})
    print(f"wrote {json_path}")
    print(f"wrote {csv_path} ({len(files)} files; listen=pending)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
