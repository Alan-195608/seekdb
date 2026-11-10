#!/usr/bin/env python3
"""Run public semantic assertions and focused regressions against the local adapter."""
import argparse
import json
import math
from pathlib import Path
import random
import subprocess
import tempfile


class Session:
    def __init__(self, binary, directory):
        self.start([binary, "--data", directory])

    def start(self, command):
        self.process = subprocess.Popen(
            command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            encoding="utf-8", bufsize=1,
        )
        self.request_id = 0
        self.mode = "exact"
        assert self.read() == {"type": "ready"}

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        if self.process.poll() is None:
            try:
                assert self.request("shutdown")["ok"] is True
            finally:
                self.process.stdin.close()
                assert self.process.wait(timeout=10) == 0
            assert self.process.stdout.read() == ""
        self.process.stdout.close()

    def read(self):
        line = self.process.stdout.readline()
        assert line.endswith("\n"), f"unexpected EOF, status={self.process.poll()}"
        return json.loads(line)

    def request(self, op, **fields):
        self.request_id += 1
        self.process.stdin.write(json.dumps({"id": self.request_id, "op": op, **fields}) + "\n")
        self.process.stdin.flush()
        response = self.read()
        assert type(response["id"]) is int and response["id"] == self.request_id
        assert type(response["ok"]) is bool
        return response

    def sql(self, sql):
        response = self.request("execute", sql=sql, search_mode=self.mode)
        if not response["ok"]:
            return {"error": response["error"]["code"]}
        return response

    def kill(self):
        self.process.kill()
        assert self.process.wait(timeout=10) < 0
        self.process.stdin.close()


def assert_result(actual, expected):
    if "error" in expected:
        assert actual.get("error", "").lower() == expected["error"], (actual, expected)
        return
    assert "error" not in actual, actual
    for key in ("affected_rows", "columns"):
        if key in expected:
            assert actual[key] == expected[key], (actual, expected)
    if "rows" not in expected:
        return
    actual, rows = actual["rows"], expected["rows"]
    assert len(actual) == len(rows), (len(actual), len(rows))
    if expected.get("unordered"):
        actual, rows = sorted(actual), sorted(rows)
    for got, wanted in zip(actual, rows):
        assert len(got) == len(wanted)
        for value, target in zip(got, wanted):
            if isinstance(target, float):
                assert type(value) in (int, float) and math.isclose(value, target, rel_tol=1e-5, abs_tol=1e-5)
            elif isinstance(target, list):
                assert isinstance(value, list)
                assert all(math.isclose(x, y, abs_tol=1e-5, rel_tol=1e-5)
                           for x, y in zip(value, target, strict=True))
            else:
                assert type(value) is type(target) and value == target, (value, target)


def public_cases(binary, root):
    manifest = json.loads((root / "manifest.json").read_text())
    count = 0
    for case in manifest["cases"]:
        name = case["id"]
        test_path = root / case.get("test", f"cases/{name}.test")
        result_path = root / case.get("result", f"cases/{name}.result")
        statements = [line.strip() for line in test_path.read_text().splitlines() if line.strip()]
        expected = iter(json.loads(result_path.read_text()))
        executed = 0
        with tempfile.TemporaryDirectory() as directory:
            db = Session(binary, directory)
            try:
                for sql in statements:
                    if sql == "--restart":
                        db.__exit__()
                        db = Session(binary, directory)
                        continue
                    if sql.startswith("--mode "):
                        db.mode = sql.split()[1]
                        continue
                    try:
                        assert_result(db.sql(sql), next(expected))
                    except AssertionError as error:
                        raise AssertionError(f"{name}: {sql}: {error}") from error
                    executed += 1
                assert next(expected, None) is None
                assert executed == case["statements"]
            finally:
                db.__exit__()
        count += executed
    print(f"Public SQL: {len(manifest['cases'])} cases, {count} statements passed")


def crash_and_statement_atomicity(binary):
    with tempfile.TemporaryDirectory() as directory:
        with Session(binary, directory) as db:
            assert_result(db.sql("CREATE TABLE t(id BIGINT PRIMARY KEY,c VARCHAR(1000),v VECTOR(2))"), {})
            assert_result(db.sql("INSERT INTO t VALUES(1,'猫',[1,0])"), {"affected_rows": 1})
            assert_result(db.sql("BEGIN"), {})
            assert_result(db.sql("UPDATE t SET c='准备提交' WHERE id=1"), {"affected_rows": 1})
            # Force multiple physical flushes before a duplicate at the end of one SQL statement.
            text = "x" * 960
            batch = ",".join(f"({i},'{text}',[0,0])" for i in range(2, 2500))
            assert db.sql(f"INSERT INTO t VALUES {batch},(1,'duplicate',[0,0])")["error"] == "duplicate_key"
            assert_result(db.sql("SELECT id,c FROM t"), {"rows": [[1, "准备提交"]]})
            assert_result(db.sql("COMMIT"), {})
            assert_result(db.sql("CREATE VECTOR INDEX v_idx ON t(v)"), {})
            assert_result(db.sql("BEGIN"), {})
            assert_result(db.sql("DELETE FROM t WHERE id=1"), {"affected_rows": 1})
            assert_result(db.sql("INSERT INTO t VALUES(1,'未提交',[0,0]),(2,'未提交',[0,0])"), {})
            db.mode = "ann"
            assert_result(db.sql("SELECT id FROM t ORDER BY L2_DISTANCE(v,[0,0]) LIMIT 1"),
                          {"rows": [[1]]})
            db.kill()

        # Repeat recovery against the same directory.
        for _ in range(3):
            with Session(binary, directory) as db:
                assert_result(db.sql("SELECT id,c FROM t"), {"rows": [[1, "准备提交"]]})
                db.mode = "ann"
                assert_result(db.sql("SELECT id,L2_DISTANCE(v,[0,0]) AS d FROM t ORDER BY d LIMIT 1"),
                              {"rows": [[1, 1.0]]})
                assert_result(db.sql("BEGIN"), {})
                assert_result(db.sql("DELETE FROM t WHERE id=1"), {"affected_rows": 1})
                assert_result(db.sql("INSERT INTO t VALUES(1,'撤回',[0,0])"), {})
                assert_result(db.sql("ROLLBACK"), {})
                assert_result(db.sql("SELECT id,c FROM t"), {"rows": [[1, "准备提交"]]})
                db.kill()
    print("Statement failure after >2 MiB of staged writes, SIGKILL, and three recoveries passed")


def index_oracle(binary):
    rng = random.Random(39125)
    rows = [(i, i % 4, rng.randrange(-8, 9), rng.randrange(-8, 9)) for i in range(512)]

    def compare(db, rows):
        for mode in ("exact", "ann"):
            db.mode = mode
            for x, y in [(0, 0), (2, -1), (100, 100)]:
                for group in range(5):
                    for k in (1, 10, 600):
                        filtered = [(i, (a - x) ** 2 + (b - y) ** 2)
                                    for i, g, a, b in rows if g == group]
                        filtered.sort(key=lambda item: (item[1], item[0]))
                        expected = [[i, math.sqrt(d)] for i, d in filtered[:k]]
                        sql = (f"SELECT id,L2_DISTANCE(v,[{x},{y}]) AS d FROM t "
                               f"WHERE g={group} ORDER BY d LIMIT {k}")
                        assert_result(db.sql(sql), {"rows": expected})

    with tempfile.TemporaryDirectory() as directory, Session(binary, directory) as db:
        assert_result(db.sql("CREATE TABLE t(id BIGINT,g BIGINT,v VECTOR(2),PRIMARY KEY(id))"), {})
        values = ",".join(f"({i},{g},[{x},{y}])" for i, g, x, y in rows)
        assert_result(db.sql("INSERT INTO t VALUES " + values), {"affected_rows": 512})
        assert_result(db.sql("CREATE VECTOR INDEX v_idx ON t(v)"), {})
        compare(db, rows)
        assert_result(db.sql("BEGIN"), {})
        assert_result(db.sql("DELETE FROM t WHERE id<50"), {"affected_rows": 50})
        assert_result(db.sql("UPDATE t SET g=4,v=[0,0] WHERE id>=500"), {"affected_rows": 12})
        compare(db, [(i, g, x, y) if i < 500 else (i, 4, 0, 0)
                     for i, g, x, y in rows if i >= 50])
        assert_result(db.sql("ROLLBACK"), {})
        compare(db, rows)
        for value in ("[1]", "[NaN,0]", "[1e39,0]", "[0,1,2]"):
            assert "error" in db.sql(f"UPDATE t SET v='{value}' WHERE id=0")
        assert "error" in db.sql("UPDATE t SET id=600 WHERE id=0")
    print("270 exact/indexed TopK comparisons against an independent oracle passed")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=lambda p: str(Path(p).resolve()))
    parser.add_argument("--cases", type=Path, default=Path(__file__).resolve().parents[1] / "mysqltest")
    args = parser.parse_args()
    public_cases(args.binary, args.cases)
    crash_and_statement_atomicity(args.binary)
    index_oracle(args.binary)
