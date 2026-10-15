# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

import http.server
import json
import sys
import tempfile
import threading
import unittest
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

from loom_serve_testing.http_harness import (
    Events,
    collect_events,
    running_server,
    wait_status,
)
from loom_serve_testing.logs import LogClient


class HttpTest(unittest.TestCase):
    def test_live_capture_keeps_connections_and_drains_interrupted_frames(self):
        requests = []

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                model = self.headers.get("X-Loom-Model")
                identity = "100:1" if model is None else "100:2"
                streaming = self.path == "/logz/stream"
                position = 0 if streaming else 1
                requests.append((self.path, model, self.headers.get("X-Loom-Cursor")))
                records = [
                    {
                        "event": "stream",
                        "scope": "server" if model is None else "model",
                        "model": model,
                        "identity": identity,
                        "head": position + 1,
                    },
                    {"event": "sample", "sequence": position},
                    {
                        "event": "cursor",
                        "value": f"{identity}:{position + 1}",
                        "next": position + 1,
                    },
                ]
                payload = b"".join(
                    json.dumps(record).encode() + b"\n" for record in records
                )
                self.send_response(200)
                if streaming:
                    self.send_header("Content-Type", "text/event-stream")
                    payload = (
                        b"event: telemetry\n"
                        + b"".join(
                            b"data: " + line + b"\n" for line in payload.splitlines()
                        )
                        + f"id: {identity}:1\n\n".encode()
                        + b"event: telemetry\ndata: {"
                    )
                else:
                    self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)
                self.wfile.flush()
                if streaming:
                    # The client must wake its blocked readers by closing the
                    # stream, not by waiting for a server heartbeat or timeout.
                    self.rfile.read(1)

            def log_message(self, *_):
                pass

        with http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler) as server:
            listener = threading.Thread(
                target=server.serve_forever, kwargs={"poll_interval": 0.01}
            )
            listener.start()
            stopped, ready = threading.Event(), threading.Event()
            records = []
            client = LogClient(f"http://127.0.0.1:{server.server_port}")
            try:
                with ThreadPoolExecutor(max_workers=1) as executor:
                    future = executor.submit(
                        collect_events,
                        client,
                        [None, "chat"],
                        stopped,
                        records.append,
                        ready=ready,
                    )
                    try:
                        ready.wait()
                        self.assertEqual(len(requests), 2)
                        self.assertEqual(len(records), 2)
                    finally:
                        stopped.set()
                    future.result()
                self.assertEqual(client.cursors, {None: "100:1:2", "chat": "100:2:2"})
                self.assertEqual(len(requests), 4)
                for model, identity in ((None, "100:1"), ("chat", "100:2")):
                    self.assertEqual(
                        [
                            (path, cursor)
                            for path, selected, cursor in requests
                            if selected == model
                        ],
                        [("/logz/stream", None), ("/logz", f"{identity}:1")],
                    )
                    self.assertEqual(
                        [
                            record["sequence"]
                            for record in records[:-1]
                            if record["model"] == model
                        ],
                        [0, 1],
                    )
                self.assertIsNone(records[-1])
            finally:
                server.shutdown()
                listener.join()

    def test_status_waits_for_model_preparation(self):
        states = [
            {"phase": "preparing", "queued_requests": 0, "active_requests": 0},
            {"phase": "idle", "queued_requests": 0, "active_requests": 0},
        ]
        responses = [
            {"data": [{"id": "chat", "loom": {"status": state}}]} for state in states
        ]
        address = SimpleNamespace(hostname="localhost", port=8080)
        with patch(
            "loom_serve_testing.http_harness.control", side_effect=responses
        ) as get:
            result = wait_status(address, model="chat", phase="idle")
        self.assertEqual(result, states[1])
        self.assertEqual(get.call_count, 2)

    def test_status_failure_reports_preparation_diagnostic_without_retry(self):
        response = {
            "data": [
                {
                    "id": "chat",
                    "loom": {
                        "registration": "failed",
                        "error": {"code": 5, "message": "missing source provider"},
                        "status": {"phase": "failed"},
                    },
                }
            ]
        }
        address = SimpleNamespace(hostname="localhost", port=8080)
        with patch(
            "loom_serve_testing.http_harness.control", return_value=response
        ) as get:
            with self.assertRaisesRegex(RuntimeError, "missing source provider"):
                wait_status(address, model="chat", phase="idle")
        get.assert_called_once_with(address, "GET", "/v1/status", 200)

    def test_child_failure_preserves_diagnostics_after_temporary_logs_are_removed(self):
        command = [
            sys.executable,
            "-c",
            "import sys; "
            'print(\'{"event":"ready","address":"127.0.0.1:1"}\', '
            "file=sys.stderr, flush=True); "
            "print('native failure evidence', file=sys.stderr, flush=True); "
            "sys.exit(7)",
        ]
        with self.assertRaises(RuntimeError) as failure:
            with tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / "server.log"
                with running_server(command, path) as (process, address):
                    self.assertEqual(address, "127.0.0.1:1")
                    self.assertEqual(process.wait(), 7)
        self.assertFalse(path.exists())
        self.assertIn("server retirement failed: 7", str(failure.exception))
        self.assertIn("native failure evidence", str(failure.exception))

    def test_names_are_scoped_and_native_session_ids_are_preserved(self):
        events = Events()
        for model, serial in (("scout", 7), ("researcher", 11)):
            events.record(
                dict(event="session_begin", name="shared", session=serial, model=model)
            )
            events.record(dict(event="admit", session=serial, model=model, request=1))
        self.assertEqual(
            events.wait("admit", "shared", model="researcher")["session"], 11
        )
        self.assertEqual(events.wait("admit", "shared", model="scout")["session"], 7)
        self.assertEqual(len(events.values), 4)
        events.record(None)
        with self.assertRaisesRegex(RuntimeError, "observer closed"):
            events.wait("request_stats", "shared", model="scout")

    def test_gaps_and_restarts_invalidate_history_even_after_a_match(self):
        for loss in (
            dict(event="gap", **{"from": 1, "to": 8}),
            dict(event="stream_reset"),
        ):
            events = Events()
            events.record(dict(event="admit", request=1))
            events.record(loss)
            with self.assertRaisesRegex(RuntimeError, "incomplete telemetry history"):
                events.wait("admit", request=1)
            with self.assertRaisesRegex(RuntimeError, "incomplete telemetry history"):
                _ = events.values

    def test_stop_drains_one_fixed_head_not_a_continuously_advancing_producer(self):
        stopped = threading.Event()
        stopped.set()
        polls = []

        class Client:
            def poll(self, model):
                polls.append(model)
                next_position = len(polls) * 64
                return SimpleNamespace(
                    stream={"scope": "model", "identity": "1:1"},
                    records=[
                        {"event": "stream", "head": next_position + 64},
                        {"event": "sample", "sequence": next_position - 1},
                        {"event": "cursor", "next": next_position},
                    ],
                )

        records = []
        collect_events(Client(), ["chat"], stopped, records.append)
        self.assertEqual(polls, ["chat", "chat"])
        self.assertEqual(records[-2]["sequence"], 127)
        self.assertIsNone(records[-1])
        self.assertEqual(records[0]["model"], "chat")
        self.assertEqual(len(records), 3)

    def test_reset_during_head_capture_cannot_wait_for_the_old_ordinal(self):
        polls = []

        class Client:
            def poll(self, model):
                polls.append(model)
                return SimpleNamespace(
                    stream={"scope": "model", "identity": f"1:{len(polls)}"},
                    records=[
                        {"event": "stream", "head": 1000},
                        {"event": "cursor", "next": 64},
                    ],
                )

        events = Events()
        stopped = threading.Event()
        stopped.set()
        with self.assertRaisesRegex(RuntimeError, "stream restarted"):
            collect_events(Client(), ["chat"], stopped, events.record)
        self.assertEqual(len(polls), 2)
        self.assertTrue(events.closed)

    def test_observation_failure_closes_waiters_and_propagates(self):
        class Client:
            def stream(self, model):
                raise OSError("reader disconnected")

        events = Events()
        with self.assertRaisesRegex(OSError, "reader disconnected"):
            collect_events(Client(), ["chat"], threading.Event(), events.record)
        with self.assertRaisesRegex(RuntimeError, "observer closed"):
            events.wait("request_stats")

    def test_http_failure_retains_diagnostics_when_child_exits_successfully(self):
        command = [
            sys.executable,
            "-c",
            "import sys; "
            'print(\'{"event":"ready","address":"127.0.0.1:1"}\', '
            "file=sys.stderr, flush=True); "
            "print('connection failure evidence', file=sys.stderr, flush=True)",
        ]
        with self.assertRaises(ConnectionResetError) as failure:
            with tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / "server.log"
                with running_server(command, path) as (process, address):
                    self.assertEqual(process.wait(), 0)
                    raise ConnectionResetError("HTTP peer reset")
        self.assertFalse(path.exists())
        self.assertEqual(str(failure.exception), "HTTP peer reset")
        notes = "\n".join(failure.exception.__notes__)
        self.assertIn("server diagnostics (exit 0)", notes)
        self.assertIn("connection failure evidence", notes)


if __name__ == "__main__":
    unittest.main()
