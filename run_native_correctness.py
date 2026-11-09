"""Compare exact TopK with an independent brute-force reference."""

from __future__ import annotations

import json
import math
import random
import tempfile

from minivectordb import MiniVectorDB, as_value


def expected(database: MiniVectorDB, query: list[float], category: str | None, k: int,
             min_id: int | None = None):
    table = database.tables["docs"]
    target = as_value(json.dumps(query), table["columns"][2])
    rows = []
    for row in table["rows"].values():
        if (category is None or row[1] == category) and (min_id is None or row[0] >= min_id):
            rows.append((math.dist(row[2], target), row[0]))
    rows.sort()
    return rows[:k]


def check(database: MiniVectorDB, query: list[float], category: str | None, k: int,
          min_id: int | None = None):
    vector = json.dumps(query, separators=(",", ":"))
    conditions = []
    if category:
        conditions.append(f"category = '{category}'")
    if min_id is not None:
        conditions.append(f"id >= {min_id}")
    where = " WHERE " + " AND ".join(conditions) if conditions else ""
    result = database.execute(
        f"SELECT id, L2_DISTANCE(embedding, '{vector}') AS distance "
        f"FROM docs{where} ORDER BY distance LIMIT {k};"
    )
    truth = expected(database, query, category, k, min_id)
    assert [row[0] for row in result["rows"]] == [key for _, key in truth]
    assert all(math.isclose(row[1], distance, rel_tol=1e-5, abs_tol=1e-5)
               for row, (distance, _) in zip(result["rows"], truth))


def main() -> int:
    generator = random.Random(714)
    with tempfile.TemporaryDirectory(prefix="mvd-native-check-") as directory:
        database = MiniVectorDB(directory)
        database.execute("CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(8), embedding VECTOR(8));")
        for begin in range(0, 200, 50):
            values = []
            for key in range(begin, begin + 50):
                vector = [round(generator.uniform(-1, 1), 3) for _ in range(8)]
                values.append(f"({key}, '{'a' if key % 3 else 'b'}', '{json.dumps(vector)}')")
            assert database.execute(f"INSERT INTO docs VALUES {','.join(values)};") == {"affected_rows": 50}
        for _ in range(30):
            query = [round(generator.uniform(-1, 1), 3) for _ in range(8)]
            check(database, query, None, 10)
            check(database, query, "a", 10)
            check(database, query, "b", 10)
        for minimum in range(0, 120, 10):
            check(database, [0] * 8, "a", 10, minimum)
        check(database, [0] * 8, "a", 10, 0)
        assert database.execute("UPDATE docs SET embedding = '[0,0,0,0,0,0,0,0]' WHERE id = 5;") == {"affected_rows": 1}
        assert database.execute("DELETE FROM docs WHERE id = 6;") == {"affected_rows": 1}
        assert database.execute("INSERT INTO docs VALUES (6, 'a', '[0,0,0,0,0,0,0,0]');") == {"affected_rows": 1}
        check(database, [0] * 8, None, 10)
        check(database, [0] * 8, "a", 10)
        assert database.execute("BEGIN;") == {}
        assert database.execute("UPDATE docs SET embedding = '[0,0,0,0,0,0,0,0]' WHERE id = 7;") == {"affected_rows": 1}
        result = database.execute("SELECT id FROM docs WHERE category = 'a' ORDER BY id LIMIT 1;")
        assert result["rows"] == [[1]]
        assert database.execute("ROLLBACK;") == {}
        database.close()
        database = MiniVectorDB(directory)
        check(database, [0] * 8, None, 10)
        database.close()
    print("PASS exact TopK random reference, filtering, ties, update/delete/reinsert, rollback, restart")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
