"""Run against an isolated local seekdb and ai_function_mock.py on port 8000."""
import json, subprocess, urllib.request, argparse, os

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--host', default='127.0.0.1')
parser.add_argument('--port', default=os.getenv('MYSQL_TEST_PORT','10000'))
parser.add_argument('--user', default='root')
args=parser.parse_args()
client=['mysql','--no-defaults','-h'+args.host,'-P'+args.port,'-u'+args.user]

def sql(query, fail=False):
    p=subprocess.run(client+[
                      '-N','-B','--raw','--default-character-set=utf8mb4','kernel_regression','-e',query],
                     text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=45)
    assert (p.returncode!=0) if fail else (p.returncode==0), (query,p.returncode,p.stdout,p.stderr)
    return p.stdout.strip()
def stats(): return json.load(urllib.request.urlopen('http://127.0.0.1:8000/stats'))
def endpoint(model,path,provider='openai',kind='dense_embedding'):
    sql(f"CALL DBMS_AI_SERVICE.CREATE_AI_MODEL('{model}', '{{\"type\":\"{kind}\",\"model_name\":\"fixture\"}}');")
    config=dict(ai_model_name=model,url='http://127.0.0.1:8000'+path,provider=provider,access_key='' if provider.lower()=='ollama' else 'fixture')
    sql(f"CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT('{model}_ep', '{json.dumps(config)}');")
    models.append(model)

models=[]
subprocess.run(client+['-e','DROP DATABASE IF EXISTS kernel_regression; CREATE DATABASE kernel_regression;'],check=True)
def cleanup(model):
    for stmt in (f"CALL DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT('{model}_ep');",f"CALL DBMS_AI_SERVICE.DROP_AI_MODEL('{model}');"):
        subprocess.run(client+['-e',stmt],text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=15)
for model in ('reg_complete','reg_ollama','reg_embed','reg_rerank'):
    cleanup(model)
try:
    endpoint('reg_complete','/v1/chat/completions',kind='completion')
    assert sql("SELECT AI_COMPLETE('reg_complete','hello',NULL)")=='hello'
    assert sql("SELECT AI_COMPLETE('reg_complete','你好',JSON_OBJECT('temperature',0.2,'max_tokens',16))")=='你好'
    sql("SELECT AI_COMPLETE('reg_complete','hello','[]')",fail=True)
    sql("CREATE TABLE lob_input(id INT PRIMARY KEY, content LONGTEXT)")
    sql("INSERT INTO lob_input VALUES(1,REPEAT('中',30000))")
    assert sql("SELECT CHAR_LENGTH(AI_COMPLETE('reg_complete',content)) FROM lob_input")=='30000'
    for path in ('/empty/v1/chat/completions','/wrong-content/v1/chat/completions','/array-root/v1/chat/completions','/close/v1/chat/completions'):
        sql("CALL DBMS_AI_SERVICE.ALTER_AI_MODEL_ENDPOINT('reg_complete_ep', '"+json.dumps(dict(url='http://127.0.0.1:8000'+path))+"');")
        sql("SELECT AI_COMPLETE('reg_complete','hello')",fail=True)
    sql("CALL DBMS_AI_SERVICE.ALTER_AI_MODEL_ENDPOINT('reg_complete_ep','{\"url\":\"http://127.0.0.1:8000/partial-retry/v1/chat/completions\"}');")
    assert sql("SELECT AI_COMPLETE('reg_complete','retry')")=='retry'
    endpoint('reg_ollama','/api/generate',provider='Ollama',kind='completion')
    assert sql("SELECT AI_COMPLETE('reg_ollama','native','{\"temperature\":0.3,\"max_tokens\":17}')")=='native'
    assert sql("SELECT AI_COMPLETE('reg_ollama','options','{\"options\":{\"seed\":7},\"temperature\":0.4}')")=='options'
    native=[r['body'] for r in stats() if r['path']=='/api/generate']
    assert native[-2]['options']==dict(temperature=0.3,num_predict=17)
    assert native[-1]['options']==dict(seed=7,temperature=0.4)
    assert all(r['stream'] is False for r in native)
    endpoint('reg_embed','/v1/embeddings')
    assert sql("SELECT JSON_LENGTH(AI_EMBED('reg_embed','hello',4))")=='4'
    before=len(stats())
    for dim in ('0','-1','NULL','1.5',"'4'",'18446744073709551615'):
        sql("SELECT AI_EMBED('reg_embed','hello',"+dim+")",fail=True)
    assert sql("SELECT AI_EMBED('reg_embed',NULL) IS NULL")=='1'
    assert len(stats())==before, 'NULL or invalid dimension reached the model'
    sql("CREATE TABLE inputs(id INT PRIMARY KEY, content LONGTEXT)")
    values=','.join(f"({i},"+('NULL' if i%7==0 else "'"+'x'*(i+1)+"'")+")" for i in range(160))
    sql('INSERT INTO inputs VALUES'+values)
    before=len(stats())
    output=sql("SELECT id, JSON_EXTRACT(AI_EMBED('reg_embed',content,4),'$[0]') FROM inputs WHERE id>3 ORDER BY id LIMIT 140")
    for line in output.splitlines():
        i,value=line.split('\t'); i=int(i)
        assert value=='NULL' if i%7==0 else float(value)==i+1, (i,value)
    requests=[r for r in stats()[before:] if 'input' in r['body']]
    sizes=[len(r['body']['input']) for r in requests]
    print('batch_request_sizes',sizes,flush=True)
    assert sizes and max(sizes)>1, 'No cross-row batch request observed'
    assert all(size<=32 for size in sizes)
    assert sum(sizes)==120, 'Filtered or LIMIT-excluded rows entered the request'
    assert all(text is not None for r in requests for text in r['body']['input'])
    varying=sql("SELECT id, JSON_LENGTH(AI_EMBED('reg_embed',content,CASE WHEN id%2=0 THEN 4 ELSE 8 END)) FROM inputs WHERE id<12 ORDER BY id")
    for line in varying.splitlines():
        i,value=line.split('\t');i=int(i)
        assert value=='NULL' if i%7==0 else int(value)==(4 if i%2==0 else 8), (i,value)
    ordinary=sql("SELECT id,JSON_LENGTH(JSON_OBJECT('id',id)),JSON_EXTRACT(JSON_OBJECT('id',id),'$.id') FROM inputs ORDER BY id")
    assert all(line.split('\t')==[str(i),'1',str(i)] for i,line in enumerate(ordinary.splitlines()))
    assert sql("SELECT JSON_EXTRACT(NULL,'$['),JSON_LENGTH(NULL,'$[')")=='NULL\tNULL'
    before=len(stats())
    assert sql("SELECT AI_EMBED('reg_embed',content,4) FROM inputs WHERE 1=0")==''
    assert sql("SELECT AI_EMBED('reg_embed',content,4) FROM inputs LIMIT 0")==''
    assert len(stats())==before, 'Zero-row query called the model'
    result=sql("SELECT id, CASE WHEN id%2=0 THEN JSON_EXTRACT(AI_EMBED('reg_embed',content,4),'$[0]') ELSE NULL END FROM inputs ORDER BY id")
    for line in result.splitlines():
        i,value=line.split('\t');i=int(i)
        assert value=='NULL' if i%2 or i%7==0 else float(value)==i+1, (i,value)
    for fault in ('bad-vector','bad-dimension','missing','duplicate'):
        sql("CALL DBMS_AI_SERVICE.ALTER_AI_MODEL_ENDPOINT('reg_embed_ep', '"+json.dumps(dict(url='http://127.0.0.1:8000/'+fault+'/v1/embeddings'))+"');")
        sql("SELECT AI_EMBED('reg_embed',content,4) FROM inputs WHERE content IS NOT NULL",fail=True)
    endpoint('reg_rerank','/v1/rerank',provider='siliconflow',kind='rerank')
    docs=[dict(id=str(i),text='match' if i%3==0 else 'other') for i in range(53)]
    output=sql("SELECT AI_RERANK('reg_rerank','match','"+json.dumps(docs)+"','text')")
    assert [d['id'] for d in json.loads(output)]==[str(i) for i in sorted(range(53),key=lambda i:(i%3!=0,i))]
    strings=json.dumps([d['text'] for d in docs])
    result=json.loads(sql("SELECT AI_RERANK('reg_rerank','match','"+strings+"',NULL)"))
    assert [d['index'] for d in result]==sorted(range(53),key=lambda i:(i%3!=0,i))
    for fault in ('duplicate','out-of-range','bad-score'):
        sql("CALL DBMS_AI_SERVICE.ALTER_AI_MODEL_ENDPOINT('reg_rerank_ep', '"+json.dumps(dict(url='http://127.0.0.1:8000/'+fault+'/v1/rerank'))+"');")
        sql("SELECT AI_RERANK('reg_rerank','match','[\"a\",\"b\"]',NULL)",fail=True)
    assert sql('SELECT 1')=='1'
    print('ai_function_regression=passed',flush=True)
finally:
    for model in reversed(models):
        cleanup(model)
