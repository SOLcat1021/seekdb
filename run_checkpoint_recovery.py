"""Check checkpoint crash recovery and compare startup to the WAL-only engine."""

from __future__ import annotations

import argparse
import json
import os
import random
import shutil
import signal
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parent
PROGRAM = ROOT / "minivectordb.py"
BASELINE = "76e637d"
NO_CHECKPOINT = 0

PAUSE_AFTER_FIRST_REDO = """
import runpy
import sys
import time
from pathlib import Path

program, directory, ready = sys.argv[1:]
engine = runpy.run_path(program)["MiniVectorDB"]
apply = engine._apply

def pause_after_apply(self, parser, *args, **kwargs):
    keyword = parser.peek()[1].upper()
    result = apply(self, parser, *args, **kwargs)
    if keyword == "UPDATE" and kwargs.get("materialize"):
        Path(ready).touch()
        time.sleep(120)
    return result

engine._apply = pause_after_apply
engine(directory).close()
"""

PAUSE_DURING_CHECKPOINT = """
import os
import runpy
import sys
import time
from pathlib import Path

program, directory, ready, stage, filename = sys.argv[1:]
engine = runpy.run_path(program)["MiniVectorDB"]
database = engine(directory)
assert database.execute("INSERT INTO docs VALUES (5, 'at publish', '[8,0]');")["affected_rows"] == 1
replace = os.replace

def pause():
    Path(ready).touch()
    time.sleep(120)

def pause_at_replace(source, destination, *args, **kwargs):
    target = Path(destination).name == filename
    if target and stage == "before":
        pause()
    result = replace(source, destination, *args, **kwargs)
    if target and stage == "after":
        pause()
    return result

os.replace = pause_at_replace
database._write_checkpoint()
"""

MEASURE_INIT = """
import json
import runpy
import sys
import time

program, directory, last_key = sys.argv[1:]
engine = runpy.run_path(program)["MiniVectorDB"]
start = time.perf_counter()
database = engine(directory)
elapsed = time.perf_counter() - start
assert database.execute(f"SELECT id FROM docs WHERE id = {last_key};")["rows"] == [[int(last_key)]]
query = json.dumps([0] * 128)
topk_started = time.perf_counter()
topk = database.execute(f"SELECT id, L2_DISTANCE(embedding, '{query}') AS d FROM docs ORDER BY d LIMIT 10;")["rows"]
topk_elapsed = time.perf_counter() - topk_started
database.close()
print(json.dumps({"startup": elapsed, "first_topk": topk_elapsed, "topk_keys": [row[0] for row in topk]}))
"""


def env(checkpoint_bytes: int | None) -> dict[str, str]:
    settings = os.environ.copy()
    settings.pop("MVD_CHECKPOINT_BYTES", None)
    if checkpoint_bytes is not None:
        settings["MVD_CHECKPOINT_BYTES"] = str(checkpoint_bytes)
    return settings


def run_sql(program: Path, directory: Path, statements: list[str], threshold: int | None) -> list[dict]:
    result = subprocess.run(
        [sys.executable, str(program), "--data-dir", str(directory)],
        input="\n".join(statements) + "\n", capture_output=True, encoding="utf-8",
        env=env(threshold), timeout=120,
    )
    if result.returncode:
        raise AssertionError(result.stderr)
    responses = [json.loads(line) for line in result.stdout.splitlines()]
    assert len(responses) == len(statements), (len(responses), len(statements), result.stderr)
    for statement, response in zip(statements, responses):
        assert "error" not in response, (statement[:120], response)
    return responses


def wal_size(directory: Path) -> int:
    wal = directory / "commit.wal"
    return wal.stat().st_size if wal.exists() else 0


def kill_at_marker(code: str, directory: Path, marker: Path, *args: str) -> None:
    with subprocess.Popen(
        [sys.executable, "-c", code, str(PROGRAM), str(directory), str(marker), *args],
        stderr=subprocess.PIPE, encoding="utf-8", env=env(NO_CHECKPOINT),
    ) as process:
        try:
            deadline = time.monotonic() + 15
            while not marker.exists() and process.poll() is None and time.monotonic() < deadline:
                time.sleep(0.01)
            if not marker.exists() or process.poll() is not None:
                detail = process.stderr.read() if process.poll() is not None else "timed out"
                raise AssertionError(f"child did not reach {marker.name}: {detail}")
            process.kill()
            assert process.wait(timeout=5) == (-signal.SIGKILL if os.name == "posix" else 1)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)


def recovery_test(baseline: Path, workspace: Path) -> None:
    directory = workspace / "recovery"
    history = [
        "CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(32), embedding VECTOR(2));",
        "INSERT INTO docs VALUES (1, 'before', '[0,0]'), (2, 'delete', '[1,0]'), (3, 'stays', '[5,0]');",
        "CREATE TABLE metadata (id BIGINT PRIMARY KEY, label VARCHAR(32));",
        "INSERT INTO metadata VALUES (42, 'legacy');",
    ]
    history += [f"INSERT INTO docs VALUES ({key}, 'filler', '[0,0]');" for key in range(100, 160)]
    run_sql(baseline, directory, history, None)
    old_size = wal_size(directory)
    lookup = "SELECT id, category FROM docs WHERE id < 100 ORDER BY id;"
    initial = [[1, "before"], [2, "delete"], [3, "stays"]]
    assert run_sql(PROGRAM, directory, [lookup], NO_CHECKPOINT)[0]["rows"] == initial
    run_sql(PROGRAM, directory, ["UPDATE docs SET category = 'checkpoint' WHERE id = 1;"], 256)
    checkpoint_size = wal_size(directory)
    assert checkpoint_size < old_size, (checkpoint_size, old_size)
    assert run_sql(PROGRAM, directory, [lookup], NO_CHECKPOINT)[0]["rows"] == [
        [1, "checkpoint"], [2, "delete"], [3, "stays"],
    ]
    run_sql(PROGRAM, directory, [
        "BEGIN;",
        "UPDATE docs SET category = 'tail', embedding = '[3,4]' WHERE id = 1;",
        "DELETE FROM docs WHERE id = 2;",
        "INSERT INTO docs VALUES (4, 'tail insert', '[1,0]');",
        "COMMIT;",
        "BEGIN;",
        "DELETE FROM metadata WHERE id = 42;",
        "ROLLBACK;",
    ], NO_CHECKPOINT)
    tail_size = wal_size(directory)
    assert tail_size > checkpoint_size, (tail_size, checkpoint_size)
    with (directory / "commit.wal").open("ab") as stream:
        stream.write(b"\x05\x00\x00")
        stream.flush()
        os.fsync(stream.fileno())

    for attempt in (1, 2):
        kill_at_marker(PAUSE_AFTER_FIRST_REDO, directory, workspace / f"redo-{attempt}")

    queries = [
        lookup,
        "SELECT id, label FROM metadata ORDER BY id;",
        "SELECT id, L2_DISTANCE(embedding, '[0,0]') AS d FROM docs WHERE category = 'tail' ORDER BY d LIMIT 1;",
    ]
    expected = [[[1, "tail"], [3, "stays"], [4, "tail insert"]], [[42, "legacy"]], [[1, 5.0]]]
    assert [item["rows"] for item in run_sql(PROGRAM, directory, queries, NO_CHECKPOINT)] == expected
    assert wal_size(directory) == tail_size, (wal_size(directory), tail_size)
    assert [item["rows"] for item in run_sql(PROGRAM, directory, queries, NO_CHECKPOINT)] == expected
    print(f"PASS legacy WAL migration and checkpoint truncation: {old_size} -> {checkpoint_size} bytes")
    print("PASS two kills after partial tail replay, complete transaction after restart, torn tail removed")

    for stage, filename in (("before", "checkpoint.dat"), ("after", "checkpoint.dat"), ("after", "commit.wal")):
        label = f"{stage}-{filename}"
        copy = workspace / label
        shutil.copytree(directory, copy)
        kill_at_marker(PAUSE_DURING_CHECKPOINT, copy, workspace / f"pause-{label}", stage, filename)
        updated = expected[0] + [[5, "at publish"]]
        replies = run_sql(PROGRAM, copy, [lookup, "INSERT INTO metadata VALUES (43, 'after restart');"], NO_CHECKPOINT)
        assert replies[0]["rows"] == updated, (label, replies[0])
        assert run_sql(PROGRAM, copy, [queries[1]], NO_CHECKPOINT)[0]["rows"] == [
            [42, "legacy"], [43, "after restart"],
        ], label
    print("PASS kills before and after checkpoint publication and after WAL replacement, including later writes")


def benchmark(baseline: Path, workspace: Path, row_count: int, rounds: int, threshold: int | None) -> None:
    randomizer = random.Random(20261109)
    workload = ["CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(8), embedding VECTOR(128));"]
    for first in range(0, row_count, 100):
        rows = []
        for key in range(first, min(first + 100, row_count)):
            vector = json.dumps([round(randomizer.uniform(-1, 1), 4) for _ in range(128)], separators=(",", ":"))
            rows.append(f"({key}, 'bench', '{vector}')")
        workload.append("INSERT INTO docs VALUES " + ",".join(rows) + ";")

    cases = (("before", baseline, workspace / "before"), ("after", PROGRAM, workspace / "after"))
    for _, program, directory in cases:
        run_sql(program, directory, workload, threshold)
    wal_bytes = [wal_size(directory) for _, _, directory in cases]
    timings: dict[str, list[float]] = {"before": [], "after": []}
    topk_timings: dict[str, list[float]] = {"before": [], "after": []}
    topk_keys = None
    for round_number in range(rounds):
        for label, program, directory in cases if round_number % 2 == 0 else reversed(cases):
            completed = subprocess.run(
                [sys.executable, "-c", MEASURE_INIT, str(program), str(directory), str(row_count - 1)],
                capture_output=True, encoding="utf-8", env=env(threshold), timeout=120,
            )
            if completed.returncode:
                raise AssertionError(completed.stderr)
            sample = json.loads(completed.stdout)
            timings[label].append(sample["startup"])
            topk_timings[label].append(sample["first_topk"])
            if topk_keys is None:
                topk_keys = sample["topk_keys"]
            else:
                assert sample["topk_keys"] == topk_keys, (label, sample["topk_keys"], topk_keys)
    medians = {label: statistics.median(samples) for label, samples in timings.items()}
    reduction = (1 - medians["after"] / medians["before"]) * 100
    print(json.dumps({
        "startup_measurement": "engine initialization in a new process, local filesystem cache unchanged",
        "rows": row_count, "dimension": 128, "batch_size": 100, "rounds": rounds,
        "checkpoint_bytes": threshold if threshold is not None else "engine default",
        "wal_bytes_before": wal_bytes[0], "wal_bytes_after": wal_bytes[1],
        "seconds": timings, "median_seconds": medians, "startup_reduction_percent": reduction,
        "first_topk_median_seconds": {label: statistics.median(samples) for label, samples in topk_timings.items()},
    }, indent=2))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", default=BASELINE, help="Git revision of the WAL-only implementation")
    parser.add_argument("--benchmark", action="store_true", help="also compare startup times")
    parser.add_argument("--rows", type=int, default=30_000)
    parser.add_argument("--rounds", type=int, default=5)
    parser.add_argument("--checkpoint-bytes", type=int, help="override the engine default for the benchmark")
    args = parser.parse_args()
    if args.rows < 1 or args.rounds < 1 or (args.checkpoint_bytes is not None and args.checkpoint_bytes < 1):
        parser.error("rows, rounds and an explicit checkpoint threshold must be positive")
    with tempfile.TemporaryDirectory(prefix="mvd-checkpoint-") as path:
        workspace = Path(path)
        baseline = workspace / "old" / PROGRAM.name
        baseline.parent.mkdir()
        baseline.write_bytes(subprocess.check_output(
            ["git", "show", f"{args.baseline}:{PROGRAM.name}"], cwd=ROOT,
        ))
        library = ROOT / "libmvdscan.so"
        if library.exists():
            shutil.copyfile(library, baseline.with_name(library.name))
        recovery_test(baseline, workspace)
        if args.benchmark:
            benchmark(baseline, workspace, args.rows, args.rounds, args.checkpoint_bytes)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
