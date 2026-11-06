#!/usr/bin/env python3
"""Offline transport-policy specs for the announcement endpoint qualifier."""
from __future__ import annotations

import contextlib
import io
import json
import socket
import sys
import threading
import time
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import qualify_announcement_transport as qualifier


class _QuietThreadingHTTPServer(ThreadingHTTPServer):
    daemon_threads = True

    def handle_error(self, request, client_address):
        pass


class _ScriptedHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self):
        self.server.requests.append({
            "path": self.path,
            "headers": list(self.headers.items()),
        })
        status, headers, body, delay = self.server.routes.get(
            self.path, (500, {}, b"unconfigured route", 0.0))
        if delay:
            time.sleep(delay)
        self.send_response(status)
        for name, value in headers.items():
            self.send_header(name, value)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if body:
            self.wfile.write(body)

    def log_message(self, format, *args):
        pass


class FakeEndpoint:
    def __init__(self):
        self.server = _QuietThreadingHTTPServer(
            ("127.0.0.1", 0), _ScriptedHandler)
        self.server.routes = {}
        self.server.requests = []
        self.thread = threading.Thread(
            target=self.server.serve_forever,
            kwargs={"poll_interval": 0.01}, daemon=True)

    @property
    def origin(self) -> str:
        return f"http://127.0.0.1:{self.server.server_port}"

    def route(self, path: str, status: int, *, location: str | None = None,
              body: bytes = b"", delay: float = 0.0) -> None:
        headers = {} if location is None else {"Location": location}
        self.server.routes[path] = (status, headers, body, delay)

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, exc_type, exc, traceback):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)


class AnnouncementTransportQualifierTests(unittest.TestCase):
    def setUp(self):
        self.receipt = (
            Path(__file__).resolve().parent
            / f".task5-{self._testMethodName}-receipt.json")

    def tearDown(self):
        self.receipt.unlink(missing_ok=True)

    def run_cli(self, url: str, *, logical_fetches: int = 1,
                max_redirect_hops: int = 3, deadline: float = 2.0,
                terminal_statuses: str = "200,206,404",
                receipt: Path | None = None) -> tuple[int, str]:
        arguments = [
            "--platform", "Test",
            "--url", url,
            "--signature-suffix", ".sig",
            "--logical-fetches", str(logical_fetches),
            "--max-redirect-hops", str(max_redirect_hops),
            "--shared-deadline-seconds", str(deadline),
            "--transport-only",
            "--discard-body",
            "--terminal-statuses", terminal_statuses,
            "--receipt", str(receipt or self.receipt),
            "--allow-plain-http-for-testing",
        ]
        errors = io.StringIO()
        with (contextlib.redirect_stderr(errors),
              contextlib.redirect_stdout(io.StringIO())):
            result = qualifier.main(arguments)
        return result, errors.getvalue()

    def load_receipt(self) -> dict:
        return json.loads(self.receipt.read_text(encoding="utf-8"))

    def test_hop_budget_is_enforced(self):
        with FakeEndpoint() as endpoint:
            for index in range(4):
                endpoint.route(
                    f"/hop-{index}", 302,
                    location=f"{endpoint.origin}/hop-{index + 1}")
            endpoint.route("/hop-4", 200)
            result, errors = self.run_cli(
                f"{endpoint.origin}/hop-0", max_redirect_hops=2)

        self.assertNotEqual(result, 0)
        self.assertIn("redirect hop budget", errors)
        receipt = self.load_receipt()
        self.assertEqual(receipt["qualified"], "unqualified")
        self.assertEqual([hop["status"] for hop in receipt["hops"]],
                         [302, 302, 302])

    def test_http_downgrade_is_refused(self):
        self.assertFalse(qualifier.redirect_allowed(
            "http://release-assets.githubusercontent.com/object",
            hop_index=1, max_redirect_hops=3))

    def test_redirect_judge_refuses_suffix_confusion(self):
        self.assertFalse(qualifier.redirect_allowed(
            "https://evil-release-assets.githubusercontent.com.evil.com/object",
            hop_index=1, max_redirect_hops=3))

    def test_redirect_judge_refuses_userinfo(self):
        self.assertFalse(qualifier.redirect_allowed(
            "https://user@release-assets.githubusercontent.com/object",
            hop_index=1, max_redirect_hops=3))

    def test_redirect_judge_refuses_explicit_port(self):
        self.assertFalse(qualifier.redirect_allowed(
            "https://release-assets.githubusercontent.com:443/object",
            hop_index=1, max_redirect_hops=3))

    def test_redirect_judge_refuses_unlisted_host(self):
        self.assertFalse(qualifier.redirect_allowed(
            "https://github.com/object",
            hop_index=1, max_redirect_hops=3))

    def test_redirect_judge_allows_only_the_three_literal_cdn_hosts(self):
        for host in (
                "release-assets.githubusercontent.com",
                "objects.githubusercontent.com",
                "github-releases.githubusercontent.com"):
            with self.subTest(host=host):
                self.assertTrue(qualifier.redirect_allowed(
                    f"https://{host}/object",
                    hop_index=1, max_redirect_hops=3))

    LATEST = ("https://github.com/aleiby/wot/releases/latest/download/"
              "WoT-Win64-announcement-v1.json")
    TAG = ("https://github.com/aleiby/wot/releases/download/v1.2.0/"
           "WoT-Win64-announcement-v1.json")

    def test_latest_to_tag_hop_is_followed_at_hop_one(self):
        # Mirrors WoT.Update.TransportPolicy.PermittedHops.LatestToTag.
        self.assertTrue(qualifier.redirect_allowed(
            self.TAG, hop_index=1, max_redirect_hops=3,
            previous_url=self.LATEST))

    def test_latest_to_tag_hop_needs_the_previous_url_and_hop_one(self):
        self.assertFalse(qualifier.redirect_allowed(
            self.TAG, hop_index=1, max_redirect_hops=3))
        self.assertFalse(qualifier.redirect_allowed(
            self.TAG, hop_index=2, max_redirect_hops=3,
            previous_url=self.LATEST))
        self.assertFalse(qualifier.redirect_allowed(
            self.TAG, hop_index=1, max_redirect_hops=3,
            previous_url=self.TAG))

    def test_latest_hop_refusals_mirror_the_native_policy(self):
        # Mirrors WoT.Update.TransportPolicy.Refuse.LatestHop.
        for location in (
                "https://github.com/attacker/wot/releases/download/v1/WoT-Win64-announcement-v1.json",
                "https://github.com/aleiby/evil/releases/download/v1/WoT-Win64-announcement-v1.json",
                "https://github.com/aleiby/wot/releases/download/v1/WoT-Mac-announcement-v1.json",
                "https://github.com/aleiby/wot/releases/tag/v1",
                "https://github.com/aleiby/wot/releases/download/latest/WoT-Win64-announcement-v1.json",
                "https://github.com/aleiby/wot/releases/download/v1/WoT-Win64-announcement-v1.json?x=1",
                "https://github.com/aleiby/wot/releases/download/v1%2F..%2F/WoT-Win64-announcement-v1.json",
                "https://github.com/aleiby/wot/releases/download/../WoT-Win64-announcement-v1.json",
                "https://github.com/aleiby/wot/releases/download/v1/x/WoT-Win64-announcement-v1.json",
                "https://github.com/aleiby/wot/Releases/download/v1/WoT-Win64-announcement-v1.json",
                "https://api.github.com/aleiby/wot/releases/download/v1/WoT-Win64-announcement-v1.json",
                "http://github.com/aleiby/wot/releases/download/v1/WoT-Win64-announcement-v1.json",
                "https://github.com:443/aleiby/wot/releases/download/v1/WoT-Win64-announcement-v1.json",
                "https://github.com./aleiby/wot/releases/download/v1/WoT-Win64-announcement-v1.json",
                "https://github.com/aleiby/wot/releases/download/v1.2.0/WoT-Win64-announcement-v1.json#"):
            with self.subTest(location=location):
                self.assertFalse(qualifier.redirect_allowed(
                    location, hop_index=1, max_redirect_hops=3,
                    previous_url=self.LATEST))

    def _scripted_fetch(self, script):
        """Drive the real _fetch loop over scripted (status, location) hops."""
        requested = []

        def one_hop(url, deadline, plain_http_testing):
            requested.append(url)
            status, location = script[len(requested) - 1]
            return status, location, None

        hops, evidence = [], {}
        with mock.patch.object(qualifier, "_one_hop", side_effect=one_hop):
            try:
                result = qualifier._fetch(
                    self.LATEST, deadline=time.monotonic() + 30,
                    max_redirect_hops=3, plain_http_testing=False,
                    hops=hops, evidence=evidence)
            except qualifier.PolicyRejected as error:
                return requested, None, str(error)
        return requested, result, None

    def test_fetch_follows_latest_to_tag_to_cdn(self):
        cdn = "https://release-assets.githubusercontent.com/asset?sig=x"
        requested, result, error = self._scripted_fetch(
            [(302, self.TAG), (302, cdn), (200, None)])
        self.assertIsNone(error)
        self.assertEqual(requested, [self.LATEST, self.TAG, cdn])
        self.assertEqual(result.redirect_count, 2)
        self.assertEqual(result.terminal_status, 200)

    def test_latest_hop_counts_against_the_three_hop_budget(self):
        cdn1 = "https://release-assets.githubusercontent.com/one"
        cdn2 = "https://objects.githubusercontent.com/two"
        cdn3 = "https://github-releases.githubusercontent.com/three"
        requested, result, error = self._scripted_fetch(
            [(302, self.TAG), (302, cdn1), (302, cdn2), (302, cdn3),
             (200, None)])
        self.assertIsNone(result)
        self.assertIn("redirect hop budget", error)
        self.assertEqual(requested, [self.LATEST, self.TAG, cdn1, cdn2])

    def test_invalid_initial_origin_writes_an_unqualified_receipt(self):
        result, errors = self.run_cli(
            "https://not-github.example/announcement")

        self.assertNotEqual(result, 0)
        self.assertIn("initial URL", errors)
        receipt = self.load_receipt()
        self.assertEqual(receipt["hops"], [])
        self.assertEqual(receipt["qualified"], "unqualified")

    def test_rejected_observed_tls_hop_keeps_its_peer_identity(self):
        identity = {
            "subject": [[["commonName", "github.com"]]],
            "issuer": [[["commonName", "Test CA"]]],
            "notAfter": "Jan 01 00:00:00 2030 GMT",
        }
        parser = qualifier.build_parser()
        args = parser.parse_args([
            "--platform", "Test",
            "--url", "https://github.com/release/announcement",
            "--signature-suffix", ".sig",
            "--logical-fetches", "1",
            "--max-redirect-hops", "3",
            "--shared-deadline-seconds", "2",
            "--transport-only",
            "--discard-body",
            "--terminal-statuses", "200,206,404",
            "--receipt", str(self.receipt),
        ])
        with mock.patch.object(
                qualifier, "_one_hop",
                return_value=(
                    302, "https://unlisted.example/object", identity)):
            receipt, error = qualifier.qualify(args)

        self.assertIsNotNone(error)
        self.assertEqual(receipt["qualified"], "unqualified")
        self.assertEqual(receipt["tls_identity"], identity)

    def test_206_is_accepted_after_an_allowed_redirect(self):
        with FakeEndpoint() as endpoint:
            endpoint.route(
                "/announcement", 302,
                location=f"{endpoint.origin}/partial")
            endpoint.route("/partial", 206, body=b"ignored candidate bytes")
            result, errors = self.run_cli(
                f"{endpoint.origin}/announcement",
                terminal_statuses="206")

        self.assertEqual(result, 0, errors)
        receipt = self.load_receipt()
        self.assertEqual(receipt["terminal_status"], 206)
        self.assertEqual(receipt["qualified"], "full-cdn-path")

    def test_404_is_a_direct_path_only_qualification(self):
        with FakeEndpoint() as endpoint:
            endpoint.route("/missing", 404, body=b"not parsed")
            result, errors = self.run_cli(f"{endpoint.origin}/missing")

        self.assertEqual(result, 0, errors)
        receipt = self.load_receipt()
        self.assertEqual(receipt["terminal_status"], 404)
        self.assertEqual(receipt["qualified"], "direct-path-only")

    def test_500_is_unqualified_and_nonzero(self):
        with FakeEndpoint() as endpoint:
            endpoint.route("/failure", 500)
            result, errors = self.run_cli(f"{endpoint.origin}/failure")

        self.assertNotEqual(result, 0)
        self.assertIn("terminal status 500", errors)
        self.assertEqual(self.load_receipt()["qualified"], "unqualified")

    def test_requests_never_carry_credentials_or_cookies(self):
        with FakeEndpoint() as endpoint:
            endpoint.route("/announcement", 404)
            endpoint.route("/announcement.sig", 404)
            result, errors = self.run_cli(
                f"{endpoint.origin}/announcement", logical_fetches=2)

        self.assertEqual(result, 0, errors)
        self.assertEqual(
            [request["path"] for request in endpoint.server.requests],
            ["/announcement", "/announcement.sig"])
        for request in endpoint.server.requests:
            names = {name.lower() for name, _ in request["headers"]}
            self.assertTrue(
                names.isdisjoint({
                    "authorization", "proxy-authorization", "cookie",
                }),
                request["headers"])

    def test_one_deadline_is_shared_by_both_logical_fetches(self):
        with FakeEndpoint() as endpoint:
            endpoint.route("/slow", 200, delay=0.07)
            endpoint.route("/slow.sig", 200, delay=0.07)
            result, errors = self.run_cli(
                f"{endpoint.origin}/slow", logical_fetches=2,
                deadline=0.10)

        self.assertNotEqual(result, 0)
        self.assertIn("deadline", errors)
        self.assertFalse(self.receipt.exists())

    def test_deadline_consumption_is_recorded(self):
        with FakeEndpoint() as endpoint:
            endpoint.route("/timed", 404, delay=0.02)
            result, errors = self.run_cli(f"{endpoint.origin}/timed")

        self.assertEqual(result, 0, errors)
        consumed = self.load_receipt()["deadline_consumed_seconds"]
        self.assertIsInstance(consumed, float)
        # The 20ms fake delay proves consumption is recorded, but Windows sleep
        # and clock quantization can undershoot the nominal delay by a hair
        # (observed: 0.014999999999 against a 0.015 bound). Assert with a
        # granularity margin; the upper bound still pins the deadline domain.
        self.assertGreaterEqual(consumed, 0.010)
        self.assertLess(consumed, 2.0)

    def test_two_logical_fetches_use_url_then_url_plus_signature_suffix(self):
        with FakeEndpoint() as endpoint:
            endpoint.route(
                "/announcement", 302,
                location=f"{endpoint.origin}/object")
            endpoint.route("/object", 200)
            endpoint.route(
                "/announcement.sig", 302,
                location=f"{endpoint.origin}/object.sig")
            endpoint.route("/object.sig", 206)
            result, errors = self.run_cli(
                f"{endpoint.origin}/announcement", logical_fetches=2)

        self.assertEqual(result, 0, errors)
        self.assertEqual(
            [request["path"] for request in endpoint.server.requests],
            ["/announcement", "/object",
             "/announcement.sig", "/object.sig"])
        self.assertEqual(self.load_receipt()["qualified"], "full-cdn-path")

    def test_receipt_has_exact_schema_and_canonical_json_format(self):
        with FakeEndpoint() as endpoint:
            endpoint.route("/missing", 404)
            url = f"{endpoint.origin}/missing"
            result, errors = self.run_cli(url)

        self.assertEqual(result, 0, errors)
        receipt = self.load_receipt()
        self.assertEqual(set(receipt), {
            "platform",
            "url",
            "hops",
            "terminal_status",
            "tls_identity",
            "deadline_consumed_seconds",
            "qualified",
            "plain_http_testing",
        })
        self.assertEqual(set(receipt["hops"][0]), {"url", "status", "host"})
        self.assertEqual(receipt["platform"], "Test")
        self.assertEqual(receipt["url"], url)
        self.assertEqual(receipt["tls_identity"], None)
        self.assertTrue(receipt["plain_http_testing"])
        raw = self.receipt.read_text(encoding="utf-8")
        self.assertTrue(raw.endswith("\n"))
        self.assertEqual(raw, json.dumps(
            receipt, indent=2, sort_keys=True) + "\n")

    def test_connection_refused_is_nonzero_and_writes_no_receipt(self):
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            unused_port = probe.getsockname()[1]
        result, errors = self.run_cli(
            f"http://127.0.0.1:{unused_port}/offline", deadline=0.10)

        self.assertNotEqual(result, 0)
        self.assertIn("unavailable", errors.lower())
        self.assertFalse(self.receipt.exists())

    def test_help_documents_that_offline_failures_write_no_receipt(self):
        help_text = qualifier.build_parser().format_help().lower()
        self.assertIn("offline", help_text)
        self.assertIn("no receipt", help_text)


if __name__ == "__main__":
    unittest.main()
