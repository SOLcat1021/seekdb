from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import json, os, subprocess, sys, tempfile

root=Path.cwd().parent
runtime=root/"contest-runtime"
artifacts=Path(tempfile.mkdtemp(prefix="mvd-q8-unrolled-validation-", dir=runtime))
print("validation_artifacts", artifacts, flush=True)
image="mvd-exact-candidate:q8-unrolled"
protocol=root/"minivectordb-collab-partner"/"protocol_check"
suite=root/"minivectordb-exact-candidate"/"tools"/"env"

def copy_binary(binary):
    container=subprocess.check_output(["docker","create",image],text=True).strip()
    try:
        subprocess.run(["docker","cp",container+":/usr/local/bin/minivector",str(binary)],check=True)
    finally:
        subprocess.run(["docker","rm",container],check=True,stdout=subprocess.DEVNULL)

def run(name,args):
    result=subprocess.run([sys.executable,*map(str,args)],text=True,capture_output=True)
    (artifacts/(name+".log")).write_text(result.stdout+result.stderr)
    print(name,"exit",result.returncode,"\n"+"\n".join((result.stdout+result.stderr).splitlines()[-12:]),flush=True)
    return result.returncode

with tempfile.TemporaryDirectory(prefix="mvd-screen-bin-",dir="/var/tmp") as temp:
    binary=Path(temp)/"minivector"
    copy_binary(binary)
    checks = {
        "public": [protocol/"run.py","--image",image,"--package",Path.cwd()/"mysqltest","--output",artifacts/"public"],
        "exact": [suite/"test_partner_exact.py","--binary",binary,"--output",artifacts/"exact"],
        "large": [suite/"test_partner_large.py","--binary",binary,"--output",artifacts/"large"],
        "recovery": [runtime/"minivectordb_bounded_commit_regression_20261111.py","--protocol-dir",protocol,"--image",image,"--out",artifacts/"recovery.json"],
    }
    with ThreadPoolExecutor(max_workers=3) as pool:
        tasks={key:pool.submit(run,key,args) for key,args in checks.items()}
        status={name:task.result() for name,task in tasks.items()}
print("RESULT",json.dumps(status),flush=True)
sys.exit(any(status.values()))
