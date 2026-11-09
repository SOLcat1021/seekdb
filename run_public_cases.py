"""Run the official public SQL/result fixtures against the local engine API."""

from __future__ import annotations

import json
import math
import tempfile
from pathlib import Path

from minivectordb import MiniVectorDB, SQLFailure, error_response


ROOT = Path(__file__).parent / "mysqltest"


def equal(actual, expected):
    if isinstance(expected, float):
        return isinstance(actual, (float, int)) and math.isclose(actual, expected, rel_tol=1e-5, abs_tol=1e-5)
    if isinstance(expected, list):
        return isinstance(actual, list) and len(actual) == len(expected) and all(
            equal(a, e) for a, e in zip(actual, expected)
        )
    return type(actual) is type(expected) and actual == expected


def check(actual: dict, expected: dict) -> bool:
    if "error" in expected:
        return actual.get("error") == expected["error"]
    if "error" in actual:
        return False
    for key, value in expected.items():
        if key == "unordered":
            continue
        if key not in actual:
            return False
        if key == "rows" and expected.get("unordered"):
            if len(actual[key]) != len(value):
                return False
            remaining = list(actual[key])
            for row in value:
                match = next((i for i, candidate in enumerate(remaining) if equal(candidate, row)), None)
                if match is None:
                    return False
                remaining.pop(match)
        elif not equal(actual[key], value):
            return False
    return True


def main() -> int:
    manifest = json.loads((ROOT / "manifest.json").read_text(encoding="utf-8"))
    total = 0
    for case in manifest["cases"]:
        name = case["id"]
        lines = [line.strip() for line in (ROOT / case["test"]).read_text(encoding="utf-8").splitlines() if line.strip()]
        expected = json.loads((ROOT / case["result"]).read_text(encoding="utf-8"))
        if sum(not line.startswith("--") for line in lines) != len(expected) or len(expected) != case["statements"]:
            raise ValueError(f"fixture length mismatch: {name}")
        with tempfile.TemporaryDirectory(prefix="mvd-public-") as directory:
            database = MiniVectorDB(directory)
            number = 0
            mode = "exact"
            for sql in lines:
                if sql == "--restart":
                    database.close()
                    database = MiniVectorDB(directory)
                    continue
                if sql == "--mode ann":
                    mode = "ann"
                    continue
                if sql.startswith("--"):
                    raise ValueError(f"unknown driver directive in {name}: {sql}")
                number += 1
                assertion = expected[number - 1]
                try:
                    # Exact search is also a valid ANN answer for these public cases.
                    actual = database.execute(sql)
                except SQLFailure as exc:
                    actual = error_response(exc)
                if not check(actual, assertion):
                    print(f"FAIL {name} #{number} ({mode}): {sql}\n  expected: {assertion}\n  actual: {actual}")
                    return 1
                total += 1
            database.close()
        print(f"PASS {name}: {number} statements")
    print(f"PASS all public cases: {total} statements")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
