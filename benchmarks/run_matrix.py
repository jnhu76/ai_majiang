#!/usr/bin/env python3
"""Repeat mock pressure runs only. Real runs require native providers and device tools."""
import argparse
import json
import pathlib
import subprocess
import csv
p=argparse.ArgumentParser()
p.add_argument("--exe", default="build/aimj_bench")
p.add_argument("--out", default="benchmarks/results")
p.add_argument("--events", type=int, default=80)
p.add_argument("--repeats", type=int, default=3)
a=p.parse_args()
out=pathlib.Path(a.out)
out.mkdir(parents=True, exist_ok=True)
rows=[]
for hz in (2,5,10,20):
    for llm_ms in (50,200,500):
        for rep in range(a.repeats):
            name=f"mock_hz{hz}_llm{llm_ms}_r{rep}.jsonl"
            cmd=[a.exe,"--events",str(a.events),"--hz",str(hz),"--llm-delay-ms",str(llm_ms),
                 "--tts-delay-ms","60","--queue","4","--deadline-ms","2000", "--out",str(out/name)]
            result=subprocess.run(cmd, capture_output=True,text=True)
            if result.returncode not in (0,2):
                raise RuntimeError(f"failed {cmd}: {result.stderr}")
            summary=json.loads((out/name).read_text(encoding="utf-8").splitlines()[-1])
            summary.update({"hz":hz,"llm_mock_delay_ms":llm_ms,"repeat":rep})
            rows.append(summary)
with (out/"matrix.csv").open("w",newline="",encoding="utf-8") as f:
    writer=csv.DictWriter(f,fieldnames=list(rows[0]))
    writer.writeheader();writer.writerows(rows)
print("Wrote",out/"matrix.csv","(MOCK ONLY; not actual LLM throughput)")
