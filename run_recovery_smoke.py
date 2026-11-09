"""Exercise transaction atomicity and committed/uncommitted crash recovery."""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

from minivectordb import MiniVectorDB, SQLFailure


PROGRAM = Path(__file__).with_name("minivectordb.py")


def send(process: subprocess.Popen, sql: str) -> dict:
    assert process.stdin is not None and process.stdout is not None
    process.stdin.write(sql + "\n")
    process.stdin.flush()
    reply = process.stdout.readline()
    if not reply:
        raise AssertionError(f"database exited before replying to: {sql}")
    return json.loads(reply)


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="mvd-recovery-") as directory:
        command = [sys.executable, str(PROGRAM), "--data-dir", directory]
        process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        try:
            assert send(process, "CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(16), embedding VECTOR(2));") == {}
            assert send(process, "INSERT INTO docs VALUES (1, 'old', '[0,0]');") == {"affected_rows": 1}
            assert send(process, "BEGIN;") == {}
            assert send(process, "UPDATE docs SET category = 'new', embedding = '[3,4]' WHERE id = 1;") == {"affected_rows": 1}
            assert send(process, "SELECT category, L2_DISTANCE(embedding, '[0,0]') AS distance FROM docs WHERE id = 1;")["rows"] == [["new", 5.0]]
            assert send(process, "ROLLBACK;") == {}
            assert send(process, "SELECT category, L2_DISTANCE(embedding, '[0,0]') AS distance FROM docs WHERE id = 1;")["rows"] == [["old", 0.0]]
            assert send(process, "BEGIN;") == {}
            assert send(process, "INSERT INTO docs VALUES (2, 'committed', '[1,1]');") == {"affected_rows": 1}
            assert "error" in send(process, "INSERT INTO docs VALUES (3, 'bad', '[1,1]'), (2, 'duplicate', '[1,1]');")
            assert send(process, "COMMIT;") == {}
            assert send(process, "BEGIN;") == {}
            assert send(process, "INSERT INTO docs VALUES (4, 'uncommitted', '[4,4]');") == {"affected_rows": 1}
            process.kill()
            process.wait(timeout=5)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)

        database = MiniVectorDB(directory)
        assert database.execute("SELECT id FROM docs ORDER BY id;")["rows"] == [[1], [2]]
        assert database.execute("SELECT L2_DISTANCE(embedding, '[0,0]') AS distance FROM docs WHERE id = 2;")["rows"] == [[2**0.5]]
        assert database.execute("DELETE FROM docs WHERE id = 1;") == {"affected_rows": 1}
        assert database.execute("INSERT INTO docs VALUES (1, 'reinserted', '[6,8]');") == {"affected_rows": 1}
        assert database.execute("SELECT category FROM docs WHERE id = 1;")["rows"] == [["reinserted"]]
        # A torn trailing frame must be discarded before future commits append.
        with (Path(directory) / "commit.wal").open("ab") as stream:
            stream.write(b"\x09\x00\x00")
            stream.flush()
            os.fsync(stream.fileno())
        database = MiniVectorDB(directory)
        assert database.execute("INSERT INTO docs VALUES (5, 'later', '[5,5]');") == {"affected_rows": 1}
        database = MiniVectorDB(directory)
        assert database.execute("SELECT id FROM docs ORDER BY id;")["rows"] == [[1], [2], [5]]
        assert database.execute("SELECT L2_DISTANCE(embedding, '[0,0]') AS distance FROM docs WHERE id = 1;")["rows"] == [[10.0]]
    print("PASS transaction rollback, statement atomicity, committed crash recovery, reinsertion, torn WAL tail")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
