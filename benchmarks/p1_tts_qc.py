#!/usr/bin/env python3
"""P1 TTS quality report: qc.tsv + listen_index.html (standard library only).

Reuses the signal analysis in p1_audio_analyze.py (loaded by file path so it
works under `python -I`), joins the TTS fixtures for speaker/text, and reports
three-speaker F0 separation. The listen column is always "待人工" -- these are
programmatic measurements, not a listening test.

mock 16 kHz and real 8 kHz output must never be pooled: pass --provider and a
single WAV directory per provider.

Usage:
  python -I benchmarks/p1_tts_qc.py --wav-dir benchmarks/results/p1/audio/tts_real \
      --lines benchmarks/p1_tts_lines.tsv --provider sherpa
"""

from __future__ import annotations

import argparse
import csv
import html
import importlib.util
import pathlib
import statistics
import sys

sys.dont_write_bytecode = True  # keep the evidence tree free of __pycache__
HERE = pathlib.Path(__file__).resolve().parent


def _load_analyzer():
    spec = importlib.util.spec_from_file_location("p1_audio_analyze", HERE / "p1_audio_analyze.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def load_lines(path: pathlib.Path) -> dict[str, dict]:
    mapping: dict[str, dict] = {}
    if not path.is_file():
        # Without the fixture the speaker/F0 section would be silently empty.
        raise SystemExit(f"lines file not found: {path}")
    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            line = line.rstrip("\r\n")
            if not line or line.startswith("#"):
                continue
            fields = line.split("\t")
            if len(fields) < 4:
                continue
            mapping[fields[0]] = {"speaker": int(fields[2]), "text": fields[3]}
    return mapping


def repo_confined(raw: str, option: str) -> pathlib.Path:
    """Normalize a CLI path and confine it to this repository.

    The QC report only ever reads and writes repo-local evidence trees, so a
    mistyped or hostile --wav-dir/--lines/--out must not read or write outside
    the repo (path traversal). Normalization resolves '..' and symlinks; '..'
    segments are rejected outright and the resolved path must stay inside.
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
    parser = argparse.ArgumentParser(description="TTS quality report (stdlib only).")
    parser.add_argument("--wav-dir", required=True, type=lambda v: repo_confined(v, "--wav-dir"))
    parser.add_argument("--lines", default=str(HERE / "p1_tts_lines.tsv"),
                        type=lambda v: repo_confined(v, "--lines"))
    parser.add_argument("--provider", default="unknown", choices=["mock", "sherpa", "unknown"])
    parser.add_argument("--out", type=lambda v: repo_confined(v, "--out"),
                        help="output directory (default: --wav-dir)")
    args = parser.parse_args(argv)
    analyzer = _load_analyzer()
    wav_dir = args.wav_dir
    if not wav_dir.is_dir():
        raise SystemExit(f"not a directory: {wav_dir}")
    out = args.out if args.out else wav_dir
    out.mkdir(parents=True, exist_ok=True)
    lines = load_lines(args.lines)

    rows: list[dict] = []
    for path in sorted(wav_dir.glob("*.wav")):
        metrics = analyzer.analyze_file(path)
        info = lines.get(path.stem, {})
        text = info.get("text", "")
        codepoints = len([ch for ch in text if not ch.isspace()])
        rows.append({
            **metrics,
            "id": path.stem,
            "speaker": info.get("speaker", 0),
            "text": text,
            "provider": args.provider,
            "duration_per_codepoint": round(metrics["duration_s"] / codepoints, 4) if codepoints else 0.0,
            "listen": "待人工",
        })

    # Three-speaker separation: median F0 per speaker over voiced files only.
    by_speaker: dict[int, list[float]] = {}
    for row in rows:
        if row["speaker"] and row["f0_mean_hz"] > 0:
            by_speaker.setdefault(row["speaker"], []).append(row["f0_mean_hz"])
    medians = {speaker: round(statistics.median(values), 2) for speaker, values in sorted(by_speaker.items())}
    speakers = sorted(medians)
    separations = []
    for i in range(len(speakers)):
        for j in range(i + 1, len(speakers)):
            separations.append({
                "pair": f"s{speakers[i]}-s{speakers[j]}",
                "delta_hz": round(abs(medians[speakers[i]] - medians[speakers[j]]), 2),
            })

    qc_path = out / "qc.tsv"
    header = ["id", "speaker", "text", "provider", "sample_rate", "duration_s", "peak_dbfs",
              "rms_dbfs", "dc_offset", "clipped_samples", "silence_pct", "head_silence_s",
              "tail_silence_s", "duration_per_codepoint", "f0_median_hz", "listen"]
    with qc_path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle, delimiter="\t", lineterminator="\n")
        writer.writerow(header)
        for row in rows:
            writer.writerow([
                row["id"], row["speaker"], row["text"], row["provider"], row["sample_rate"],
                row["duration_s"], row["peak_dbfs"], row["rms_dbfs"], row["dc_offset"],
                row["clipped_samples"], row["silence_pct"], row["head_silence_s"],
                row["tail_silence_s"], row["duration_per_codepoint"], row["f0_mean_hz"],
                row["listen"],
            ])

    index_path = out / "listen_index.html"
    parts = [
        "<!doctype html><meta charset=\"utf-8\">",
        "<title>P1 TTS listen index</title>",
        "<style>body{font-family:sans-serif;margin:1.5rem}td,th{padding:.25rem .5rem;border-bottom:1px solid #ddd}"
        ".pending{color:#a60}</style>",
        f"<h1>P1 TTS listen index — provider={html.escape(args.provider)}</h1>",
        "<p>Programmatic metrics only. The listen column stays 待人工 until a person grades it.</p>",
        f"<p>F0 medians: {html.escape(str(medians))}; pairwise: {html.escape(str(separations))}</p>",
        "<table><thead><tr><th>WAV</th><th>speaker</th><th>text</th><th>rate</th>"
        "<th>duration</th><th>peak dBFS</th><th>RMS dBFS</th><th>clipped</th>"
        "<th>silence %</th><th>F0 Hz</th><th>listen</th></tr></thead><tbody>",
    ]
    for row in rows:
        parts.append(
            "<tr><td><a href=\"{file}\">{file}</a></td><td>{speaker}</td><td>{text}</td>"
            "<td>{rate}</td><td>{duration}</td><td>{peak}</td><td>{rms}</td>"
            "<td>{clipped}</td><td>{silence}</td><td>{f0}</td>"
            "<td class=\"pending\">待人工</td></tr>".format(
                file=html.escape(row["file"]), speaker=row["speaker"], text=html.escape(row["text"]),
                rate=row["sample_rate"], duration=row["duration_s"], peak=row["peak_dbfs"],
                rms=row["rms_dbfs"], clipped=row["clipped_samples"], silence=row["silence_pct"],
                f0=row["f0_mean_hz"]))
    parts.append("</tbody></table>")
    index_path.write_text("\n".join(parts) + "\n", encoding="utf-8")
    print(f"wrote {qc_path}")
    print(f"wrote {index_path} ({len(rows)} files; listen=待人工)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
