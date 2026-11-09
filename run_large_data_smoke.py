"""Show a committed table larger than the process memory cap remains queryable."""

from __future__ import annotations

import gc
import resource
import tempfile
from pathlib import Path

from minivectordb import MiniVectorDB


MEMORY_CAP = 64 * 1024 * 1024
RECORDS = 17000
BATCH = 100
PAYLOAD = "x" * 4096


def main() -> int:
    resource.setrlimit(resource.RLIMIT_AS, (MEMORY_CAP, MEMORY_CAP))
    with tempfile.TemporaryDirectory(prefix="mvd-large-") as directory:
        database = MiniVectorDB(directory)
        database.execute("CREATE TABLE docs (id BIGINT PRIMARY KEY, content VARCHAR(4096));")
        for start in range(1, RECORDS + 1, BATCH):
            values = ",".join(f"({number}, '{PAYLOAD}')" for number in range(start, min(start + BATCH, RECORDS + 1)))
            result = database.execute(f"INSERT INTO docs VALUES {values};")
            assert result["affected_rows"] == min(BATCH, RECORDS - start + 1)
        result = database.execute("SELECT id FROM docs WHERE id >= 16990 ORDER BY id LIMIT 10;")
        assert result["rows"] == [[number] for number in range(16990, 17000)]
        assert database.execute("BEGIN;") == {}
        assert database.execute("UPDATE docs SET content = 'temporary' WHERE id = 17000;") == {"affected_rows": 1}
        assert database.execute("SELECT content FROM docs WHERE id = 17000;")["rows"] == [["temporary"]]
        assert database.execute("ROLLBACK;") == {}
        assert database.execute("SELECT content FROM docs WHERE id = 17000;")["rows"] == [[PAYLOAD]]
        assert database.execute("UPDATE docs SET content = 'committed' WHERE id = 17000;") == {"affected_rows": 1}
        data_size = (Path(directory) / "materialized" / "docs.dat").stat().st_size
        assert data_size > MEMORY_CAP, data_size
        database = None
        gc.collect()
        database = MiniVectorDB(directory)
        assert database.execute("SELECT id FROM docs WHERE id >= 16999 ORDER BY id LIMIT 2;")["rows"] == [[16999], [17000]]
        assert database.execute("SELECT content FROM docs WHERE id = 17000;")["rows"] == [["committed"]]
        peak_kib = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
        print(f"PASS {RECORDS} rows, data file {data_size / 2**20:.1f} MiB, address-space cap 64 MiB, peak RSS {peak_kib / 1024:.1f} MiB")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
