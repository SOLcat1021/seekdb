"""Deterministic 10k x 128 x 1000 local probe, not an official benchmark."""

from __future__ import annotations

import json
import math
import os
import random
import resource
import tempfile
import time
from pathlib import Path

from minivectordb import MiniVectorDB, NATIVE_SCAN, as_value


def cgroup_sample() -> dict[str, int] | None:
    """Read cgroup v2 totals for the current process, if available."""
    entries = Path("/proc/self/cgroup").read_text().splitlines()
    unified = next((line.split(":", 2)[2] for line in entries if line.startswith("0::")), None)
    if unified is None:
        return None
    directory = Path("/sys/fs/cgroup") / unified.lstrip("/")
    if not (directory / "io.stat").exists() or not (directory / "memory.peak").exists():
        return None
    sample = {"rbytes": 0, "wbytes": 0}
    for line in (directory / "io.stat").read_text().splitlines():
        for field in line.split()[1:]:
            name, _, value = field.partition("=")
            if name in sample:
                sample[name] += int(value)
    sample["memory_peak"] = int((directory / "memory.peak").read_text())
    return sample


def main() -> int:
    if NATIVE_SCAN is None:
        raise RuntimeError("build libmvdscan.so first: make libmvdscan.so")
    generator = random.Random(20261109)
    queries = random.Random(20261110)
    query_count = int(os.environ.get("MVD_PROBE_QUERIES", "1000"))
    batch_size = int(os.environ.get("MVD_PROBE_BATCH", "100"))
    if query_count < 4 or batch_size < 1:
        raise ValueError("probe requires at least four queries and a positive batch size")
    measure_cgroup = os.environ.get("MVD_PROBE_CGROUP") == "1"
    cgroup_before = cgroup_sample() if measure_cgroup else None
    if measure_cgroup and cgroup_before is None:
        raise RuntimeError("cgroup v2 io.stat and memory.peak are required")
    with tempfile.TemporaryDirectory(prefix="mvd-perf-") as directory:
        database = MiniVectorDB(directory)
        database.execute("CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(8), embedding VECTOR(128));")
        started = time.perf_counter()
        for begin in range(0, 10000, batch_size):
            batch = []
            for i in range(begin, min(begin + batch_size, 10000)):
                vector = [round(generator.uniform(-1, 1), 4) for _ in range(128)]
                batch.append(f"({i}, '{'a' if i % 2 else 'b'}', '{json.dumps(vector, separators=(',', ':'))}')")
            values = ",".join(batch)
            assert database.execute(f"INSERT INTO docs VALUES {values};") == {"affected_rows": len(batch)}
        write_time = time.perf_counter() - started
        if measure_cgroup:
            os.sync()
            cgroup_after_write = cgroup_sample()
        table = database.tables["docs"]
        results = []
        started = time.perf_counter()
        for number in range(query_count):
            query = [round(queries.uniform(-1, 1), 4) for _ in range(128)]
            target = json.dumps(query, separators=(",", ":"))
            filter_sql = " WHERE category = 'a'" if number % 2 else ""
            sql = f"SELECT id, L2_DISTANCE(embedding, '{target}') AS distance FROM docs{filter_sql} ORDER BY distance LIMIT 10;"
            result = database.execute(sql)
            assert len(result["rows"]) == 10
            if number < 4:
                results.append((number, query, result["rows"]))
        query_time = time.perf_counter() - started
        if measure_cgroup:
            os.sync()
            cgroup_after_query = cgroup_sample()
        for number, query, observed in results:
            target = as_value(json.dumps(query), table["columns"][2])
            reference = sorted(
                (
                    (math.dist(row[2], target), row[0])
                for row in (table["rows"].cache.values() if table["rows"].cache is not None
                            else table["rows"].values())
                    if number % 2 == 0 or row[1] == "a"
                )
            )[:10]
            assert [row[0] for row in observed] == [key for _, key in reference]
            assert all(math.isclose(row[1], dist, rel_tol=1e-5, abs_tol=1e-5) for row, (dist, _) in zip(observed, reference))
        data_file = Path(directory) / "materialized" / "docs.dat"
        peak_rss = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss / 1024
        metrics = {
            "rows": 10000, "dimension": 128, "queries": query_count, "batch": batch_size,
            "write_seconds": write_time, "query_seconds": query_time,
            "data_mib": data_file.stat().st_size / 2**20,
            "wal_mib": (Path(directory) / "commit.wal").stat().st_size / 2**20,
            "peak_rss_mib": peak_rss,
            "vector_cache_active": table["rows"].cache is not None,
            "native_scan_active": table["rows"].native_vectors is not None,
        }
        if measure_cgroup:
            assert cgroup_before is not None and cgroup_after_write is not None and cgroup_after_query is not None
            metrics.update({
                "cgroup_write_io_mib": sum(cgroup_after_write[key] - cgroup_before[key] for key in ("rbytes", "wbytes")) / 2**20,
                "cgroup_query_io_mib": sum(cgroup_after_query[key] - cgroup_after_write[key] for key in ("rbytes", "wbytes")) / 2**20,
                "cgroup_peak_mib": cgroup_after_query["memory_peak"] / 2**20,
            })
        if os.environ.get("MVD_PROBE_JSON") == "1":
            print(json.dumps(metrics, sort_keys=True))
        else:
            print(f"PASS 10000 rows x 128 dimensions; write={write_time:.3f}s, {query_count} queries={query_time:.3f}s")
            print(f"data={metrics['data_mib']:.2f} MiB, WAL={metrics['wal_mib']:.2f} MiB, peak RSS={peak_rss:.1f} MiB")
            if measure_cgroup:
                print(f"cgroup I/O write={metrics['cgroup_write_io_mib']:.2f} MiB, query={metrics['cgroup_query_io_mib']:.2f} MiB; peak memory={metrics['cgroup_peak_mib']:.1f} MiB")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
