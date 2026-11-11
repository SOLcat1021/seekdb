from pathlib import Path
import json, statistics, subprocess, sys, tempfile

root = Path.cwd().parent
workspace = Path(tempfile.mkdtemp(prefix="mvd-q8-diverse-large-", dir=root/"contest-runtime"))
print("artifacts", workspace, flush=True)
sys.path.insert(0, str(root/"contest-runtime"/"minivectordb-comparison-20261110"))
import run_compare as bench
cfg = json.loads((bench.WORKLOAD_REPO/"protocol_check/config.json").read_text())
import random, struct
def larger_workload(count):
    rng = random.Random(20261112)
    binary = bytearray(struct.pack("<4I", 100000, 128, count, 10))
    inserts = []
    batch = []
    for i in range(100000):
        values = [round(rng.uniform(-1,1),6) for _ in range(128)]
        binary.extend(struct.pack("<128f",*values))
        literal = "'[" + ",".join("%.6f"%x for x in values) + "]'"
        batch.append("(%d,'c%d',%s)"%(i,i%10,literal))
        if len(batch)==100:
            inserts.append("INSERT INTO bench VALUES "+",".join(batch)+";")
            batch=[]
    queries=[]
    for i in range(count):
        values=[round(rng.uniform(-1,1),6) for _ in range(128)]
        binary.extend(struct.pack("<128f",*values))
        literal="'["+",".join("%.6f"%x for x in values)+"]'"
        predicate=" WHERE cat='c%d'"%(i%10) if i%2 else ""
        queries.append("SELECT id,L2_DISTANCE(embedding,%s) AS d FROM bench%s ORDER BY d LIMIT 10;"%(literal,predicate))
    return inserts,queries,bytes(binary)
inserts, queries, binary = larger_workload(128)
truth, key = bench.oracle(binary, workspace/"oracle.json")
original_snapshot = bench.snapshot
def snapshot(cgroup):
    data = original_snapshot(cgroup)
    data["cpu.stat"] = (cgroup/"cpu.stat").read_text().strip()
    return data
bench.snapshot = snapshot
images = {
    "baseline": "mvd-exact-candidate:continuous-lto",
    "candidate": "mvd-exact-candidate:q8-unrolled",
}
meta = {name: subprocess.check_output(["docker","image","inspect","--format","{{.Id}}",image], text=True).strip() for name,image in images.items()}
a,b = images
sequence = (a,b,b,a, b,a,a,b, a,b,b,a)
report = {"scope":"local", "image_ids":meta, "oracle_key":key, "rows":100000,
          "dimensions":128, "config":cfg, "runs":[]}

def cpu_use(data):
    return int(dict(line.split() for line in data["cpu.stat"].splitlines())["usage_usec"])

for i,name in enumerate(sequence, 1):
    row = bench.run(images[name], bench.Session, cfg, inserts, queries, truth, workspace/f"{i:02d}-{name}.stderr")
    phases = row["cgroup_phases"]
    row["write_cpu_seconds"] = (cpu_use(phases["loaded"])-cpu_use(phases["created"]))/1e6
    row["query_cpu_seconds"] = (cpu_use(phases["queried"])-cpu_use(phases["loaded"]))/1e6
    row["version"] = name
    report["runs"].append(row)
    (workspace/"comparison.json").write_text(json.dumps(report, indent=2)+"\n")
    print(i,name,{k:round(row[k],5) for k in ("write_seconds","query_seconds","write_cpu_seconds","query_cpu_seconds")},flush=True)

metrics = ("write_seconds","query_seconds","write_cpu_seconds","query_cpu_seconds","memory_peak_bytes_before_shutdown")
report["summary"] = {}
for name in images:
    rows = [r for r in report["runs"] if r["version"]==name]
    report["summary"][name] = {metric:{"median":statistics.median(r[metric] for r in rows), "min":min(r[metric] for r in rows), "max":max(r[metric] for r in rows)} for metric in metrics}
    report["summary"][name]["queries_verified"] = sum(r["queries_verified"] for r in rows)
(workspace/"comparison.json").write_text(json.dumps(report,indent=2)+"\n")
print("SUMMARY",json.dumps(report["summary"]),flush=True)
