"""Exercise the production HTTPS client locally: TLS, redirects, streaming, limits, cancellation and downloads."""
import hashlib
import http.server
import os
from pathlib import Path
import socketserver
import ssl
import subprocess
import sys
import tempfile
import threading
import time

PAYLOAD = b'beacon-update\0' * 20000


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        try:
            self.respond()
        except (BrokenPipeError, ConnectionResetError, ssl.SSLError):
            pass  # Expected when the client rejects a header or a limit, or cancels.

    def respond(self):
        redirects = {
            '/redirect': '/fixed',
            '/cross-host': f'https://127.0.0.1:{self.server.server_port}/fixed',
            '/downgrade': f'http://localhost:{self.server.server_port}/fixed',
            '/loop': '/loop',
            '/relative': 'fixed',
            '/authority': f'//localhost:{self.server.server_port}/fixed',
        }
        if self.path == '/slow-header':
            self.server.release.wait(4)
            return
        if self.path in redirects:
            self.send_response(302)
            self.send_header('Location', redirects[self.path])
            # Redirect response bytes must never contaminate the body.
            self.send_header('Content-Length', '4')
            self.end_headers()
            self.wfile.write(b'junk')
            return
        self.send_response(404 if self.path == '/missing' else 200)
        if self.path == '/long-header':
            self.send_header('X-Padding', 'x' * 80000)
        chunked = self.path in ('/chunked', '/huge-chunk', '/broken-chunk')
        if chunked:
            self.send_header('Transfer-Encoding', 'chunked')
        else:
            self.send_header('Content-Length', str(len(PAYLOAD)))
        if self.path == '/encoded':
            self.send_header('Content-Encoding', 'gzip')
        self.end_headers()
        if self.path == '/slow':
            self.server.release.wait(4)
        elif self.path == '/huge-chunk':
            self.wfile.write(b'FFFFFFFFFFFFFFF\r\n')
        elif chunked:
            # A single chunk larger than the client's read buffer.
            self.wfile.write(f'{len(PAYLOAD):x}\r\n'.encode() + PAYLOAD)
            self.wfile.write(b'XX0\r\n\r\n' if self.path == '/broken-chunk' else b'\r\n0\r\n\r\n')
        elif self.path == '/truncated':
            self.wfile.write(b'short')
        else:
            self.wfile.write(PAYLOAD)


def run(probe, *arguments, environment=None, timeout=8):
    return subprocess.run([probe, *map(str, arguments)], env=environment, capture_output=True, timeout=timeout)


def main():
    probe, openssl = sys.argv[1:]
    size = len(PAYLOAD)
    digest = hashlib.sha256(PAYLOAD).hexdigest()
    with tempfile.TemporaryDirectory(prefix='beacon-tls-') as directory:
        root = Path(directory).resolve()
        (root / 'empty').mkdir()
        subprocess.run([openssl, 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1',
                        '-keyout', str(root / 'key.pem'), '-out', str(root / 'cert.pem'),
                        '-subj', '/CN=localhost', '-addext', 'subjectAltName=DNS:localhost'],
                       capture_output=True, check=True)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(root / 'cert.pem', root / 'key.pem')
        trusted = dict(os.environ, SSL_CERT_FILE=str(root / 'cert.pem'), SSL_CERT_DIR=str(root / 'empty'))
        destination = root / 'cache' / 'artifact.zip'
        with http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler) as server:
            server.release = threading.Event()
            server.socket = context.wrap_socket(server.socket, server_side=True)
            worker = threading.Thread(target=server.serve_forever, daemon=True)
            worker.start()
            base = f'https://localhost:{server.server_port}'
            checks = [('/fixed', True), ('/chunked', True), ('/redirect', True),
                      ('/relative', True), ('/authority', True), ('/cross-host', False),
                      ('/downgrade', False), ('/loop', False), ('/huge-chunk', False),
                      ('/broken-chunk', False), ('/encoded', False), ('/truncated', False),
                      ('/missing', False), ('/long-header', False), ('/slow', False)]
            try:
                for suffix, success in checks:
                    for file_mode in (False, True):
                        destination.unlink(missing_ok=True)
                        extra = (destination, digest) if file_mode else ()
                        result = run(probe, base + suffix, size, *extra, environment=trusted)
                        assert result.returncode == (0 if success else 1), (suffix, file_mode, result.stderr)
                        if success:
                            received = destination.read_bytes() if file_mode else result.stdout
                            assert received == PAYLOAD, (suffix, file_mode, len(received), result.stderr)
                        elif file_mode:
                            assert not destination.exists(), suffix
                        assert not list(root.glob('cache/*.part')), suffix
                    print('PASS', suffix, flush=True)
                failures = {
                    'content length limit': (base + '/fixed', 4),
                    'chunked aggregate limit': (base + '/chunked', 4),
                    'wrong hostname': (base.replace('localhost', '127.0.0.1') + '/fixed', size),
                    'non HTTPS': ('http://localhost/', size),
                }
                for name, (url, limit) in failures.items():
                    assert run(probe, url, limit, environment=trusted).returncode == 1, name
                untrusted = dict(trusted, SSL_CERT_FILE=str(root / 'missing'))
                assert run(probe, base + '/fixed', size, environment=untrusted).returncode == 1, 'untrusted'
                result = run(probe, base + '/fixed', size, destination, '0' * 64, environment=trusted)
                assert result.returncode == 1 and not destination.exists(), 'wrong hash accepted'
                print('PASS limits, hostname, trust and hash verification', flush=True)
                for suffix in ('/slow-header', '/slow'):
                    start = time.monotonic()
                    result = run(probe, base + suffix, size, 'cancel', environment=trusted, timeout=2)
                    assert result.returncode == 1 and time.monotonic() - start < 2, ('cancellation', suffix)
                    print('PASS cancellation', suffix, flush=True)
            finally:
                server.release.set()
                server.shutdown()
                worker.join()

    class StallHandshake(socketserver.BaseRequestHandler):
        def handle(self):
            self.server.release.wait(3)

    with socketserver.ThreadingTCPServer(('127.0.0.1', 0), StallHandshake) as server:
        server.release = threading.Event()
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        try:
            result = run(probe, f'https://localhost:{server.server_address[1]}/', 4, 'cancel', timeout=2)
            assert result.returncode == 1, ('TLS handshake cancellation', result.stderr)
            print('PASS TLS handshake cancellation', flush=True)
        finally:
            server.release.set()
            server.shutdown()
            worker.join()


if __name__ == '__main__':
    main()
