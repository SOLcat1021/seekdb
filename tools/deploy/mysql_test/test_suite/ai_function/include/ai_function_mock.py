"""Local protocol fixture for AI-function regression; no external model calls."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json, math, threading

requests = []
lock = threading.Lock()
class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args): pass
    def reply(self, value, status=200):
        body=json.dumps(value,ensure_ascii=False).encode()
        self.send_response(status); self.send_header('Content-Type','application/json')
        self.send_header('Content-Length',str(len(body))); self.end_headers(); self.wfile.write(body)
    def do_GET(self):
        with lock: self.reply(list(requests))
    def do_POST(self):
        body=json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        with lock: requests.append(dict(path=self.path,body=body))
        if '/array-root/' in self.path: return self.reply([])
        if '/close/' in self.path:
            self.close_connection=True
            return
        if '/partial-retry/' in self.path:
            with lock: attempt=sum(r['path']==self.path for r in requests)
            if attempt==1:
                self.send_response(200);self.send_header('Content-Length','1000');self.end_headers()
                self.wfile.write(b'{"partial":');self.wfile.flush();self.close_connection=True
                return
        if '/fail/' in self.path: return self.reply({'error':'fixture failure'},500)
        if '/empty/' in self.path: return self.reply({'choices':[{'message':{'content':''}}]})
        if '/wrong-content/' in self.path: return self.reply({'choices':[{'message':{'content':{}}}]})
        if self.path.endswith('completions'):
            assert isinstance(body['messages'],list)
            return self.reply({'choices':[{'message':{'content':body['messages'][-1]['content']}}]})
        if self.path.endswith('/generate'):
            assert body['stream'] is False
            return self.reply({'response':body['prompt'],'done':True})
        if self.path.endswith('embeddings') or self.path.endswith('/embed'):
            inputs=body['input']; inputs=[inputs] if isinstance(inputs,str) else inputs
            dimension=body.get('dimensions',768)
            if not isinstance(dimension,int) or not 0<dimension<=65536:
                return self.reply({'error':'fixture dimension resource limit'},400)
            data=[dict(index=i,embedding=[float(len(text))]+[float(i)]*(dimension-1)) for i,text in enumerate(inputs)]
            if '/duplicate/' in self.path and len(data)>1: data[1]['index']=0
            if '/bad-vector/' in self.path: data[0]['embedding'][0]='bad'
            if '/bad-dimension/' in self.path: data[0]['embedding'].pop()
            if '/missing/' in self.path: data=data[:-1]
            if self.path.endswith('/embed'): return self.reply({'embeddings':[r['embedding'] for r in data]})
            return self.reply({'data':list(reversed(data))})
        if self.path.endswith('/rerank'):
            data=[dict(index=i,relevance_score=1.0 if body['query'] in text else 0.5)
                  for i,text in enumerate(body['documents'])]
            if '/duplicate/' in self.path and len(data)>1: data[1]['index']=0
            if '/out-of-range/' in self.path: data[0]['index']=len(data)
            if '/bad-score/' in self.path: data[0]['relevance_score']='bad'
            return self.reply({'results':list(reversed(data))})
        self.reply({'error':'unknown fixture route'},404)

if __name__=='__main__':
    ThreadingHTTPServer(('127.0.0.1',8000),Handler).serve_forever()
