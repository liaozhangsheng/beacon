"""Local-only regression checks for the production HTTPS downloader."""
import http.server
import os
from pathlib import Path
import ssl
import subprocess
import sys
import tempfile
import threading
import time
import socketserver


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        if self.path == '/slow-header':
            self.server.release.wait(3)
            return
        self.send_response(200)
        if self.path in ('/chunked', '/large-chunk'):
            self.send_header('Transfer-Encoding', 'chunked')
            self.end_headers()
            body = (b'10000000\r\n' if self.path == '/large-chunk'
                    else b'3\r\nabc\r\n3\r\ndef\r\n0\r\n\r\n')
        else:
            body = b'abcdef' if self.path == '/large' else b'ok'
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
        if self.path == '/slow-body':
            self.server.release.wait(3)
            return
        self.wfile.write(body)


def main():
    probe, openssl = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix='beacon-tls-') as directory:
        root = Path(directory)
        (root / 'empty').mkdir()
        subprocess.run([
            openssl, 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1',
            '-keyout', str(root / 'key.pem'), '-out', str(root / 'cert.pem'),
            '-subj', '/CN=localhost', '-addext', 'subjectAltName=DNS:localhost',
        ], capture_output=True, check=True)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(root / 'cert.pem', root / 'key.pem')
        with http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler) as server:
            server.release = threading.Event()
            server.socket = context.wrap_socket(server.socket, server_side=True)
            worker = threading.Thread(target=server.serve_forever, daemon=True)
            worker.start()
            base = f'https://localhost:{server.server_port}'
            trusted = dict(os.environ, SSL_CERT_FILE=str(root / 'cert.pem'),
                           SSL_CERT_DIR=str(root / 'empty'))
            untrusted = dict(trusted, SSL_CERT_FILE=str(root / 'missing'))
            checks = [
                ('valid TLS', base + '/ok', 4, True, trusted),
                ('wrong hostname', base.replace('localhost', '127.0.0.1') + '/ok', 4, False, trusted),
                ('untrusted certificate', base + '/ok', 4, False, untrusted),
                ('content length limit', base + '/large', 4, False, trusted),
                ('chunked aggregate limit', base + '/chunked', 4, False, trusted),
                ('oversized chunk header', base + '/large-chunk', 4, False, trusted),
                ('valid chunked', base + '/chunked', 6, True, trusted),
                ('non HTTPS', 'http://localhost/', 4, False, trusted),
            ]
            try:
                for name, url, limit, success, environment in checks:
                    result = subprocess.run([probe, url, str(limit)], env=environment,
                                            capture_output=True, timeout=15)
                    expected_code = 0 if success else 1
                    if result.returncode != expected_code:
                        raise RuntimeError((name, result.returncode, result.stdout, result.stderr))
                    if success and result.stdout not in (b'ok', b'abcdef'):
                        raise RuntimeError((name, 'unexpected response', result.stdout))
                    print('PASS', name, flush=True)
                for suffix in ('/slow-header', '/slow-body'):
                    start = time.monotonic()
                    result = subprocess.run([probe, base + suffix, '4', 'cancel'],
                                            env=trusted, capture_output=True, timeout=2)
                    if result.returncode != 1 or time.monotonic() - start >= 2:
                        raise RuntimeError(('cancellation', suffix, result.returncode, result.stderr))
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
            result = subprocess.run([probe, f'https://localhost:{server.server_address[1]}/', '4', 'cancel'],
                                    capture_output=True, timeout=2)
            if result.returncode != 1:
                raise RuntimeError(('TLS handshake cancellation', result.returncode, result.stderr))
            print('PASS TLS handshake cancellation', flush=True)
        finally:
            server.release.set()
            server.shutdown()
            worker.join()


if __name__ == '__main__':
    main()
