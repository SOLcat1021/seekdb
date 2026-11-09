"""Self-contained MiniVectorDB semantic baseline.

The line-oriented JSON protocol in main() is local-only until the official
competition driver is published. Storage, SQL execution, and WAL are ours.
"""

from __future__ import annotations

import argparse
from array import array
import ctypes
import heapq
import itertools
import json
import math
import os
import re
import shutil
import struct
import sys
import uuid
import zlib
from functools import cmp_to_key
from pathlib import Path


class SQLFailure(ValueError):
    pass


def error_response(exc: ValueError) -> dict[str, str]:
    message = str(exc)
    if message == "duplicate primary key":
        code = "duplicate_key"
    elif message == "VECTOR dimension mismatch":
        code = "dimension_mismatch"
    elif message == "unsupported statement":
        code = "unsupported_sql"
    else:
        code = "sql_error"
    return {"error": code}


TOKEN = re.compile(
    r"\s*(?:'((?:''|[^'])*)'|([A-Za-z_][A-Za-z_0-9]*)|(-?\d+(?:\.\d+)?(?:[eE][+-]?\d+)?)|(<=|>=|<>|!=|[(),;*=<>]))",
    re.DOTALL,
)
IDENT = re.compile(r"^[A-Za-z_][A-Za-z_0-9]*$")
MAX_FRAME = 64 * 1024 * 1024
COMPRESSED_FRAME = 1 << 31
BUCKETS = int(os.environ.get("MVD_BUCKETS", "32768"))
if BUCKETS < 1024 or BUCKETS > 1 << 20 or BUCKETS & (BUCKETS - 1):
    raise ValueError("MVD_BUCKETS must be a power of two between 1024 and 1048576")
ROW_HEADER = struct.Struct("<qQIB")
TOMBSTONE = 0xFFFFFFFF
DEFAULT_VECTOR_CACHE_BYTES = 16 * 1024 * 1024
WAL_COMPRESSION_LEVEL = int(os.environ.get("MVD_WAL_COMPRESSION_LEVEL", "4"))
WAL_COMPRESSION_THRESHOLD = int(os.environ.get("MVD_WAL_COMPRESSION_THRESHOLD", "1024"))
if not 0 <= WAL_COMPRESSION_LEVEL <= 9 or WAL_COMPRESSION_THRESHOLD < 0:
    raise ValueError("invalid WAL compression settings")
WAL_MAGIC = b"MVDWAL2\n"
WAL_HEADER_SIZE = len(WAL_MAGIC) + 16
CHECKPOINT_MAGIC = b"MVDCKP1\n"
COPY_BLOCK = 1024 * 1024


def sync_directory(path: Path) -> None:
    if hasattr(os, "O_DIRECTORY"):
        descriptor = os.open(path, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(descriptor)
        finally:
            os.close(descriptor)


def load_native_scan():
    library = Path(__file__).with_name("libmvdscan.so")
    if not library.exists():
        return None
    try:
        native = ctypes.CDLL(str(library))
    except OSError:
        return None
    native.mvd_topk.argtypes = [
        ctypes.POINTER(ctypes.c_float), ctypes.POINTER(ctypes.c_int64),
        ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t, ctypes.c_size_t,
        ctypes.POINTER(ctypes.c_float), ctypes.c_size_t,
        ctypes.POINTER(ctypes.c_size_t), ctypes.POINTER(ctypes.c_double),
    ]
    native.mvd_topk.restype = ctypes.c_size_t
    return native


NATIVE_SCAN = load_native_scan()


class CacheBudget:
    def __init__(self):
        self.limit = max(0, int(os.environ.get("MVD_VECTOR_CACHE_BYTES", DEFAULT_VECTOR_CACHE_BYTES)))
        self.used = 0


class DiskRows:
    """Append-only rows with a fixed-memory bucket directory and disk chains."""

    def __init__(self, directory: Path, table_name: str, columns: list[dict], budget: CacheBudget, *, heads=None):
        directory.mkdir(parents=True, exist_ok=True)
        self.heap = (directory / f"{table_name}.dat").open("w+b", buffering=0)
        self.columns = columns
        self.vector_structs = {
            index: struct.Struct(f"<{column['size']}f")
            for index, column in enumerate(columns)
            if column["type"] == "vector"
        }
        self.budget = budget
        self.cache_pending = heads is not None and bool(self.vector_structs and budget.limit)
        self.cache: dict[str, list] | None = {} if self.vector_structs and budget.limit and heads is None else None
        self.cache_sizes: dict[str, int] = {}
        self.native_keys: array | None = None
        self.native_vectors: array | None = None
        self.native_bytes = 0
        self.native_filter_masks: dict[tuple, bytearray] = {}
        self.heap.write(b"\0")  # zero is the empty-chain sentinel
        self.heads = [0] * BUCKETS if heads is None else heads

    def _bucket(self, key: int) -> int:
        return key & (len(self.heads) - 1)

    def _header(self, offset: int) -> tuple[int, int, int, int]:
        self.heap.seek(offset)
        header = self.heap.read(ROW_HEADER.size)
        if len(header) != ROW_HEADER.size:
            raise SQLFailure("corrupt row heap")
        key, previous, length, active = ROW_HEADER.unpack(header)
        if length != TOMBSTONE and length > MAX_FRAME:
            raise SQLFailure("oversized row")
        if active not in (0, 1):
            raise SQLFailure("corrupt row header")
        return key, previous, length, active

    def _encode(self, row: list) -> bytes:
        payload = bytearray()
        for index, (column, value) in enumerate(zip(self.columns, row)):
            kind = column["type"]
            if kind == "bigint":
                payload.extend(struct.pack("<q", value))
            elif kind == "varchar":
                text = value.encode("utf-8")
                payload.extend(struct.pack("<I", len(text)))
                payload.extend(text)
            else:
                payload.extend(self.vector_structs[index].pack(*value))
        return bytes(payload)

    def _decode(self, payload: bytes) -> list:
        row = []
        offset = 0
        for index, column in enumerate(self.columns):
            kind = column["type"]
            if kind == "bigint":
                row.append(struct.unpack_from("<q", payload, offset)[0])
                offset += 8
            elif kind == "varchar":
                length = struct.unpack_from("<I", payload, offset)[0]
                offset += 4
                row.append(payload[offset:offset + length].decode("utf-8"))
                offset += length
            else:
                fmt = self.vector_structs[index]
                row.append(list(fmt.unpack_from(payload, offset)))
                offset += fmt.size
        if offset != len(payload):
            raise SQLFailure("corrupt row payload")
        return row

    def _read_row(self, offset: int, length: int) -> list:
        self.heap.seek(offset + ROW_HEADER.size)
        payload = self.heap.read(length)
        if len(payload) != length:
            raise SQLFailure("corrupt row payload")
        try:
            return self._decode(payload)
        except (ValueError, UnicodeError, struct.error) as exc:
            raise SQLFailure("corrupt row payload") from exc

    def _latest(self, key: int) -> tuple[int, int]:
        offset = self.heads[self._bucket(key)]
        while offset:
            found_key, previous, length, _ = self._header(offset)
            if found_key == key:
                return offset, length
            offset = previous
        return 0, TOMBSTONE

    def __contains__(self, key: str) -> bool:
        offset, length = self._latest(int(key))
        return bool(offset) and length != TOMBSTONE

    def __getitem__(self, key: str):
        offset, length = self._latest(int(key))
        if not offset or length == TOMBSTONE:
            raise KeyError(key)
        return self._read_row(offset, length)

    def _append(self, key: str, row) -> None:
        numeric = int(key)
        bucket = self._bucket(numeric)
        payload = b"" if row is None else self._encode(row)
        if len(payload) >= TOMBSTONE:
            raise SQLFailure("row too large")
        old_offset, _ = self._latest(numeric)
        if old_offset:
            self.heap.seek(old_offset + ROW_HEADER.size - 1)
            self.heap.write(b"\0")
        self.heap.seek(0, os.SEEK_END)
        offset = self.heap.tell()
        self.heap.write(ROW_HEADER.pack(numeric, self.heads[bucket], TOMBSTONE if row is None else len(payload), 1))
        if payload:
            self.heap.write(payload)
        self.heads[bucket] = offset
        self._update_cache(key, row)

    def _update_cache(self, key: str, row) -> None:
        if self.cache is None:
            return
        self._invalidate_native()
        previous = self.cache_sizes.pop(key, 0)
        self.budget.used -= previous
        self.cache.pop(key, None)
        if row is None:
            return
        copied = list(row)
        size = 256
        for index, column in enumerate(self.columns):
            if column["type"] == "vector":
                copied[index] = array("f", copied[index])
                size += len(copied[index]) * 4
            elif column["type"] == "varchar":
                size += len(copied[index].encode("utf-8"))
            else:
                size += 8
        if self.budget.used + size > self.budget.limit:
            self.budget.used -= sum(self.cache_sizes.values())
            self.cache = None
            self.cache_sizes.clear()
            return
        self.cache[key] = copied
        self.cache_sizes[key] = size
        self.budget.used += size

    def _invalidate_native(self) -> None:
        self.budget.used -= self.native_bytes
        self.native_bytes = 0
        self.native_keys = None
        self.native_vectors = None
        self.native_filter_masks.clear()

    def load_cache(self) -> None:
        if self.cache_pending:
            self.cache_pending = False
            self.cache = {}
            for key, row in self.items():
                self._update_cache(key, row)
                if self.cache is None:
                    break

    def native_topk(self, target: list[float], tests: list, limit: int):
        if NATIVE_SCAN is not None and limit > 0:
            self.load_cache()
        if NATIVE_SCAN is None or self.cache is None or limit <= 0:
            return None
        dimension = len(target)
        if self.native_vectors is None:
            needed = len(self.cache) * (dimension * 4 + 8)
            if self.budget.used + needed > self.budget.limit:
                return None
            keys = array("q")
            vectors = array("f")
            vector_column = next(iter(self.vector_structs))
            for key, row in self.cache.items():
                keys.append(int(key))
                vectors.extend(row[vector_column])
            self.native_keys = keys
            self.native_vectors = vectors
            self.native_bytes = needed
            self.budget.used += needed
        keys = self.native_keys
        vectors = self.native_vectors
        if not keys:
            return []
        eligible = None
        if tests:
            filter_key = tuple(tests)
            eligible = self.native_filter_masks.get(filter_key)
            if eligible is None:
                eligible = bytearray(1 if matches(self.cache[str(key)], tests) else 0 for key in keys)
                if len(self.native_filter_masks) >= 8:
                    self.native_filter_masks.clear()
                self.native_filter_masks[filter_key] = eligible
        query = array("f", target)
        capacity = min(limit, len(keys))
        positions = (ctypes.c_size_t * capacity)()
        distances = (ctypes.c_double * capacity)()
        found = NATIVE_SCAN.mvd_topk(
            (ctypes.c_float * len(vectors)).from_buffer(vectors),
            (ctypes.c_int64 * len(keys)).from_buffer(keys),
            (ctypes.c_uint8 * len(eligible)).from_buffer(eligible) if eligible is not None else None,
            len(keys), dimension,
            (ctypes.c_float * len(query)).from_buffer(query), capacity,
            positions, distances,
        )
        return [(self.cache[str(keys[positions[i]])], distances[i]) for i in range(found)]

    def __setitem__(self, key: str, row) -> None:
        self._append(key, row)

    def __delitem__(self, key: str) -> None:
        if key not in self:
            raise KeyError(key)
        self._append(key, None)

    def items(self):
        self.heap.seek(0, os.SEEK_END)
        end = self.heap.tell()
        offset = 1
        while offset < end:
            key, _, length, active = self._header(offset)
            if active and length != TOMBSTONE:
                yield str(key), self._read_row(offset, length)
            offset += ROW_HEADER.size + (0 if length == TOMBSTONE else length)

    def values(self):
        for _, row in self.items():
            yield row

    def close(self) -> None:
        self.heap.close()


class OverlayRows:
    """Transaction/statement changes over a disk view or an earlier overlay."""

    def __init__(self, base):
        self.base = base
        self.changes: dict[str, list | None] = {}

    def __contains__(self, key: str) -> bool:
        return self.changes[key] is not None if key in self.changes else key in self.base

    def __getitem__(self, key: str):
        if key in self.changes:
            if self.changes[key] is None:
                raise KeyError(key)
            return self.changes[key]
        return self.base[key]

    def __setitem__(self, key: str, row) -> None:
        self.changes[key] = row

    def __delitem__(self, key: str) -> None:
        if key not in self:
            raise KeyError(key)
        self.changes[key] = None

    def items(self):
        for key, row in self.base.items():
            if key in self.changes:
                if self.changes[key] is not None:
                    yield key, self.changes[key]
            else:
                yield key, row
        for key, row in self.changes.items():
            if row is not None and key not in self.base:
                yield key, row

    def values(self):
        for _, row in self.items():
            yield row


def tokenize(sql: str) -> list[tuple[str, str]]:
    result = []
    pos = 0
    while pos < len(sql):
        if not sql[pos:].strip():
            break
        match = TOKEN.match(sql, pos)
        if not match:
            raise SQLFailure(f"unsupported syntax near {sql[pos:pos + 24]!r}")
        if match.group(1) is not None:
            result.append(("string", match.group(1).replace("''", "'")))
        elif match.group(2) is not None:
            result.append(("word", match.group(2)))
        elif match.group(3) is not None:
            result.append(("number", match.group(3)))
        else:
            result.append(("symbol", match.group(4)))
        pos = match.end()
    if result and result[-1] == ("symbol", ";"):
        result.pop()
    return result


class Parser:
    def __init__(self, sql: str):
        self.tokens = tokenize(sql)
        self.at = 0

    def peek(self) -> tuple[str, str]:
        return self.tokens[self.at] if self.at < len(self.tokens) else ("eof", "")

    def take(self) -> tuple[str, str]:
        token = self.peek()
        if token[0] == "eof":
            raise SQLFailure("unexpected end of SQL")
        self.at += 1
        return token

    def accept(self, value: str) -> bool:
        if self.peek()[1].upper() == value.upper():
            self.at += 1
            return True
        return False

    def expect(self, value: str) -> None:
        if not self.accept(value):
            raise SQLFailure(f"expected {value}, got {self.peek()[1]!r}")

    def ident(self) -> str:
        kind, value = self.take()
        if kind != "word" or not IDENT.fullmatch(value):
            raise SQLFailure("expected identifier")
        return value.lower()

    def unsigned(self) -> int:
        kind, value = self.take()
        if kind != "number" or not value.isdigit():
            raise SQLFailure("expected nonnegative integer")
        return int(value)

    def literal(self):
        kind, value = self.take()
        if kind == "string":
            return value
        if kind == "number":
            return int(value) if re.fullmatch(r"-?\d+", value) else float(value)
        raise SQLFailure("expected literal")

    def done(self) -> None:
        if self.peek()[0] != "eof":
            raise SQLFailure(f"unexpected token {self.peek()[1]!r}")


def as_value(value, column: dict):
    kind = column["type"]
    size = column["size"]
    if kind == "bigint":
        if type(value) is not int or not -(2**63) <= value < 2**63:
            raise SQLFailure("BIGINT out of range")
        return value
    if kind == "varchar":
        if not isinstance(value, str) or len(value) > size:
            raise SQLFailure("invalid VARCHAR")
        return value
    if kind == "vector":
        if not isinstance(value, str):
            raise SQLFailure("invalid VECTOR")
        try:
            data = json.loads(value)
        except (ValueError, TypeError) as exc:
            raise SQLFailure("invalid VECTOR") from exc
        if not isinstance(data, list) or len(data) != size:
            raise SQLFailure("VECTOR dimension mismatch")
        converted = []
        for item in data:
            if type(item) not in (int, float) or not math.isfinite(item):
                raise SQLFailure("nonfinite VECTOR element")
            try:
                number = struct.unpack("<f", struct.pack("<f", item))[0]
            except (OverflowError, struct.error) as exc:
                raise SQLFailure("VECTOR element out of float32 range") from exc
            if not math.isfinite(number):
                raise SQLFailure("VECTOR element out of float32 range")
            converted.append(number)
        return converted
    raise SQLFailure("unknown column type")


def column_index(table: dict, name: str) -> int:
    for index, column in enumerate(table["columns"]):
        if column["name"] == name:
            return index
    raise SQLFailure(f"unknown column {name}")


def predicate(parser: Parser, table: dict):
    tests = []
    if not parser.accept("WHERE"):
        return tests
    while True:
        name = parser.ident()
        index = column_index(table, name)
        operator = parser.take()[1]
        if operator not in ("=", "<", "<=", ">", ">="):
            raise SQLFailure("unsupported comparison")
        rhs = as_value(parser.literal(), table["columns"][index])
        if isinstance(rhs, list):
            raise SQLFailure("VECTOR comparison unsupported")
        tests.append((index, operator, rhs))
        if not parser.accept("AND"):
            break
    return tests


def matches(row: list, tests: list) -> bool:
    for index, operator, rhs in tests:
        lhs = row[index]
        comparison = (lhs > rhs) - (lhs < rhs)
        if operator == "=":
            passed = comparison == 0
        elif operator == "<":
            passed = comparison < 0
        elif operator == "<=":
            passed = comparison <= 0
        elif operator == ">":
            passed = comparison > 0
        else:
            passed = comparison >= 0
        if not passed:
            return False
    return True


def candidate_items(table: dict, tests: list, *, use_cache: bool = False):
    """Use the primary-key directory for an equality predicate."""
    rows = table["rows"]
    for index, operator, value in tests:
        if index == table["primary"] and operator == "=":
            key = str(value)
            if use_cache and isinstance(rows, DiskRows) and rows.cache is not None:
                rows = rows.cache
            try:
                yield key, rows[key]
            except KeyError:
                pass
            return
    if use_cache and isinstance(rows, DiskRows):
        rows.load_cache()
        if rows.cache is not None:
            rows = rows.cache
    yield from rows.items()


def distance(vector: list[float], target: list[float]) -> float:
    return math.dist(vector, target)


class MiniVectorDB:
    def __init__(self, data_dir: str | Path):
        self.data_dir = Path(data_dir)
        self.data_dir.mkdir(parents=True, exist_ok=True)
        self.wal = self.data_dir / "commit.wal"
        self.checkpoint = self.data_dir / "checkpoint.dat"
        self.checkpoint_bytes = int(os.environ.get("MVD_CHECKPOINT_BYTES", str(8 * 1024 * 1024)))
        if self.checkpoint_bytes < 0:
            raise ValueError("MVD_CHECKPOINT_BYTES must be nonnegative")
        self.wal_generation: str | None = None
        self.wal_size = 0
        self.materialized = self.data_dir / "materialized"
        self.materialized.mkdir(exist_ok=True)
        self.cache_budget = CacheBudget()
        for suffix in ("*.dat", "*.idx"):
            for path in self.materialized.glob(suffix):
                path.unlink()
        self.tables: dict[str, dict] = {}
        self.tx_tables: dict[str, dict] | None = None
        self.tx_sql: list[str] = []
        self._recover(self._restore_checkpoint())

    def close(self) -> None:
        self.tx_tables = None
        self.tx_sql = []
        for table in self.tables.values():
            rows = table["rows"]
            if isinstance(rows, DiskRows):
                rows.close()

    def _restore_checkpoint(self) -> dict | None:
        if not self.checkpoint.exists():
            return None
        with self.checkpoint.open("rb") as stream:
            header = stream.read(len(CHECKPOINT_MAGIC) + 8)
            if len(header) != len(CHECKPOINT_MAGIC) + 8 or not header.startswith(CHECKPOINT_MAGIC):
                raise SQLFailure("invalid checkpoint header")
            length, checksum = struct.unpack_from("<II", header, len(CHECKPOINT_MAGIC))
            if length > MAX_FRAME:
                raise SQLFailure("oversized checkpoint metadata")
            payload = stream.read(length)
            if len(payload) != length or zlib.crc32(payload) != checksum:
                raise SQLFailure("corrupt checkpoint metadata")
            metadata = json.loads(payload)
            for table in metadata["tables"]:
                name, columns, primary = table["name"], table["columns"], table["primary"]
                size, buckets = table["heap_size"], table["buckets"]
                if (not IDENT.fullmatch(name) or name in self.tables or size < 1
                        or not 1024 <= buckets <= 1 << 20 or buckets & (buckets - 1)):
                    raise SQLFailure("invalid checkpoint table")
                packed = stream.read(buckets * 8)
                if len(packed) != buckets * 8:
                    raise SQLFailure("truncated checkpoint directory")
                heads = list(struct.unpack(f"<{buckets}Q", packed))
                if any(offset >= size for offset in heads):
                    raise SQLFailure("invalid checkpoint bucket offset")
                rows = DiskRows(self.materialized, name, columns, self.cache_budget, heads=heads)
                self.tables[name] = {"columns": columns, "primary": primary, "rows": rows}
                rows.heap.seek(0)
                remaining = size
                while remaining:
                    block = stream.read(min(remaining, COPY_BLOCK))
                    if not block:
                        raise SQLFailure("truncated checkpoint heap")
                    if rows.heap.write(block) != len(block):
                        raise OSError("short write restoring checkpoint heap")
                    remaining -= len(block)
            if stream.read(1):
                raise SQLFailure("trailing checkpoint data")
        return metadata

    def _write_checkpoint(self) -> None:
        generation = uuid.uuid4().hex
        tables = []
        for name, table in self.tables.items():
            rows = table["rows"]
            tables.append({
                "name": name, "columns": table["columns"], "primary": table["primary"],
                "heap_size": rows.heap.seek(0, os.SEEK_END), "buckets": len(rows.heads),
            })
        payload = json.dumps({
            "generation": generation, "previous_generation": self.wal_generation,
            "wal_cutoff": self.wal_size, "tables": tables,
        }, separators=(",", ":")).encode("utf-8")
        if len(payload) > MAX_FRAME:
            raise OSError("checkpoint metadata too large")
        temporary = self.checkpoint.with_suffix(".tmp")
        with temporary.open("wb") as stream:
            stream.write(CHECKPOINT_MAGIC + struct.pack("<II", len(payload), zlib.crc32(payload)))
            stream.write(payload)
            for table in self.tables.values():
                rows = table["rows"]
                stream.write(struct.pack(f"<{len(rows.heads)}Q", *rows.heads))
                rows.heap.seek(0)
                shutil.copyfileobj(rows.heap, stream, length=COPY_BLOCK)
            stream.flush()
            os.fsync(stream.fileno())
        # Publish and sync the snapshot before discarding any covered WAL bytes.
        os.replace(temporary, self.checkpoint)
        sync_directory(self.data_dir)
        self._rotate_wal(generation)

    def _rotate_wal(self, generation: str) -> None:
        temporary = self.wal.with_suffix(".tmp")
        with temporary.open("wb") as stream:
            stream.write(WAL_MAGIC + bytes.fromhex(generation))
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, self.wal)
        sync_directory(self.data_dir)
        self.wal_generation = generation
        self.wal_size = WAL_HEADER_SIZE

    def _maybe_checkpoint(self) -> None:
        header_size = WAL_HEADER_SIZE if self.wal_generation is not None else 0
        if self.checkpoint_bytes and self.wal_size - header_size >= self.checkpoint_bytes:
            self._write_checkpoint()

    def _recover(self, checkpoint: dict | None) -> None:
        if not self.wal.exists():
            if checkpoint is not None:
                raise SQLFailure("missing checkpoint WAL")
            return
        with self.wal.open("r+b") as stream:
            size = os.fstat(stream.fileno()).st_size
            start = 0
            if stream.read(len(WAL_MAGIC)) == WAL_MAGIC:
                generation = stream.read(16)
                if len(generation) != 16:
                    raise SQLFailure("truncated WAL generation")
                self.wal_generation = generation.hex()
                start = WAL_HEADER_SIZE
            old_wal = False
            if checkpoint is None:
                if self.wal_generation is not None:
                    raise SQLFailure("missing WAL checkpoint")
            elif self.wal_generation != checkpoint["generation"]:
                if self.wal_generation != checkpoint["previous_generation"]:
                    raise SQLFailure("checkpoint WAL generation mismatch")
                # The checkpoint was published, but the covered WAL was not replaced yet.
                start = checkpoint["wal_cutoff"]
                if not stream.tell() <= start <= size:
                    raise SQLFailure("invalid checkpoint WAL offset")
                old_wal = True
            stream.seek(start)
            valid = start
            while True:
                header = stream.read(8)
                if not header:
                    break
                if len(header) != 8:
                    break
                encoded_length, checksum = struct.unpack("<II", header)
                compressed = bool(encoded_length & COMPRESSED_FRAME)
                length = encoded_length & ~COMPRESSED_FRAME
                if length > MAX_FRAME:
                    break
                payload = stream.read(length)
                if len(payload) != length or zlib.crc32(payload) != checksum:
                    break
                if compressed:
                    decoder = zlib.decompressobj()
                    try:
                        payload = decoder.decompress(payload, MAX_FRAME + 1)
                    except zlib.error:
                        break
                    if len(payload) > MAX_FRAME or not decoder.eof or decoder.unused_data:
                        break
                try:
                    commands = json.loads(payload)
                    if not isinstance(commands, list) or not all(isinstance(x, str) for x in commands):
                        break
                    for sql in commands:
                        parser = Parser(sql)
                        self._apply(parser, self.tables, materialize=True)
                        parser.done()
                except (ValueError, SQLFailure, KeyError, TypeError):
                    raise SQLFailure("corrupt committed WAL frame")
                valid = stream.tell()
            if valid != size:
                stream.truncate(valid)
                os.fsync(stream.fileno())
        self.wal_size = valid
        if old_wal and valid == start:
            self._rotate_wal(checkpoint["generation"])

    def _commit_log(self, commands: list[str]) -> None:
        if not commands:
            return
        payload = json.dumps(commands, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        if len(payload) > MAX_FRAME:
            raise SQLFailure("transaction log frame too large")
        encoded_length = len(payload)
        if len(payload) >= WAL_COMPRESSION_THRESHOLD:
            packed = zlib.compress(payload, level=WAL_COMPRESSION_LEVEL)
            if len(packed) < len(payload):
                payload = packed
                encoded_length = len(payload) | COMPRESSED_FRAME
        frame = struct.pack("<II", encoded_length, zlib.crc32(payload)) + payload
        fd = os.open(self.wal, os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600)
        try:
            view = memoryview(frame)
            while view:
                view = view[os.write(fd, view):]
            os.fsync(fd)
        finally:
            os.close(fd)
        if not self.wal_size:
            sync_directory(self.data_dir)
        self.wal_size += len(frame)

    def execute(self, sql: str) -> dict:
        parser = Parser(sql)
        keyword = parser.peek()[1].upper()
        if keyword == "BEGIN":
            parser.take(); parser.done()
            if self.tx_tables is not None:
                raise SQLFailure("transaction already active")
            self.tx_tables = self._overlay_tables(self.tables)
            self.tx_sql = []
            return {}
        if keyword == "ROLLBACK":
            parser.take(); parser.done()
            if self.tx_tables is None:
                raise SQLFailure("no active transaction")
            self.tx_tables = None
            self.tx_sql = []
            return {}
        if keyword == "COMMIT":
            parser.take(); parser.done()
            if self.tx_tables is None:
                raise SQLFailure("no active transaction")
            self._commit_log(self.tx_sql)
            for command in self.tx_sql:
                replay = Parser(command)
                self._apply(replay, self.tables, materialize=True)
                replay.done()
            self.tx_tables = None
            self.tx_sql = []
            self._maybe_checkpoint()
            return {}
        active = self.tx_tables if self.tx_tables is not None else self.tables
        if keyword == "SELECT":
            result = self._select(parser, active)
            parser.done()
            return result
        candidate = self._overlay_tables(active)
        result = self._apply(parser, candidate)
        parser.done()
        if self.tx_tables is None:
            self._commit_log([sql])
            self._materialize_candidate(candidate)
            self._maybe_checkpoint()
        else:
            self.tx_tables = candidate
            self.tx_sql.append(sql)
        return result

    def _materialize_candidate(self, candidate: dict) -> None:
        """Apply the already validated autocommit delta after its WAL is durable."""
        for name, table in candidate.items():
            if name not in self.tables:
                rows = DiskRows(self.materialized, name, table["columns"], self.cache_budget)
                self.tables[name] = {"columns": table["columns"], "primary": table["primary"], "rows": rows}
                continue
            changes = table["rows"].changes
            destination = self.tables[name]["rows"]
            for key, row in changes.items():
                if row is None:
                    del destination[key]
                else:
                    destination[key] = row

    @staticmethod
    def _overlay_tables(tables: dict) -> dict:
        return {
            name: {"columns": table["columns"], "primary": table["primary"], "rows": OverlayRows(table["rows"])}
            for name, table in tables.items()
        }

    def _apply(self, parser: Parser, tables: dict, *, materialize: bool = False) -> dict:
        keyword = parser.peek()[1].upper()
        if keyword == "CREATE":
            parser.expect("CREATE")
            if parser.accept("VECTOR"):
                parser.expect("INDEX")
                parser.ident()
                parser.expect("ON")
                table = self._table(tables, parser.ident())
                parser.expect("(")
                index = column_index(table, parser.ident())
                parser.expect(")")
                if table["columns"][index]["type"] != "vector":
                    raise SQLFailure("index target must be VECTOR")
                parser.done()
                return {}
            parser.expect("TABLE")
            name = parser.ident()
            if name in tables:
                raise SQLFailure("table already exists")
            parser.expect("(")
            columns = []
            primary = None
            vector_count = 0
            while True:
                col_name = parser.ident()
                if any(x["name"] == col_name for x in columns):
                    raise SQLFailure("duplicate column")
                kind = parser.ident()
                if kind not in ("bigint", "varchar", "vector"):
                    raise SQLFailure("unsupported type")
                size = None
                if kind != "bigint":
                    parser.expect("(")
                    size = parser.unsigned()
                    parser.expect(")")
                    if size <= 0:
                        raise SQLFailure("invalid column size")
                if kind == "vector":
                    vector_count += 1
                if parser.accept("PRIMARY"):
                    parser.expect("KEY")
                    if kind != "bigint" or primary is not None:
                        raise SQLFailure("invalid primary key")
                    primary = len(columns)
                columns.append({"name": col_name, "type": kind, "size": size})
                if not parser.accept(","):
                    break
            parser.expect(")")
            if primary is None or vector_count > 1:
                raise SQLFailure("one BIGINT primary key and at most one VECTOR required")
            rows = DiskRows(self.materialized, name, columns, self.cache_budget) if materialize else {}
            tables[name] = {"columns": columns, "primary": primary, "rows": rows}
            return {}
        if keyword == "INSERT":
            parser.expect("INSERT"); parser.expect("INTO")
            table = self._table(tables, parser.ident())
            order = list(range(len(table["columns"])))
            if parser.accept("("):
                order = []
                while True:
                    order.append(column_index(table, parser.ident()))
                    if not parser.accept(","):
                        break
                parser.expect(")")
                if len(set(order)) != len(order) or len(order) != len(table["columns"]):
                    raise SQLFailure("all columns required exactly once")
            parser.expect("VALUES")
            pending = []
            while True:
                parser.expect("(")
                row = [None] * len(table["columns"])
                for offset, index in enumerate(order):
                    if offset:
                        parser.expect(",")
                    row[index] = as_value(parser.literal(), table["columns"][index])
                parser.expect(")")
                pending.append(row)
                if not parser.accept(","):
                    break
            keys = [row[table["primary"]] for row in pending]
            if len(set(keys)) != len(keys) or any(str(key) in table["rows"] for key in keys):
                raise SQLFailure("duplicate primary key")
            for key, row in zip(keys, pending):
                table["rows"][str(key)] = row
            return {"affected_rows": len(pending)}
        if keyword == "UPDATE":
            parser.expect("UPDATE")
            table = self._table(tables, parser.ident())
            parser.expect("SET")
            changes = []
            while True:
                index = column_index(table, parser.ident())
                if index == table["primary"]:
                    raise SQLFailure("primary key update unsupported")
                parser.expect("=")
                changes.append((index, as_value(parser.literal(), table["columns"][index])))
                if not parser.accept(","):
                    break
            tests = predicate(parser, table)
            affected = 0
            for key, row in candidate_items(table, tests):
                if matches(row, tests):
                    changed = list(row)
                    for index, value in changes:
                        changed[index] = value
                    table["rows"][key] = changed
                    affected += 1
            return {"affected_rows": affected}
        if keyword == "DELETE":
            parser.expect("DELETE"); parser.expect("FROM")
            table = self._table(tables, parser.ident())
            tests = predicate(parser, table)
            keys = [key for key, row in candidate_items(table, tests) if matches(row, tests)]
            for key in keys:
                del table["rows"][key]
            return {"affected_rows": len(keys)}
        raise SQLFailure("unsupported statement")

    @staticmethod
    def _table(tables: dict, name: str) -> dict:
        if name not in tables:
            raise SQLFailure(f"unknown table {name}")
        return tables[name]

    def _select(self, parser: Parser, tables: dict) -> dict:
        parser.expect("SELECT")
        projections = []
        if parser.accept("*"):
            projections.append(("star", None, None, None))
        else:
            while True:
                if parser.accept("L2_DISTANCE"):
                    parser.expect("(")
                    name = parser.ident()
                    parser.expect(",")
                    target = parser.literal()
                    parser.expect(")")
                    parser.expect("AS")
                    alias = parser.ident()
                    projections.append(("distance", name, target, alias))
                else:
                    name = parser.ident()
                    projections.append(("column", name, None, name))
                if not parser.accept(","):
                    break
        parser.expect("FROM")
        table = self._table(tables, parser.ident())
        if projections[0][0] == "star":
            projections = [("column", c["name"], None, c["name"]) for c in table["columns"]]
        resolved = []
        for kind, name, raw_target, alias in projections:
            index = column_index(table, name)
            target = None
            if kind == "distance":
                column = table["columns"][index]
                if column["type"] != "vector":
                    raise SQLFailure("L2_DISTANCE requires VECTOR")
                target = as_value(raw_target, column)
            resolved.append((kind, index, target, alias))
        tests = predicate(parser, table)
        order = None
        descending = False
        if parser.accept("ORDER"):
            parser.expect("BY")
            order = parser.ident()
            if parser.accept("DESC"):
                descending = True
            else:
                parser.accept("ASC")
            if order not in [x["name"] for x in table["columns"]] + [x[3] for x in resolved]:
                raise SQLFailure("unknown ORDER BY expression")
        limit = None
        if parser.accept("LIMIT"):
            limit = parser.unsigned()
        distance_columns = [item for item in resolved if item[0] == "distance"]
        if (
            limit is not None and not descending and len(distance_columns) == 1
            and order == distance_columns[0][3]
            and isinstance(table["rows"], DiskRows)
        ):
            hits = table["rows"].native_topk(distance_columns[0][2], tests, limit)
            if hits is not None:
                output_rows = []
                for row, exact_distance in hits:
                    projected = []
                    for kind, index, _, _ in resolved:
                        if kind == "distance":
                            projected.append(exact_distance)
                        else:
                            value = row[index]
                            projected.append(list(value) if isinstance(value, array) else value)
                    output_rows.append(projected)
                return {"columns": [item[3] for item in resolved], "rows": output_rows}
        def selected_rows():
            for _, row in candidate_items(table, tests, use_cache=True):
                if not matches(row, tests):
                    continue
                output = []
                for kind, index, target, _ in resolved:
                    if kind == "distance":
                        output.append(distance(row[index], target))
                    else:
                        value = row[index]
                        output.append(list(value) if isinstance(value, array) else value)
                yield row, output

        selected = selected_rows()
        if order is not None:
            alias_positions = {item[3]: i for i, item in enumerate(resolved)}
            col_positions = {item["name"]: i for i, item in enumerate(table["columns"])}
            def compare(left, right):
                a = left[1][alias_positions[order]] if order in alias_positions else left[0][col_positions[order]]
                b = right[1][alias_positions[order]] if order in alias_positions else right[0][col_positions[order]]
                value = (a > b) - (a < b)
                if value:
                    return -value if descending else value
                pa = left[0][table["primary"]]
                pb = right[0][table["primary"]]
                return (pa > pb) - (pa < pb)
            key = cmp_to_key(compare)
            if limit is not None:
                selected = heapq.nsmallest(limit, selected, key=key)
            else:
                selected = sorted(selected, key=key)
        elif limit is not None:
            selected = itertools.islice(selected, limit)
        return {"columns": [item[3] for item in resolved], "rows": [item[1] for item in selected]}


def main() -> int:
    arguments = argparse.ArgumentParser()
    arguments.add_argument("--data-dir", default="/data")
    args = arguments.parse_args()
    database = MiniVectorDB(args.data_dir)
    try:
        for line in sys.stdin:
            if not line.strip():
                continue
            try:
                result = database.execute(line)
            except (SQLFailure, ValueError) as exc:
                result = error_response(exc)
            print(json.dumps(result, ensure_ascii=False, separators=(",", ":")), flush=True)
    finally:
        database.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
