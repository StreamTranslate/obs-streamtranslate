from http.server import ThreadingHTTPServer,BaseHTTPRequestHandler
import time
class Handler(BaseHTTPRequestHandler):
 def log_message(self,*a):pass
 def do_GET(self):
  assert not self.headers.get('Authorization') and not self.headers.get('Cookie')
  assert 'pluginKey' not in self.path
  if self.path=='/slow':time.sleep(15)
  body={'/ok':b'{"status":"ok"}','/bad':b'{"status":"failed"}','/invalid':b'not json','/large':b'x'*6000}.get(self.path,b'{"status":"ok"}')
  self.send_response(302 if self.path=='/redirect' else 503 if self.path=='/error' else 200)
  if self.path=='/redirect':self.send_header('Location','http://127.0.0.1:8798/ok')
  self.send_header('Content-Length',str(len(body)));self.end_headers()
  try:self.wfile.write(body)
  except (BrokenPipeError,ConnectionResetError):pass
ThreadingHTTPServer(('127.0.0.1',8798),Handler).serve_forever()
