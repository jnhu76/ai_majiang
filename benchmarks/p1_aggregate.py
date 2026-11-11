#!/usr/bin/env python3
"""Compatibility alias for the plan's p1_aggregate.py entry point.

The authoritative aggregator is p1_analyze.py, whose CLI and metrics schema are
fixed by the P1 measurement contract. This shim forwards arguments so the older
name keeps working without a second implementation drifting out of sync.

  python -I benchmarks/p1_aggregate.py --root benchmarks/results/p1
"""

import pathlib
import runpy
import sys

sys.dont_write_bytecode = True

if __name__ == "__main__":
    target = pathlib.Path(__file__).resolve().with_name("p1_analyze.py")
    runpy.run_path(str(target), run_name="__main__")
