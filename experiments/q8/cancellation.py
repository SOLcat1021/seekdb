from contextlib import closing
from pathlib import Path
import json, subprocess, sys, tempfile

root = Path.cwd().parent
sys.path.insert(0,str(root/'minivectordb-collab-partner'/'protocol_check'))
from _transport import Session
cfg=json.loads((root/'minivectordb-collab-partner'/'protocol_check'/'config.json').read_text())
out=Path(tempfile.mkdtemp(prefix='mvd-q8-cancellation-',dir=root/'contest-runtime'))
results=[]
with tempfile.TemporaryDirectory(dir='/var/tmp') as tmp:
    binary=Path(tmp)/'minivector'
    container=subprocess.check_output(['docker','create','mvd-exact-candidate:q8-unrolled'],text=True).strip()
    try:
        subprocess.run(['docker','cp',container+':/usr/local/bin/minivector',str(binary)],check=True)
    finally:
        subprocess.run(['docker','rm',container],check=True,stdout=subprocess.DEVNULL)
    for budget in (0,64):
        with closing(Session(cfg,out/f'cache-{budget}.stderr',command=[str(binary),'--data','{data_dir}','--cache-mb',str(budget)])) as session:
            session.start()
            def execute(sql):
                response=session.request(sql)
                assert response['ok'],response
                return response
            execute('CREATE TABLE t (id BIGINT PRIMARY KEY,v VECTOR(2));')
            execute('INSERT INTO t VALUES '+','.join(f"({i},'[100000000,1]')" for i in range(1,101))+",(101,'[100000000,0]');")
            actual=execute("SELECT id,L2_DISTANCE(v,'[100000000,0]') AS d FROM t ORDER BY d LIMIT 1;")['rows']
            assert actual==[[101,0.0]],actual
            results.append({'cache_mb':budget,'rows':actual,'expected':[[101,0.0]],'passed':True})
            session.shutdown()
(out/'result.json').write_text(json.dumps(results,indent=2)+'\n')
print(out,results)
