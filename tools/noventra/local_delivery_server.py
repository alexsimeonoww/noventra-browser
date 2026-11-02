#!/usr/bin/env python3
"""Read-only loopback server for one explicitly published local release directory."""

import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import ipaddress
from pathlib import Path
import urllib.parse

import release_manifest as release


class DeliveryHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        self.serve(False)

    def do_HEAD(self):
        self.serve(True)

    def log_message(self, *args):
        pass  # No request data or credentials in logs.

    def serve(self, head):
        source = None
        try:
            root = self.server.release_root
            request = urllib.parse.urlsplit(self.path)
            release.require(not request.query and not request.fragment, 'Unsupported request')
            name = request.path.removeprefix('/')
            release.filename(name)  # No decoding, traversal or nested paths.
            if name not in {'manifest.json', 'manifest.sig', 'distribution.json'}:
                manifest = release.parse(release.read(root / 'manifest.json', release.MANIFEST_LIMIT))
                artifact = manifest.get('artifact')
                release.require(isinstance(artifact, dict), 'Missing release artifact')
                release.require(name == artifact.get('filename') and name.endswith('.tar.xz'),
                                'Not a release artifact')
            source = release.open_regular(root / name)
            size = source.seek(0, 2)
            source.seek(0)
        except (ValueError, OSError):
            if source is not None:
                source.close()
            self.send_error(404)
            return
        with source:
            self.send_response(200)
            self.send_header('Content-Type', 'application/octet-stream')
            self.send_header('Content-Length', str(size))
            self.send_header('Cache-Control', 'no-store')
            self.send_header('X-Content-Type-Options', 'nosniff')
            self.end_headers()
            if not head:
                try:
                    while data := source.read(1024 * 1024):
                        self.wfile.write(data)
                except (BrokenPipeError, ConnectionResetError):
                    pass


def make_server(root, host='127.0.0.1', port=0):
    release.require(ipaddress.ip_address(host).is_loopback and ':' not in host,
                    'This local server binds IPv4 loopback only; use an owned HTTPS server for remote clients')
    root = Path(root).absolute()
    release.read(root / 'manifest.json', release.MANIFEST_LIMIT)
    server = ThreadingHTTPServer((host, port), DeliveryHandler)
    server.daemon_threads = True
    server.release_root = root
    return server


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=0)
    args = parser.parse_args()
    try:
        with make_server(args.root, args.host, args.port) as server:
            print(f'Local release URL: http://{server.server_address[0]}:{server.server_address[1]}/', flush=True)
            try:
                server.serve_forever()
            except KeyboardInterrupt:
                pass
    except (ValueError, OSError) as error:
        parser.exit(1, f'Local delivery server: {error}\n')


if __name__ == '__main__':
    main()
