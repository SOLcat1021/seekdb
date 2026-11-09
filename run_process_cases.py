"""Exercise public fixtures through the current line-oriented process protocol."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import tempfile
from pathlib import Path

from run_public_cases import ROOT, check


PROGRAM = Path(__file__).with_name("minivectordb.py")


def start(directory: str, docker_image: str | None) -> subprocess.Popen:
    command = (
        ["docker", "run", "--rm", "-i", "--network", "none", "--mount", f"type=bind,src={directory},dst=/data", docker_image]
        if docker_image else
        [sys.executable, str(PROGRAM), "--data-dir", directory]
    )
    return subprocess.Popen(
        command,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
    )


def stop(process: subprocess.Popen) -> None:
    assert process.stdin is not None
    process.stdin.close()
    process.wait(timeout=10)
    if process.returncode:
        assert process.stderr is not None
        raise AssertionError(f"database exited {process.returncode}: {process.stderr.read()}")
    process.stdout.close()
    process.stderr.close()


def main() -> int:
    arguments = argparse.ArgumentParser()
    arguments.add_argument("--docker-image", help="run fixtures through this locally built image")
    args = arguments.parse_args()
    manifest = json.loads((ROOT / "manifest.json").read_text(encoding="utf-8"))
    total = 0
    for case in manifest["cases"]:
        name = case["id"]
        lines = [line.strip() for line in (ROOT / case["test"]).read_text(encoding="utf-8").splitlines() if line.strip()]
        assertions = json.loads((ROOT / case["result"]).read_text(encoding="utf-8"))
        if sum(not line.startswith("--") for line in lines) != len(assertions):
            raise AssertionError(f"fixture length mismatch: {name}")
        with tempfile.TemporaryDirectory(prefix="mvd-process-") as directory:
            process = start(directory, args.docker_image)
            number = 0
            try:
                for line in lines:
                    if line == "--restart":
                        stop(process)
                        process = start(directory, args.docker_image)
                        continue
                    if line == "--mode ann":
                        # Exact results satisfy the public ANN assertions.
                        continue
                    if line.startswith("--"):
                        raise AssertionError(f"unknown directive: {line}")
                    assert process.stdin is not None and process.stdout is not None
                    process.stdin.write(line + "\n")
                    process.stdin.flush()
                    reply = process.stdout.readline()
                    if not reply:
                        raise AssertionError(f"no response for {name} #{number + 1}: {line}")
                    actual = json.loads(reply)
                    expected = assertions[number]
                    number += 1
                    if not check(actual, expected):
                        raise AssertionError(
                            f"{name} #{number}: {line}\nexpected {expected}\nactual {actual}"
                        )
                    total += 1
            finally:
                if process.poll() is None:
                    stop(process)
        print(f"PASS process {name}: {number} statements")
    print(f"PASS all public process cases: {total} statements")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
