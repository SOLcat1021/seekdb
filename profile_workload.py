"""Small, deterministic diagnostic workload; not a competition benchmark."""

from __future__ import annotations

import cProfile
import io
import pstats
import tempfile
import time

from minivectordb import MiniVectorDB


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="mvd-profile-") as directory:
        database = MiniVectorDB(directory)
        database.execute("CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(8), embedding VECTOR(128));")
        vector = "[" + ",".join(["0"] * 128) + "]"
        started = time.perf_counter()
        for begin in range(0, 1000, 100):
            batch = ",".join(f"({i}, '{'a' if i % 2 else 'b'}', '{vector}')" for i in range(begin, begin + 100))
            database.execute(f"INSERT INTO docs VALUES {batch};")
        print(f"1000-row insert: {time.perf_counter() - started:.3f}s")
        query = f"SELECT id, L2_DISTANCE(embedding, '{vector}') AS distance FROM docs WHERE category = 'a' ORDER BY distance LIMIT 10;"
        profiler = cProfile.Profile()
        started = time.perf_counter()
        profiler.enable()
        for _ in range(20):
            database.execute(query)
        profiler.disable()
        print(f"20 filtered TopK queries: {time.perf_counter() - started:.3f}s")
        report = io.StringIO()
        pstats.Stats(profiler, stream=report).sort_stats("cumulative").print_stats(18)
        print(report.getvalue())


if __name__ == "__main__":
    main()
