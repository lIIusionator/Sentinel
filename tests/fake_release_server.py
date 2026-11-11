"""A stand-in for GitHub's release API, for tests/update_e2e.sh: it lets the exe's update path run against a server on this PC.

usage: fake_release_server.py <port> <dir> <mode>        <dir> holds SentinelWatch.exe, the "new" program the release offers
modes: ok         a newer release (v1.0.1) whose files are good
       badsum     SHA256SUMS.txt lists a wrong hash
       truncate   the exe download stops half way (Content-Length promises more)
       same       the latest release is v1.0, the version the test exe says it is
       prerelease the latest release is marked pre-release
       notfound   GitHub's answer when a repository has no release (HTTP 404)
Every request is written to stderr as "[fake] ...", which the test script reads.
"""
import hashlib, http.server, json, os, sys

port, root, mode = int(sys.argv[1]), sys.argv[2], sys.argv[3]
exe = open(os.path.join(root, 'SentinelWatch.exe'), 'rb').read()
sums = ('0' * 64 if mode == 'badsum' else hashlib.sha256(exe).hexdigest()) + '  SentinelWatch.exe\n'
base = 'http://127.0.0.1:%d' % port
release = {'tag_name': 'v1.0' if mode == 'same' else 'v1.0.1', 'draft': False, 'prerelease': mode == 'prerelease', 'assets': [
    {'name': 'SentinelWatch.exe', 'browser_download_url': base + '/dl/SentinelWatch.exe'},
    {'name': 'SHA256SUMS.txt', 'browser_download_url': base + '/dl/SHA256SUMS.txt'}]}


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        sys.stderr.write('[fake] ' + (fmt % args) + '\n')
        sys.stderr.flush()

    def reply(self, code, body, ctype='application/octet-stream', send=None):
        self.send_response(code)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(body)))       # the full length, even when fewer bytes follow (mode truncate)
        self.end_headers()
        self.wfile.write(body if send is None else body[:send])

    def do_GET(self):
        if self.path == '/latest.json':
            if mode == 'notfound':
                self.reply(404, b'{"message":"Not Found"}', 'application/json')
            else:
                self.reply(200, json.dumps(release).encode(), 'application/json')
        elif self.path == '/dl/SHA256SUMS.txt':
            self.reply(200, sums.encode(), 'text/plain')
        elif self.path == '/dl/SentinelWatch.exe':
            self.reply(200, exe, send=len(exe) // 2 if mode == 'truncate' else None)
        else:
            self.reply(404, b'')


http.server.ThreadingHTTPServer(('127.0.0.1', port), Handler).serve_forever()
