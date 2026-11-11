from contextlib import closing
import hashlib, heapq, json, math, os, random, shutil, statistics, struct, subprocess, sys, tempfile, time
from pathlib import Path

root=Path.cwd().parent
protocol=root/"minivectordb-collab-partner"/"protocol_check"
sys.path.insert(0,str(protocol))
from _transport import Session
cfg=json.loads((protocol/"config.json").read_text())
work=Path(tempfile.mkdtemp(prefix="mvd-q8-large-",dir=root/"contest-runtime"))
print("cold_artifacts",work,flush=True)
images={"baseline":"mvd-exact-candidate:continuous-lto","candidate":"mvd-exact-candidate:q8-unrolled"}
images_ids={key:subprocess.check_output(["docker","image","inspect","--format","{{.Id}}",value],text=True).strip() for key,value in images.items()}
def execute(session,sql):
    result=session.request(sql)
    assert result["ok"], result
    return result
def floats(values):
    return struct.unpack(f"<{len(values)}f",struct.pack(f"<{len(values)}f",*(float(v) for v in values)))

rng=random.Random(20261112)
prototypes=[[f"{rng.uniform(-1,1):.6f}" for _ in range(127)] for _ in range(1024)]
norms=[math.fsum(x*x for x in floats(proto)) for proto in prototypes]
literals=[",".join(proto) for proto in prototypes]
rows=100000
distances=[]
sql="SELECT id,L2_DISTANCE(v,'["+",".join(["0"]*128)+"]') AS d FROM t ORDER BY d LIMIT 10;"
with closing(Session(cfg, work/"load.stderr", image=images["baseline"], data_root="/var/tmp")) as seed:
    seed.start()
    execute(seed, "CREATE TABLE t (id BIGINT PRIMARY KEY, v VECTOR(128));")
    for first in range(1,rows+1,500):
        batch=[]
        for key in range(first,first+500):
            index=(key-1)%len(prototypes)
            last=f"{key/131072:.10f}"
            last_value,=floats([last])
            distances.append((math.sqrt(math.fsum([norms[index],last_value*last_value])), key))
            batch.append(f"({key},'[{literals[index]},{last}]')")
        assert execute(seed,"INSERT INTO t VALUES "+",".join(batch)+";")["affected_rows"]==len(batch)
    seed.shutdown()
    snapshot=Path(seed.bind_data.name)
    expected=[[key,distance] for distance,key in heapq.nsmallest(10,distances)]
    report={"scope":"local fresh process; saved data copied before timing; OS cache not cleared",
        "rows":rows,"dimension":128,"config":cfg,"images":images_ids,"oracle_topk":expected,"runs":[]}
    def verify(response):
        assert response["columns"]==["id","d"]
        actual=response["rows"]
        assert [row[0] for row in actual]==[row[0] for row in expected], (actual, expected)
        assert all(math.isclose(a[1],b[1],rel_tol=2e-14) for a,b in zip(actual,expected))
    def cpu_usage(cgroup):
        return int(dict(line.split() for line in (cgroup/"cpu.stat").read_text().splitlines())["usage_usec"])
    a,b=images
    for i,name in enumerate((a,b,b,a,b,a,a,b,a,b,b,a),1):
        with closing(Session(cfg,work/f"{i:02d}-{name}.stderr",image=images[name],data_root="/var/tmp")) as session:
            shutil.copytree(snapshot,session.bind_data.name,dirs_exist_ok=True)
            session.start()
            pid=session.docker("inspect",session.name,"--format","{{.State.Pid}}").stdout.decode().strip()
            cgroup=Path("/sys/fs/cgroup")/Path("/proc",pid,"cgroup").read_text().split("0::",1)[1].strip().lstrip("/")
            cpu_before=cpu_usage(cgroup)
            start=time.perf_counter()
            response=execute(session,sql)
            elapsed=time.perf_counter()-start
            cpu_after=cpu_usage(cgroup)
            verify(response)
            warm=[]
            for _ in range(128):
                start=time.perf_counter()
                response=execute(session,sql)
                warm.append(time.perf_counter()-start)
                verify(response)
            memory=int((cgroup/"memory.peak").read_text())
            session.shutdown()
        r={"version":name,"first_query_ms":elapsed*1000,
            "first_query_cpu_ms":(cpu_after-cpu_before)/1000,
            "warm_query_ms":statistics.median(warm)*1000,
            "memory_peak_bytes":memory,"queries_verified":129}
        report["runs"].append(r)
        print(i,name, {k:round(v,3) for k,v in r.items() if isinstance(v,float)}, flush=True)
        (work/"cold-comparison.json").write_text(json.dumps(report,indent=2)+"\n")
keys=("first_query_ms","first_query_cpu_ms","warm_query_ms","memory_peak_bytes")
report["summary"]={}
for name in images:
    subset=[r for r in report["runs"] if r["version"]==name]
    report["summary"][name]={k:{"median":statistics.median(r[k] for r in subset),"min":min(r[k] for r in subset),"max":max(r[k] for r in subset)} for k in keys}
(work/"cold-comparison.json").write_text(json.dumps(report,indent=2)+"\n")
print("COLD_SUMMARY",json.dumps(report["summary"]),flush=True)
