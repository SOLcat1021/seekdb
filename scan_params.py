"""One-factor parameter sweep for the deterministic local workload.

Results are diagnostic, not an estimate of the competition score.
"""

from __future__ import annotations

import json
import os
import platform
import statistics
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).parent
MIB = 1024 * 1024
REPEATS = 2
QUERY_COUNT = 300
BASE = {
    # Preserve the original level-1 baseline so the saved sweep is reproducible.
    "MVD_VECTOR_CACHE_BYTES": str(16 * MIB),
    "MVD_BUCKETS": "32768",
    "MVD_WAL_COMPRESSION_LEVEL": "1",
    "MVD_WAL_COMPRESSION_THRESHOLD": "1024",
}
SWEEPS = [
    ("cache_mib", "MVD_VECTOR_CACHE_BYTES", [8, 12, 13, 14, 16, 24], MIB),
    ("buckets", "MVD_BUCKETS", [1024, 2048, 4096, 8192, 16384, 32768, 65536], 1),
    ("wal_level", "MVD_WAL_COMPRESSION_LEVEL", [0, 1, 2, 3, 4, 6, 9], 1),
]


def probe(settings: dict[str, str]) -> dict:
    env = os.environ.copy()
    env.update(settings)
    env.update({"MVD_PROBE_JSON": "1", "MVD_PROBE_QUERIES": str(QUERY_COUNT)})
    completed = subprocess.run(
        [sys.executable, str(ROOT / "run_perf_probe.py")],
        cwd=ROOT, env=env, capture_output=True, text=True, check=True,
    )
    return json.loads(completed.stdout.strip().splitlines()[-1])


def main() -> int:
    output = {
        "platform": platform.platform(), "python": sys.version.split()[0],
        "query_count": QUERY_COUNT, "repeats": REPEATS, "baseline": BASE,
        "measurements": [],
    }
    for name, env_name, values, multiplier in SWEEPS:
        for value in values:
            settings = dict(BASE)
            settings[env_name] = str(value * multiplier)
            trials = [probe(settings) for _ in range(REPEATS)]
            summary = {
                "write_seconds": statistics.median(t["write_seconds"] for t in trials),
                "query_seconds": statistics.median(t["query_seconds"] for t in trials),
                "wal_mib": statistics.median(t["wal_mib"] for t in trials),
                "peak_rss_mib": statistics.median(t["peak_rss_mib"] for t in trials),
                "native_scan_active": all(t["native_scan_active"] for t in trials),
            }
            output["measurements"].append({
                "parameter": name, "value": value, "settings": settings,
                "summary": summary, "trials": trials,
            })
            print(
                f"{name}={value:<7} write={summary['write_seconds']:.3f}s "
                f"query={summary['query_seconds']:.3f}s WAL={summary['wal_mib']:.2f}MiB "
                f"RSS={summary['peak_rss_mib']:.1f}MiB native={summary['native_scan_active']}",
                flush=True,
            )
    path = ROOT / "param_scan_results.json"
    path.write_text(json.dumps(output, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"saved {path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
