"""Run the local probe in three independent systemd cgroup v2 scopes."""

from __future__ import annotations

import json
import statistics
import subprocess
import sys


METRICS = (
    "write_seconds",
    "query_seconds",
    "cgroup_write_io_mib",
    "cgroup_query_io_mib",
    "cgroup_peak_mib",
)


def main() -> int:
    rounds = []
    for number in range(1, 4):
        command = [
            "systemd-run", "--scope", "--collect", "--quiet",
            "-p", "IOAccounting=yes", "-p", "MemoryAccounting=yes",
            "env", "TMPDIR=/var/tmp", "MVD_PROBE_CGROUP=1", "MVD_PROBE_JSON=1",
            sys.executable, "run_perf_probe.py",
        ]
        result = subprocess.run(command, capture_output=True, text=True, check=True)
        measured = json.loads(result.stdout)
        rounds.append(measured)
        print(f"round {number}: " + ", ".join(f"{key}={measured[key]:.3f}" for key in METRICS))
    medians = {key: statistics.median(item[key] for item in rounds) for key in METRICS}
    print("median: " + ", ".join(f"{key}={medians[key]:.3f}" for key in METRICS))
    print("Local API probe only; these values do not yield an official E score.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
