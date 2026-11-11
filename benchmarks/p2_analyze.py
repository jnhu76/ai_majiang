#!/usr/bin/env python3
"""Offline analysis for the P2 voice-loop experiment.

Reads only local files written by `aimj_p2_voice_loop` (results.jsonl), the
curated bank manifest, the 44-event fixture and the local WAV cache. It never
calls a model, never touches the network, and never fills in human judgments.

Usage:
  python -I benchmarks/p2_analyze.py
  python -I benchmarks/p2_analyze.py --out docs/experiments/2026-11-11-p2-voice-loop/metrics.json
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import wave
from collections import Counter, defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RUNS = ROOT / "benchmarks" / "results" / "p2" / "runs"
AUDIO = ROOT / "benchmarks" / "results" / "p2" / "audio" / "bank_audio"
DEFAULT_OUT = ROOT / "docs" / "experiments" / "2026-11-11-p2-voice-loop" / "metrics.json"
CHECKLIST = ROOT / "docs" / "experiments" / "2026-11-11-p2-voice-loop" / "listening-checklist.csv"
COMPARISON = ROOT / "docs" / "experiments" / "2026-11-11-p2-voice-loop" / "strategy-comparison.csv"


def read_tsv(path: Path) -> list[list[str]]:
    rows: list[list[str]] = []
    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            line = line.rstrip("\r\n")
            if not line or line.startswith("#"):
                continue
            rows.append(line.split("\t"))
    return rows


def read_jsonl(path: Path) -> list[dict]:
    rows: list[dict] = []
    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if line:
                rows.append(json.loads(line))
    return rows


def percentile(values: list[float], p: float) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    rank = (p / 100.0) * (len(ordered) - 1)
    lo = math.floor(rank)
    hi = math.ceil(rank)
    if lo == hi:
        return ordered[lo]
    return ordered[lo] + (ordered[hi] - ordered[lo]) * (rank - lo)


def r3(value: float | None) -> float | None:
    return None if value is None else round(value, 3)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def wav_stats(path: Path) -> dict:
    try:
        with wave.open(str(path), "rb") as wav:
            channels = wav.getnchannels()
            rate = wav.getframerate()
            width = wav.getsampwidth()
            frames = wav.getnframes()
            raw = wav.readframes(frames)
    except (wave.Error, OSError):
        return {"ok": False, "error": "unreadable wav"}
    if width != 2 or channels != 1 or not raw:
        return {
            "ok": True,
            "channels": channels,
            "sample_rate": rate,
            "sample_width": width,
            "duration_s": round(frames / rate, 4) if rate else None,
            "peak_dbfs": None,
            "rms_dbfs": None,
        }
    samples = memoryview(raw).cast("h")
    peak = max(abs(sample) for sample in samples)
    power = sum(sample * sample for sample in samples) / len(samples)
    peak_dbfs = 20.0 * math.log10(peak / 32768.0) if peak else None
    rms_dbfs = 20.0 * math.log10(math.sqrt(power) / 32768.0) if power else None
    return {
        "ok": True,
        "channels": channels,
        "sample_rate": rate,
        "sample_width": width,
        "duration_s": round(len(samples) / rate, 4),
        "peak_dbfs": r3(peak_dbfs),
        "rms_dbfs": r3(rms_dbfs),
    }


def load_bank(manifest: Path) -> tuple[list[dict], dict]:
    entries: list[dict] = []
    for row in read_tsv(manifest):
        if len(row) != 11:
            raise SystemExit(f"bank row has {len(row)} columns, expected 11: {row[:2]}")
        entries.append(
            {
                "line_id": row[0],
                "event_type": row[1],
                "fact_key": row[2],
                "speaker": int(row[3]),
                "persona_id": row[4],
                "emotion": row[5],
                "text": row[6],
                "tts_sid": int(row[7]),
                "audio_file": row[8],
                "audio_sha256": row[9],
                "weight": int(row[10]),
            }
        )
    index = {entry["line_id"]: entry for entry in entries}
    return entries, index


def load_run(name: str) -> dict:
    path = RUNS / name / "results.jsonl"
    if not path.is_file():
        return {"available": False, "path": str(path.relative_to(ROOT))}
    rows = read_jsonl(path)
    events = [row for row in rows if row.get("type") == "event"]
    summaries = [row for row in rows if row.get("type") == "summary"]
    return {
        "available": True,
        "path": str(path.relative_to(ROOT)),
        "events": events,
        "summary": summaries[-1] if summaries else None,
    }


def summarize_run(run: dict, bank_index: dict[str, dict]) -> dict:
    if not run.get("available"):
        return {"available": False, "path": run["path"]}
    events = run["events"]
    delivered = [event for event in events if event.get("approved_text")]
    methods = Counter(str(event.get("selection_method")) for event in events)
    validations = Counter(str(event.get("validation_result")) for event in events)
    persona_matches = sum(
        1
        for event in delivered
        if bank_index[event["selected_line_id"]]["persona_id"] == event["persona_id"]
    )
    sid_matches = sum(
        1
        for event in delivered
        if bank_index[event["selected_line_id"]]["tts_sid"] == event.get("tts_sid")
    )
    ids = [event["selected_line_id"] for event in delivered]
    repeats = len(ids) - len(set(ids))
    consecutive = 0
    longest = 0
    previous = None
    for line_id in ids:
        if line_id == previous:
            consecutive += 1
        else:
            consecutive = 1
            previous = line_id
        longest = max(longest, consecutive)
    pcm = [event["event_to_pcm_ms"] for event in delivered if isinstance(event.get("event_to_pcm_ms"), (int, float))]
    submit = [event["pcm_to_player_submit_ms"] for event in events if isinstance(event.get("pcm_to_player_submit_ms"), (int, float)) and event["pcm_to_player_submit_ms"] > 0]
    callback = [event["player_callback_ms"] for event in events if isinstance(event.get("player_callback_ms"), (int, float)) and event["player_callback_ms"] > 0]
    durations = [event["audio_duration_ms"] for event in events if isinstance(event.get("audio_duration_ms"), (int, float)) and event["audio_duration_ms"] > 0]
    summary = run.get("summary") or {}
    return {
        "available": True,
        "path": run["path"],
        "events_total": len(events),
        "delivered": len(delivered),
        "no_candidate": sum(1 for event in events if event.get("selection_method") == "no_candidate"),
        "validation_rejected": sum(1 for event in events if event.get("validation_result") == "rejected"),
        "factual_violations": summary.get("factual_violations"),
        "sid_mismatches": summary.get("sid_mismatches"),
        "persona_matches": persona_matches,
        "persona_match_rate": round(persona_matches / len(delivered), 4) if delivered else None,
        "sid_matches": sid_matches,
        "sid_match_rate": round(sid_matches / len(delivered), 4) if delivered else None,
        "unique_lines": len(set(ids)),
        "repeat_occurrences": repeats,
        "max_consecutive_same": longest,
        "methods": dict(methods),
        "validations": dict(validations),
        "event_to_pcm_ms": {
            "n": len(pcm),
            "p50": r3(percentile(pcm, 50)),
            "p95": r3(percentile(pcm, 95)),
            "max": r3(max(pcm)) if pcm else None,
        },
        "pcm_to_player_submit_ms": {
            "n": len(submit),
            "p50": r3(percentile(submit, 50)),
            "p95": r3(percentile(submit, 95)),
            "max": r3(max(submit)) if submit else None,
        },
        "player_callback_ms": {
            "n": len(callback),
            "p50": r3(percentile(callback, 50)),
            "p95": r3(percentile(callback, 95)),
            "max": r3(max(callback)) if callback else None,
        },
        "audio_duration_ms": {
            "n": len(durations),
            "p50": r3(percentile(durations, 50)),
            "p95": r3(percentile(durations, 95)),
            "max": r3(max(durations)) if durations else None,
        },
        "llm": summary.get("llm_calls") and {
            "calls": summary.get("llm_calls"),
            "valid": summary.get("llm_valid"),
            "invalid": summary.get("llm_invalid"),
            "p50_ms": summary.get("llm_p50_ms"),
            "p95_ms": summary.get("llm_p95_ms"),
        },
        "peak_rss_mb": summary.get("peak_rss_mb"),
        "process_cpu_ms": summary.get("process_cpu_ms"),
        "played": summary.get("played"),
        "first_audio_output_ms": None,
        "first_audible_ms": None,
        "first_audible_reason": summary.get("first_audible_reason"),
    }


def bank_audio(entries: list[dict]) -> dict:
    files: dict[str, dict] = {}
    mismatches: list[str] = []
    for entry in entries:
        name = entry["audio_file"]
        if name in files:
            continue
        path = AUDIO / name
        if not path.is_file():
            files[name] = {"ok": False, "error": "missing"}
            continue
        stats = wav_stats(path)
        stats["sha256_match"] = sha256_file(path) == entry["audio_sha256"]
        if not stats["sha256_match"]:
            mismatches.append(name)
        stats["bytes"] = path.stat().st_size
        files[name] = stats
    durations = [item["duration_s"] for item in files.values() if item.get("duration_s")]
    peaks = [item["peak_dbfs"] for item in files.values() if item.get("peak_dbfs") is not None]
    rms = [item["rms_dbfs"] for item in files.values() if item.get("rms_dbfs") is not None]
    return {
        "distinct_files": len(files),
        "missing": sum(1 for item in files.values() if not item.get("ok")),
        "sha256_mismatches": mismatches,
        "total_bytes": sum(item.get("bytes", 0) for item in files.values()),
        "total_duration_s": round(sum(durations), 3),
        "duration_s": {"min": r3(min(durations)) if durations else None, "max": r3(max(durations)) if durations else None},
        "peak_dbfs": {"min": r3(min(peaks)) if peaks else None, "max": r3(max(peaks)) if peaks else None},
        "rms_dbfs": {"min": r3(min(rms)) if rms else None, "max": r3(max(rms)) if rms else None},
        "sample_rates": sorted({item.get("sample_rate") for item in files.values() if item.get("sample_rate")}),
    }


def coverage(fixtures: Path, bank_entries: list[dict]) -> dict:
    keys = {(entry["event_type"], entry["fact_key"], entry["speaker"]) for entry in bank_entries}
    fixtures_rows = read_tsv(fixtures)
    missing = []
    for row in fixtures_rows:
        key = (row[2], row[9], int(row[6]))
        if key not in keys:
            missing.append({"event_id": row[0], "event_type": row[2], "fact_key": row[9], "speaker": int(row[6]), "note": row[11]})
    return {
        "fixtures": len(fixtures_rows),
        "covered": len(fixtures_rows) - len(missing),
        "missing": missing,
    }


def write_checklist(entries: list[dict]) -> None:
    CHECKLIST.parent.mkdir(parents=True, exist_ok=True)
    columns = [
        "line_id", "speaker", "persona_id", "emotion", "tts_sid", "text", "audio_file",
        "duration_s", "peak_dbfs", "rms_dbfs",
        "mahjong_pronunciation_ok", "clarity_ok", "rhythm_ok",
        "voice_separation_ok", "volume_ok", "repeat_willing", "notes",
    ]
    with CHECKLIST.open("w", encoding="utf-8-sig", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=columns)
        writer.writeheader()
        for entry in entries:
            stats = wav_stats(AUDIO / entry["audio_file"])
            writer.writerow(
                {
                    "line_id": entry["line_id"],
                    "speaker": entry["speaker"],
                    "persona_id": entry["persona_id"],
                    "emotion": entry["emotion"],
                    "tts_sid": entry["tts_sid"],
                    "text": entry["text"],
                    "audio_file": entry["audio_file"],
                    "duration_s": stats.get("duration_s"),
                    "peak_dbfs": stats.get("peak_dbfs"),
                    "rms_dbfs": stats.get("rms_dbfs"),
                    "mahjong_pronunciation_ok": "",
                    "clarity_ok": "",
                    "rhythm_ok": "",
                    "voice_separation_ok": "",
                    "volume_ok": "",
                    "repeat_willing": "",
                    "notes": "",
                }
            )


def write_comparison(strategies: dict) -> None:
    columns = [
        "strategy", "events", "delivered", "no_candidate", "validation_rejected",
        "factual_violations", "sid_mismatches", "persona_match_rate", "unique_lines",
        "repeat_occurrences", "max_consecutive_same", "event_to_pcm_p50_ms",
        "event_to_pcm_p95_ms", "llm_calls", "llm_valid", "llm_invalid",
        "llm_p50_ms", "llm_p95_ms", "peak_rss_mb", "process_cpu_ms",
        "human_fun_rating",
    ]
    with COMPARISON.open("w", encoding="utf-8-sig", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=columns)
        writer.writeheader()
        for name, data in strategies.items():
            if not data.get("available"):
                continue
            llm = data.get("llm") or {}
            writer.writerow(
                {
                    "strategy": name,
                    "events": data["events_total"],
                    "delivered": data["delivered"],
                    "no_candidate": data["no_candidate"],
                    "validation_rejected": data["validation_rejected"],
                    "factual_violations": data["factual_violations"],
                    "sid_mismatches": data["sid_mismatches"],
                    "persona_match_rate": data["persona_match_rate"],
                    "unique_lines": data["unique_lines"],
                    "repeat_occurrences": data["repeat_occurrences"],
                    "max_consecutive_same": data["max_consecutive_same"],
                    "event_to_pcm_p50_ms": data["event_to_pcm_ms"]["p50"],
                    "event_to_pcm_p95_ms": data["event_to_pcm_ms"]["p95"],
                    "llm_calls": llm.get("calls", 0),
                    "llm_valid": llm.get("valid", 0),
                    "llm_invalid": llm.get("invalid", 0),
                    "llm_p50_ms": llm.get("p50_ms", 0),
                    "llm_p95_ms": llm.get("p95_ms", 0),
                    "peak_rss_mb": data["peak_rss_mb"],
                    "process_cpu_ms": data["process_cpu_ms"],
                    "human_fun_rating": "",
                }
            )


def write_audio_index(entries: list[dict]) -> Path:
    path = AUDIO / "index.html"
    rows = []
    for entry in entries:
        rows.append(
            f'<tr><td>{entry["line_id"]}</td><td>{entry["speaker"]}</td>'
            f'<td>{entry["persona_id"]}/{entry["emotion"]}</td>'
            f'<td>{entry["text"]}</td>'
            f'<td><audio controls preload="none" src="{entry["audio_file"]}"></audio></td></tr>'
        )
    path.write_text(
        "<!doctype html><meta charset=\"utf-8\"><title>P2 bank audio index</title>"
        "<style>body{font-family:sans-serif;margin:2rem}td{padding:.3rem .6rem;border-bottom:1px solid #ddd}</style>"
        "<h1>P2 bank audio index (local only)</h1>"
        "<p>One row per curated bank line. This file is generated under the gitignored results directory.</p>"
        "<table><tr><th>line_id</th><th>speaker</th><th>persona/emotion</th><th>text</th><th>audio</th></tr>"
        + "".join(rows)
        + "</table>",
        encoding="utf-8",
    )
    return path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    parser.add_argument("--manifest", type=Path, default=ROOT / "benchmarks" / "p2_bank_manifest.tsv")
    parser.add_argument("--fixtures", type=Path, default=ROOT / "benchmarks" / "p2_bench_fixtures.tsv")
    args = parser.parse_args()

    entries, index = load_bank(args.manifest)
    runs = {name: load_run(name) for name in ("fixed", "weighted", "llm", "play-fixed", "play-test")}
    strategies = {name: summarize_run(runs[name], index) for name in ("fixed", "weighted", "llm")}

    by_speaker = Counter(entry["speaker"] for entry in entries)
    by_sid = Counter(entry["tts_sid"] for entry in entries)
    by_event = Counter(entry["event_type"] for entry in entries)
    weights = [entry["weight"] for entry in entries]

    metrics = {
        "schema": 1,
        "experiment": "p2-voice-loop",
        "source": "local Windows run; no model or network calls in this analyzer",
        "bank": {
            "entries": len(entries),
            "by_speaker": dict(sorted(by_speaker.items())),
            "by_sid": dict(sorted(by_sid.items())),
            "by_event_type": dict(sorted(by_event.items())),
            "weight_min": min(weights),
            "weight_max": max(weights),
            "exact_key_required": True,
        },
        "coverage": coverage(args.fixtures, entries),
        "strategies": strategies,
        "playback": summarize_run(runs["play-fixed"], index),
        "playback_smoke": summarize_run(runs["play-test"], index),
        "audio": bank_audio(entries),
        "human": {
            "listening": "pending",
            "fun_rating": None,
            "source": str(CHECKLIST.relative_to(ROOT)),
            "note": "No listening score or fun rating is recorded by this script; only a human can fill these.",
        },
        "timing_semantics": {
            "event_to_pcm_ms": "event handling + bank selection + SHA-256 + WAV read; excludes playback",
            "pcm_to_player_submit_ms": "waveOutOpen/Prepare/Write latency",
            "player_callback_ms": "time until WOM_DONE for the whole buffer; not audible onset",
            "first_audio_output_ms": None,
            "first_audible_ms": None,
        },
    }

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(metrics, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    write_checklist(entries)
    write_comparison(strategies)
    audio_index = write_audio_index(entries)
    print(f"wrote {args.out}")
    print(f"wrote {CHECKLIST}")
    print(f"wrote {COMPARISON}")
    print(f"wrote {audio_index}")
    print(json.dumps({"coverage": metrics["coverage"], "audio": metrics["audio"], "playback": {k: metrics["playback"].get(k) for k in ("delivered", "played", "event_to_pcm_ms", "pcm_to_player_submit_ms", "player_callback_ms")}}, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
