#!/usr/bin/env python3
"""Check a running UMT HTTP service without starting probe/dump/library jobs.

python tests/http_integration.py --base http://127.0.0.1:18765
Only reads and rejected requests are sent. No ADB or dependencies are required.
"""
from __future__ import annotations

import argparse
import http.client
import json
import socket
import struct
import sys
import unittest
from urllib.parse import quote, urlsplit

BASE = "http://127.0.0.1:18765"


def request(method="GET", target="/api/health", headers=None, body=None):
    parsed = urlsplit(BASE)
    connection = http.client.HTTPConnection(parsed.hostname, parsed.port or 80, timeout=15)
    try:
        connection.request(method, target, body=body, headers=headers or {})
        response = connection.getresponse()
        return response.status, dict(response.getheaders()), response.read()
    finally:
        connection.close()


def raw_request(data: bytes):
    parsed = urlsplit(BASE)
    with socket.create_connection((parsed.hostname, parsed.port or 80), timeout=15) as stream:
        stream.sendall(data)
        response = http.client.HTTPResponse(stream)
        response.begin()
        return response.status, dict(response.getheaders()), response.read()


class HttpIntegration(unittest.TestCase):
    def json_response(self, path):
        code, headers, body = request(target=path)
        self.assertEqual(code, 200, body)
        self.assertIn("application/json", headers.get("Content-Type", ""))
        self.assertEqual(int(headers["Content-Length"]), len(body))
        self.assertEqual(headers.get("X-Content-Type-Options"), "nosniff")
        self.assertNotIn("Access-Control-Allow-Origin", headers)
        return json.loads(body)

    def rejection(self, code, method="GET", target="/api/health", headers=None, body=None):
        actual, _, response = request(method, target, headers, body)
        self.assertEqual(actual, code, response)
        self.assertIsInstance(json.loads(response).get("error"), str)

    def test_health_schema(self):
        health = self.json_response("/api/health")
        self.assertIsInstance(health["version"], str)
        self.assertIsInstance(health["uid"], int)
        self.assertGreater(health["pid"], 0)

    def test_state_schema_and_progress(self):
        state = self.json_response("/api/state")
        for name in ("busy", "probed"):
            self.assertIsInstance(state[name], bool, name)
        for name in ("phase", "error", "package", "profile", "resultPath", "soDumpPath"):
            self.assertIsInstance(state[name], str, name)
        self.assertIsInstance(state["pid"], int)
        for name in ("objectsPercent", "dumpPercent"):
            self.assertIsInstance(state[name], (int, float))
            self.assertGreaterEqual(state[name], 0)
            self.assertLessEqual(state[name], 100)
        for name in ("offsets", "groups", "logs"):
            self.assertIsInstance(state[name], list)
        self.assertTrue(all(isinstance(line, str) for line in state["logs"]))
        self.assertIsInstance(state["metrics"], dict)
        for offset in state["offsets"]:
            for name in ("name", "value", "relative"):
                self.assertIsInstance(offset[name], str)
            self.assertIsInstance(offset["found"], bool)
        for group in state["groups"]:
            self.assertIsInstance(group["name"], str)
            self.assertIsInstance(group["fields"], list)
            for field in group["fields"]:
                for name in ("name", "type", "offset", "description"):
                    self.assertIsInstance(field[name], str)
                self.assertIsInstance(field["found"], bool)

    def test_process_schema(self):
        processes = self.json_response("/api/processes")
        self.assertIsInstance(processes, list)
        for process in processes:
            self.assertGreater(process["pid"], 0)
            self.assertIsInstance(process["package"], str)
            self.assertIsInstance(process["profileName"], str)
            self.assertIsInstance(process["dedicated"], bool)

    def test_file_schema_and_same_origin_links(self):
        files = self.json_response("/api/files")
        self.assertIsInstance(files, list)
        for file in files:
            self.assertIsInstance(file["name"], str)
            self.assertGreaterEqual(file["size"], 0)
            self.assertIsInstance(file["url"], str)
            self.assertTrue(file["url"].startswith("/api/file?name="), file)
            self.assertFalse(urlsplit(file["url"]).netloc)

    def test_browser_assets_and_content_types(self):
        for path, content_type, needle in (("/", "text/html", b"/app.js"), ("/style.css", "text/css", b"--accent"), ("/app.js", "text/javascript", b"X-UMT-Client")):
            with self.subTest(path=path):
                code, headers, body = request(target=path)
                self.assertEqual(code, 200, body)
                self.assertIn(content_type, headers.get("Content-Type", ""))
                self.assertEqual(int(headers["Content-Length"]), len(body))
                self.assertIn(needle, body)
                self.assertIn("frame-ancestors 'none'", headers.get("Content-Security-Policy", ""))

    def test_host_rebinding_and_malformed_ports_rejected(self):
        for host in ("rebinding.example:18765", "127.0.0.1.attacker.invalid", "localhost:0", "localhost:65536", "localhost:", "localhost:abc", "[::1]:0"):
            with self.subTest(host=host):
                self.rejection(403, headers={"Host": host})

    def test_loopback_host_allowlist_accepts_forwarded_ports(self):
        for host in ("localhost:49152", "127.0.0.1:18766", "[::1]:18765"):
            code, _, body = request(headers={"Host": host})
            self.assertEqual(code, 200, body)

    def test_cross_origin_and_null_origin_rejected(self):
        for origin in ("https://malicious.invalid", "null", "http://localhost:1"):
            with self.subTest(origin=origin):
                self.rejection(403, "POST", "/api/action", {"Content-Type": "application/json", "X-UMT-Client": "1", "Origin": origin}, b'{"action":"__umt_http_test_invalid__"}')

    def test_custom_header_and_json_required(self):
        for headers in ({"Content-Type": "application/json"}, {"Content-Type": "text/plain", "X-UMT-Client": "1"}, {"Content-Type": "application/jsonevil", "X-UMT-Client": "1"}):
            with self.subTest(headers=headers):
                self.rejection(403, "POST", "/api/action", headers, b"{}")

    def test_unknown_action_rejected_without_starting_job(self):
        before = self.json_response("/api/state")
        authority = urlsplit(BASE).netloc
        code, _, body = request("POST", "/api/action", {"Content-Type": "application/json; charset=utf-8", "X-UMT-Client": "1", "Origin": f"http://{authority}"}, b'{"action":"__umt_http_test_invalid__","pid":0}')
        self.assertEqual(code, 409, body)
        answer = json.loads(body)
        self.assertFalse(answer.get("accepted", False))
        # If another client was already working, the invalid action must still
        # be rejected. Do not assert the task stays busy: it can finish normally.
        if not before["busy"]:
            after = self.json_response("/api/state")
            self.assertFalse(after["busy"], "An invalid action must not start a worker")
            self.assertEqual(after["pid"], before["pid"])

    def test_invalid_json_and_encoded_names_rejected(self):
        self.rejection(400, "POST", "/api/action", {"Content-Type": "application/json", "X-UMT-Client": "1"}, b"{")
        for name in ("%", "%2", "%GG", "%00"):
            with self.subTest(name=name):
                self.rejection(400, target="/api/file?name=" + name)
        for name in ("../service.log", "/proc/self/maps", "../../etc/passwd"):
            with self.subTest(name=name):
                self.rejection(404, target="/api/file?name=" + quote(name, safe=""))

    def test_duplicate_headers_and_transfer_encoding_rejected(self):
        authority = urlsplit(BASE).netloc.encode()
        requests = (
            b"GET /api/health HTTP/1.1\r\nHost: " + authority + b"\r\nHost: localhost\r\n\r\n",
            b"GET /api/health HTTP/1.1\r\nHost: " + authority + b"\r\nTransfer-Encoding: chunked\r\n\r\n",
            b"GET /api/health HTTP/9.9\r\nHost: " + authority + b"\r\n\r\n",
        )
        for payload in requests:
            code, _, body = raw_request(payload)
            self.assertEqual(code, 400, body)

    def test_oversized_body_and_headers_rejected(self):
        authority = urlsplit(BASE).netloc.encode()
        code, _, body = raw_request(b"POST /api/action HTTP/1.1\r\nHost: " + authority + b"\r\nContent-Type: application/json\r\nX-UMT-Client: 1\r\nContent-Length: 65537\r\n\r\n")
        self.assertEqual(code, 413, body)
        code, _, body = raw_request(b"GET /api/health HTTP/1.1\r\nHost: " + authority + b"\r\nX-Large: " + b"a" * 17000 + b"\r\n\r\n")
        self.assertEqual(code, 431, body)

    def test_unknown_paths_and_methods(self):
        self.rejection(404, target="/not-a-real-asset")
        self.rejection(405, "DELETE")
        self.rejection(405, "OPTIONS", "/api/action")

    def test_cancelled_download_keeps_service_alive(self):
        files = self.json_response("/api/files")
        candidates = [item for item in files if item["size"] >= 1024 * 1024]
        if not candidates:
            self.skipTest("No existing >= 1 MB export available to verify interrupted sendfile")
        parsed = urlsplit(BASE)
        stream = socket.create_connection((parsed.hostname, parsed.port or 80), timeout=15)
        try:
            payload = f"GET {candidates[0]['url']} HTTP/1.1\r\nHost: {parsed.netloc}\r\nConnection: close\r\n\r\n".encode()
            stream.sendall(payload)
            self.assertIn(b"200", stream.recv(200).split(b"\r\n", 1)[0])
            # Reset the connection instead of reading a large dump into Python.
            stream.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("hh" if sys.platform == "win32" else "ii", 1, 0))
        finally:
            stream.close()
        self.json_response("/api/health")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", default=BASE, help="Forwarded loopback UMT URL")
    args, remaining = parser.parse_known_args()
    parsed = urlsplit(args.base)
    if parsed.scheme != "http" or parsed.hostname not in ("localhost", "127.0.0.1", "::1") or parsed.path not in ("", "/"):
        parser.error("--base must be an HTTP loopback URL without a path")
    BASE = args.base.rstrip("/")
    unittest.main(argv=[sys.argv[0], "-v", *remaining])
