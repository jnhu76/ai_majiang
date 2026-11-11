#!/usr/bin/env python3
"""P1 evidence aggregator (standard library only, Python 3.12).

Reads only the four evidence trees below --root and never pools mock, mixed,
smoke, interaction or hybrid samples into the configuration metrics:

  dialogue/<run_id>/results.jsonl      -> configs   (mode == "real" only)
  interaction/<scenario>/results.jsonl -> interaction
  hybrid/<approach>/results.jsonl      -> hybrid
  audio/tts_real/results.jsonl         -> tts

Configuration rows additionally require per-row status == "ok" and
usage_valid == true, i.e. a successful call from a non-mock provider. Rows that
failed, expired, went stale or were rejected are counted separately and never
enter p50/p95/token-rate statistics.

Usage:
  python -I benchmarks/p1_analyze.py --root benchmarks/results/p1
  python -I benchmarks/p1_analyze.py --root benchmarks/results/p1 --merge-reviews
  python -I benchmarks/p1_analyze.py --root benchmarks/results/p1 --samples FILE

The scan also exports every accepted dialogue run to results-samples.jsonl
(run header first, then its case/summary rows), so the README round trip
`--samples benchmarks/results/p1/results-samples.jsonl` can re-derive the same
configs from the exported evidence alone. --samples evaluates such a JSONL
(one or more runs, each opened by its "run" header) instead of scanning
dialogue/, for schema regression.

--merge-reviews joins review/group-*.csv (fixed header) into dialogue-review.csv
and appends human_confirmation=pending.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import pathlib
import statistics
import sys

SCHEMA = 1
REVIEW_HEADER = [
    "case_id", "strategy", "persona", "generated_text", "factual",
    "length_ok", "natural_1_5", "persona_1_5", "note",
]
REVIEW_INPUT_HEADER = [
    "case_id", "strategy", "persona", "speaker", "config", "prompt_text",
    "generated_text", "factual_hint", "required_kw", "forbidden_kw",
]


def read_jsonl(path: pathlib.Path) -> list[dict]:
    rows: list[dict] = []
    if not path.is_file():
        return rows
    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if not line:
                continue
            try:
                value = json.loads(line)
            except json.JSONDecodeError:
                continue
            if isinstance(value, dict):
                rows.append(value)
    return rows


def percentile(values: list[float], p: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(float(v) for v in values)
    if len(ordered) == 1:
        return ordered[0]
    rank = (p / 100.0) * (len(ordered) - 1)
    lo = math.floor(rank)
    hi = math.ceil(rank)
    if lo == hi:
        return ordered[lo]
    return ordered[lo] + (ordered[hi] - ordered[lo]) * (rank - lo)


def pct(part: float, whole: float) -> float:
    return round(100.0 * part / whole, 3) if whole else 0.0


def r3(value: float) -> float:
    return round(float(value), 3)


def config_of(header: dict) -> str:
    name = header.get("config")
    if name:
        return str(name)
    backend = header.get("mnn_backend_requested") or header.get("mnn_backend") or ""
    threads = header.get("mnn_threads") or 0
    if backend == "opencl":
        return "opencl"
    if threads:
        return f"cpu{threads}"
    return "unknown"


def header_of(rows: list[dict]) -> dict | None:
    for row in rows:
        if row.get("type") == "run":
            return row
    return None


def is_real(header: dict | None) -> bool:
    return bool(header) and header.get("mode") == "real"


def keep_run(header: dict | None) -> bool:
    # Same acceptance rule for the dialogue/ scan and the samples round trip.
    return is_real(header) and header.get("battery") in (None, "dialogue")


def split_runs(rows: list[dict]) -> list[tuple[dict, list[dict]]]:
    # A samples file may hold several runs back to back; each "run" header
    # opens a new one. Rows before the first header attach to that first run.
    runs: list[tuple[dict, list[dict]]] = []
    header: dict | None = None
    pre: list[dict] = []
    body: list[dict] = []
    for row in rows:
        if row.get("type") == "run":
            if header is not None:
                runs.append((header, body))
            header, body = row, list(pre)
            pre = []
        elif header is not None:
            body.append(row)
        else:
            pre.append(row)
    if header is not None:
        runs.append((header, body))
    return runs


def write_samples(root: pathlib.Path, runs: list[tuple[dict, list[dict]]]) -> pathlib.Path:
    # Schema-regression export: every row of every accepted dialogue run, the
    # run header written once and first, so --samples can re-derive configs
    # from the exported evidence alone. Original results.jsonl stay untouched.
    out = root / "results-samples.jsonl"
    root.mkdir(parents=True, exist_ok=True)
    with out.open("w", encoding="utf-8", newline="\n") as handle:
        for header, rows in runs:
            for row in [header] + [r for r in rows if r is not header]:
                handle.write(json.dumps(row, ensure_ascii=False) + "\n")
    return out


def formal_cases(rows: list[dict]) -> list[dict]:
    return [
        row for row in rows
        if row.get("type") == "case" and not row.get("warmup", False)
    ]


def load_dialogue_runs(root: pathlib.Path) -> list[tuple[dict, list[dict]]]:
    runs: list[tuple[dict, list[dict]]] = []
    dialogue_dir = root / "dialogue"
    if not dialogue_dir.is_dir():
        return runs
    for run_dir in sorted(dialogue_dir.iterdir()):
        path = run_dir / "results.jsonl"
        if not path.is_file():
            continue
        rows = read_jsonl(path)
        header = header_of(rows)
        if not keep_run(header):
            continue
        runs.append((header, rows))
    return runs


def merge_configs(runs: list[tuple[dict, list[dict]]]) -> list[dict]:
    # Pool the raw rows of every run of the same config first; averaging
    # per-run percentiles would be a statistic of statistics.
    per_config: dict[str, list[tuple[dict, list[dict]]]] = {}
    for header, rows in runs:
        per_config.setdefault(config_of(header), []).append((header, rows))
    merged: list[dict] = []
    for name in sorted(per_config):
        formal: list[dict] = []
        ok_rows: list[dict] = []
        summaries: list[dict] = []
        unresolved = 0
        for header, rows in per_config[name]:
            formal.extend(formal_cases(rows))
            ok_rows.extend(
                row for row in formal_cases(rows)
                if row.get("status") == "ok" and row.get("usage_valid") is True
                and row.get("mode", header.get("mode")) == "real"
            )
            summaries.extend(row for row in rows if row.get("type") == "summary")
            unresolved += max((int(row.get("unresolved") or 0) for row in rows
                               if row.get("type") == "summary"), default=0)
        pcm = [row["end_to_end_pcm_ms"] for row in ok_rows
               if isinstance(row.get("end_to_end_pcm_ms"), (int, float))]
        ttfc = [row["llm_ttfc_ms"] for row in ok_rows
                if isinstance(row.get("llm_ttfc_ms"), (int, float))]
        prompt_tokens = sum(int(row.get("prompt_tokens") or 0) for row in ok_rows)
        gen_tokens = sum(int(row.get("generated_tokens") or 0) for row in ok_rows)
        prefill_ms = sum(float(row.get("prefill_ms") or 0) for row in ok_rows)
        decode_ms = sum(float(row.get("decode_ms") or 0) for row in ok_rows)
        peak_rss = max((int(row.get("peak_rss_bytes") or 0) for row in summaries), default=0)
        offered = len(formal) + unresolved
        merged.append({
            "config": name,
            "offered": offered,
            "ok": len(ok_rows),
            "failed": max(offered - len(ok_rows), 0),
            "p50_pcm_ms": r3(percentile(pcm, 50)),
            "p95_pcm_ms": r3(percentile(pcm, 95)),
            "ttfc_p50_ms": r3(percentile(ttfc, 50)),
            "prefill_tok_s": r3(prompt_tokens / (prefill_ms / 1000.0)) if prefill_ms > 0 else 0.0,
            "decode_tok_s": r3(gen_tokens / (decode_ms / 1000.0)) if decode_ms > 0 else 0.0,
            "peak_rss_mb": r3(peak_rss / (1024.0 * 1024.0)),
            "reject_pct": pct(sum(1 for row in formal if row.get("status") == "queue_rejected"), offered),
            "expire_pct": pct(sum(1 for row in formal if row.get("status") == "timeout"), offered),
            "stale_pct": pct(sum(1 for row in formal if row.get("status") == "stale"), offered),
        })
    return merged


def aggregate_tts(root: pathlib.Path) -> dict:
    path = root / "audio" / "tts_real" / "results.jsonl"
    rows = read_jsonl(path)
    header = header_of(rows)
    lines = int(header.get("tts_lines") or 0) if header else 0
    if not lines:
        lines = sum(1 for row in rows if row.get("type") == "tts_line")
    ok_rows = [row for row in rows if row.get("type") == "tts_line" and row.get("status") == "ok"]
    rates = [int(row.get("sample_rate") or 0) for row in ok_rows if row.get("sample_rate")]
    rate = statistics.mode(rates) if rates else 0
    return {"lines": lines, "ok": len(ok_rows), "sample_rate": int(rate)}


def aggregate_interaction(root: pathlib.Path) -> list[dict]:
    result: list[dict] = []
    base = root / "interaction"
    if not base.is_dir():
        return result
    for scenario_dir in sorted(base.iterdir()):
        path = scenario_dir / "results.jsonl"
        if not path.is_file():
            continue
        rows = read_jsonl(path)
        header = header_of(rows)
        formal = formal_cases(rows) if is_real(header) else []
        pcm = [row["end_to_end_pcm_ms"] for row in formal
               if row.get("status") == "ok" and isinstance(row.get("end_to_end_pcm_ms"), (int, float))]
        offered = len(formal)
        result.append({
            "scenario": scenario_dir.name,
            "e2e_p95_ms": r3(percentile(pcm, 95)),
            "reject_pct": pct(sum(1 for row in formal if row.get("status") == "queue_rejected"), offered),
            "expire_pct": pct(sum(1 for row in formal if row.get("status") == "timeout"), offered),
        })
    return result


def aggregate_hybrid(root: pathlib.Path) -> list[dict]:
    result: list[dict] = []
    base = root / "hybrid"
    if not base.is_dir():
        return result
    for approach_dir in sorted(base.iterdir()):
        path = approach_dir / "results.jsonl"
        if not path.is_file():
            continue
        rows = read_jsonl(path)
        header = header_of(rows)
        formal = formal_cases(rows) if is_real(header) else []
        ok_rows = [row for row in formal if row.get("status") == "ok"]
        pcm = [row["end_to_end_pcm_ms"] for row in ok_rows
               if isinstance(row.get("end_to_end_pcm_ms"), (int, float))]
        result.append({
            "approach": approach_dir.name,
            "p95_pcm_ms": r3(percentile(pcm, 95)),
            "factual_ok": sum(1 for row in ok_rows if row.get("factual_ok") is True),
            "total": len(formal),
        })
    return result


def write_review_input(root: pathlib.Path, runs: list[tuple[dict, list[dict]]]) -> pathlib.Path:
    # One row per (config, case, strategy): the final attempt, so a retried line
    # is reviewed instead of its rejected first draft.
    latest: dict[tuple[str, str, str], dict] = {}
    for header, rows in runs:
        config = config_of(header)
        for row in formal_cases(rows):
            if not row.get("generated_text"):
                continue
            key = (config, str(row.get("id") or ""), str(row.get("strategy") or ""))
            previous = latest.get(key)
            if previous is None or int(row.get("attempt") or 1) >= int(previous.get("attempt") or 1):
                decorated = dict(row)
                decorated["_config"] = config
                latest[key] = decorated
    out = root / "dialogue-review-input.tsv"
    root.mkdir(parents=True, exist_ok=True)
    with out.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle, delimiter="\t", lineterminator="\n")
        writer.writerow(REVIEW_INPUT_HEADER)
        for key in sorted(latest):
            row = latest[key]
            def clean(value) -> str:
                return str(value or "").replace("\t", " ").replace("\r", " ").replace("\n", " ")
            writer.writerow([
                clean(row.get("id")), clean(row.get("strategy")), clean(row.get("persona")),
                row.get("speaker", ""), key[0], clean(row.get("prompt_text")),
                clean(row.get("generated_text")), clean(row.get("expected_review_hint")),
                clean(row.get("required_kw")), clean(row.get("forbidden_kw")),
            ])
    return out


def merge_reviews(root: pathlib.Path) -> pathlib.Path:
    out = root / "dialogue-review.csv"
    review_dir = root / "review"
    files = sorted(review_dir.glob("group-*.csv")) if review_dir.is_dir() else []
    root.mkdir(parents=True, exist_ok=True)
    with out.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle, lineterminator="\n")
        writer.writerow(REVIEW_HEADER + ["human_confirmation"])
        for path in files:
            with path.open("r", encoding="utf-8-sig", newline="") as source:
                reader = csv.DictReader(source)
                if reader.fieldnames != REVIEW_HEADER:
                    raise SystemExit(
                        f"{path}: expected header {','.join(REVIEW_HEADER)}, got {reader.fieldnames}")
                for row in reader:
                    writer.writerow([row[name] for name in REVIEW_HEADER] + ["pending"])
    return out


def repo_confined(raw: str, option: str) -> pathlib.Path:
    """Normalize a CLI path and confine it to this repository.

    The aggregator only ever reads and writes repo-local evidence trees, so a
    mistyped or hostile --root/--samples must not read or write outside the
    repo (path traversal). Normalization resolves '..' and symlinks; '..'
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
    parser = argparse.ArgumentParser(description="Aggregate P1 probe evidence (stdlib only).")
    parser.add_argument("--root", required=True, type=lambda v: repo_confined(v, "--root"),
                        help="benchmarks/results/p1")
    parser.add_argument("--merge-reviews", action="store_true",
                        help="merge review/group-*.csv into dialogue-review.csv")
    parser.add_argument("--samples", type=lambda v: repo_confined(v, "--samples"),
                        help="explicit JSONL (run header + case rows) for configs")
    args = parser.parse_args(argv)
    root = args.root

    if args.samples:
        sample_path = args.samples
        if not sample_path.is_file():
            raise SystemExit(f"samples file not found: {sample_path}")
        runs = [run for run in split_runs(read_jsonl(sample_path)) if keep_run(run[0])]
    else:
        runs = load_dialogue_runs(root)

    metrics = {
        "schema": SCHEMA,
        "configs": merge_configs(runs),
        "tts": aggregate_tts(root),
        "interaction": aggregate_interaction(root),
        "hybrid": aggregate_hybrid(root),
    }
    root.mkdir(parents=True, exist_ok=True)
    metrics_path = root / "metrics.json"
    metrics_path.write_text(json.dumps(metrics, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    review_input = write_review_input(root, runs)
    samples_path = None
    if not args.samples:
        samples_path = write_samples(root, runs)
    print(f"wrote {metrics_path}")
    print(f"wrote {review_input}")
    if samples_path:
        print(f"wrote {samples_path}")
    if args.merge_reviews:
        merged = merge_reviews(root)
        print(f"wrote {merged}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
