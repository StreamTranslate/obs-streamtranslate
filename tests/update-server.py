"""Local-only HTTP fixtures. No service credentials, customer data or audio."""
import http.server
import json
import time

class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        assert self.headers.get('Authorization') is None
        assert self.headers.get('Cookie') is None
        assert self.headers.get('User-Agent') == 'StreamTranslate-OBS-update-check'
        path = self.path
        status = 200
        data = {'draft': False, 'prerelease': False, 'tag_name': 'v0.1.13'}
        if path == '/equal': data['tag_name'] = 'v0.1.12'
        if path == '/older': data['tag_name'] = 'v0.1.11'
        if path == '/draft': data['draft'] = True
        if path == '/prerelease': data['prerelease'] = True
        if path == '/missing': del data['draft']
        if path == '/wrong-type': data['draft'] = 'false'
        if path == '/markup': data['tag_name'] = '<img src=x onerror=alert(1)>'
        if path == '/blocked': status = 403
        if path == '/redirect': status = 302
        if path == '/slow': time.sleep(12)
        body = json.dumps(data).encode()
        if path == '/oversize': body = b'x' * (128 * 1024 + 1)
        if path == '/malformed': body = b'not json'
        if path == '/nul': body = body + b'\x00junk'
        self.send_response(status)
        if path == '/redirect': self.send_header('Location', 'http://127.0.0.1:8797/new')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        try: self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError): pass

http.server.ThreadingHTTPServer(('127.0.0.1', 8797), Handler).serve_forever()
