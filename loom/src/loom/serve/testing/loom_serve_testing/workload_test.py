# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

import contextlib
import http.server
import io
import json
import tempfile
import threading
import unittest
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from loom_serve_testing import replay, workload


def plan(*records):
    return workload.parse(json.dumps(record) for record in records)


def request(identity, *, path="/v1/embeddings", model="vectors", **fields):
    return {
        "id": identity,
        "path": path,
        "body": {"model": model, "input": identity},
        "at_ms": 0,
        **fields,
    }


@contextlib.contextmanager
def endpoint(respond):
    """A real HTTP dependency whose response ordering is controlled by events."""
    observed = []
    failures = []

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_POST(self):
            body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            observed.append((self.path, dict(self.headers), body))
            try:
                status, content_type, payload = respond(self.path, body)
            except BaseException as error:
                failures.append(error)
                status, content_type, payload = 500, "application/json", b"{}"
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)

        def log_message(self, *_):
            pass

    with http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler) as server:
        thread = threading.Thread(
            target=server.serve_forever, kwargs={"poll_interval": 0.01}
        )
        thread.start()
        try:
            yield workload.origin(f"http://127.0.0.1:{server.server_port}"), observed
        finally:
            server.shutdown()
            thread.join()
        if failures:
            raise failures[0]


def vector_result(body):
    return (
        200,
        "application/json",
        json.dumps(
            {
                "model": body["model"],
                "data": [{"embedding": [1, 2]}],
                "usage": {"prompt_tokens": 5, "total_tokens": 5},
            }
        ).encode(),
    )


def chat_result(body, *, done=True):
    events = [
        {
            "model": body["model"],
            "choices": [{"index": 0, "delta": {"reasoning_content": "consider"}}],
        },
        {
            "model": body["model"],
            "choices": [
                {"index": 0, "delta": {"content": "answer"}, "finish_reason": "stop"}
            ],
            "usage": {
                "prompt_tokens": 9,
                "completion_tokens": 7,
                "prompt_tokens_details": {"cached_tokens": 3},
            },
        },
    ]
    payload = b"".join(
        b"data: " + json.dumps(event).encode() + b"\n\n" for event in events
    )
    return 200, "text/event-stream", payload + (b"data: [DONE]\n\n" if done else b"")


class WorkloadTest(unittest.TestCase):
    def test_preflight_rejects_invalid_plan_before_any_io(self):
        valid = request("one")
        invalid = [
            {},
            {**valid, "id": ""},
            {**valid, "typo": 0},
            {**valid, "path": "/v1/models/unregister"},
            {**valid, "body": {}},
            {**valid, "at_ms": -1},
            {**valid, "at_ms": True},
            {**valid, "at_ms": float("nan")},
            {**valid, "after": "one"},
            {**valid, "delay_ms": 1},
            {**valid, "headers": {"Content-Length": "0"}},
            {**valid, "headers": {"X-Loom-Session": "a\nb"}},
            {**valid, "headers": {"X-A": "a", "x-a": "b"}},
            {**valid, "body": {"model": "x", "stream": True}},
            {**valid, "body": {"model": "x", "temperature": float("inf")}},
            {**valid, "path": "/v1/chat/completions"},
            {
                **valid,
                "path": "/v1/chat/completions",
                "body": {"model": "x", "stream": True, "stream_options": []},
            },
        ]
        for record in invalid:
            with self.subTest(record=record), self.assertRaises(ValueError):
                plan(record)
        with self.assertRaisesRegex(ValueError, "unique"):
            plan(valid, valid)
        with self.assertRaisesRegex(ValueError, "earlier"):
            plan(
                {
                    "id": "child",
                    "path": valid["path"],
                    "body": valid["body"],
                    "after": "later",
                }
            )
        with self.assertRaises(ValueError):
            workload.parse([])
        for value in (
            "file:///tmp/socket",
            "http://host/model",
            "http://user:pass@host",
            "http://host?ignored=1",
            "http://host:99999",
        ):
            with self.subTest(value=value), self.assertRaises(ValueError):
                workload.origin(value)

    def test_frozen_bodies_parallel_modalities_and_completion_dependencies(self):
        text_received = threading.Event()
        vector_received = threading.Event()
        release = threading.Event()
        chat_body = {
            "model": "text",
            "stream": True,
            "stream_options": {"include_usage": True},
            "top_p": 0.82,
            "messages": [{"role": "user", "content": "frozen"}],
        }
        requests = plan(
            request(
                "text",
                path="/v1/chat/completions",
                body=chat_body,
                headers={"X-Loom-Session": "agent-a"},
            ),
            request("vector"),
            {
                "id": "follow",
                "after": "text",
                "delay_ms": 2,
                "path": "/v1/chat/completions",
                "body": {**chat_body, "seed": 7},
            },
        )

        def respond(path, body):
            if path == "/v1/chat/completions":
                text_received.set()
                release.wait()
                return chat_result(body)
            vector_received.set()
            release.wait()
            return vector_result(body)

        records = []
        with (
            endpoint(respond) as (address, observed),
            ThreadPoolExecutor(max_workers=1) as runner,
        ):
            future = runner.submit(
                workload.replay, address, requests, 2, records.append
            )
            try:
                text_received.wait()
                vector_received.wait()
                self.assertEqual(len(observed), 2)
            finally:
                release.set()
            summary = future.result()
        self.assertEqual(len(records), 3)
        by_id = {record["id"]: record for record in records}
        first, follow = by_id["text"], by_id["follow"]
        self.assertAlmostEqual(follow["scheduled_ms"], first["completed_ms"] + 2)
        self.assertGreaterEqual(follow["started_ms"], follow["scheduled_ms"])
        self.assertLessEqual(first["first_output_ms"], first["first_text_ms"])
        self.assertLessEqual(first["first_text_ms"], first["request_ms"])
        self.assertEqual(len(first["output_chunk_ms"]), 2)
        self.assertEqual(first["usage"]["completion_tokens"], 7)
        self.assertEqual(
            observed[0 if observed[0][0].endswith("completions") else 1][2], chat_body
        )
        self.assertEqual(observed[-1][2], {**chat_body, "seed": 7})
        classes = {group["model"]: group for group in summary["classes"]}
        self.assertEqual(classes["text"]["completion_tokens"]["sum"], 14)
        self.assertEqual(classes["text"]["cached_prompt_tokens"]["sum"], 6)
        self.assertIsNone(classes["vectors"]["completion_tokens"]["sum"])
        self.assertIsNone(classes["vectors"]["first_output_ms"])

    def test_concurrency_cap_exposes_launch_lag(self):
        received = threading.Event()
        release = threading.Event()
        records = []

        def respond(path, body):
            if body["input"] == "one":
                received.set()
                release.wait()
            return vector_result(body)

        with (
            endpoint(respond) as (address, observed),
            ThreadPoolExecutor(max_workers=1) as runner,
        ):
            future = runner.submit(
                workload.replay,
                address,
                plan(request("one"), request("two")),
                1,
                records.append,
            )
            try:
                received.wait()
                self.assertEqual(len(observed), 1)
            finally:
                release.set()
            future.result()
        first, second = records
        self.assertGreaterEqual(second["started_ms"], first["completed_ms"])
        self.assertEqual(second["client_lag_ms"], second["started_ms"])
        self.assertAlmostEqual(
            second["arrival_to_completion_ms"],
            second["client_lag_ms"] + second["request_ms"],
        )

    def test_failed_request_blocks_descendants_but_not_other_models(self):
        requests = plan(
            request("bad"),
            {
                "id": "child",
                "after": "bad",
                "path": "/v1/embeddings",
                "body": {"model": "vectors"},
            },
            {
                "id": "grandchild",
                "after": "child",
                "path": "/v1/embeddings",
                "body": {"model": "vectors"},
            },
            request("good", model="peer"),
        )

        def respond(path, body):
            if body["input"] == "bad":
                return 503, "application/json", b'{"error":"overloaded"}'
            return vector_result(body)

        records = []
        with endpoint(respond) as (address, observed):
            summary = workload.replay(address, requests, 2, records.append)
        self.assertEqual(len(observed), 2)
        outcomes = {record["id"]: record["outcome"] for record in records}
        self.assertEqual(
            outcomes,
            {
                "bad": "failed",
                "child": "blocked",
                "grandchild": "blocked",
                "good": "completed",
            },
        )
        classes = {group["model"]: group for group in summary["classes"]}
        self.assertEqual(classes["vectors"]["client_lag_ms"]["count"], 1)
        self.assertIsNone(classes["vectors"]["request_ms"])
        self.assertEqual(classes["peer"]["completed"], 1)

    def test_cli_preserves_partial_evidence_and_fails_incomplete_stream(self):
        body = {
            "model": "text",
            "stream": True,
            "stream_options": {"include_usage": True},
        }
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "requests.jsonl"
            source.write_text(
                json.dumps(request("one", path="/v1/chat/completions", body=body))
            )
            output = io.StringIO()
            with endpoint(lambda path, body: chat_result(body, done=False)) as (
                address,
                _,
            ):
                with contextlib.redirect_stdout(output):
                    status = replay.main(
                        [
                            "--server",
                            address.geturl(),
                            "--workload",
                            str(source),
                            "--concurrency",
                            "1",
                        ]
                    )
            self.assertEqual(status, 1)
            records = [json.loads(line) for line in output.getvalue().splitlines()]
            self.assertEqual(
                [record["event"] for record in records],
                ["workload", "request", "summary"],
            )
            self.assertIn("Incomplete successful SSE", records[1]["error"])
            self.assertEqual(records[2]["classes"][0]["failed"], 1)

    def test_independent_arrival_and_finite_protocol_failures(self):
        for response, diagnostic in (
            ({"error": "device failed"}, "device failed"),
            ({"model": "wrong"}, "wrong model"),
            ({"usage": {"completion_tokens": -1}}, "completion_tokens"),
            ({"usage": {"prompt_tokens_details": []}}, "prompt_tokens_details"),
            (
                {"usage": {"prompt_tokens_details": {"cached_tokens": "3"}}},
                "cached_tokens",
            ),
            (
                {
                    "usage": {
                        "prompt_tokens": 2,
                        "prompt_tokens_details": {"cached_tokens": 3},
                    }
                },
                "cached_tokens",
            ),
        ):
            with self.subTest(response=response):
                records = []
                with endpoint(
                    lambda path, body: (
                        200,
                        "application/json",
                        json.dumps(response).encode(),
                    )
                ) as (address, _):
                    summary = workload.replay(
                        address, plan(request("one", at_ms=1)), 1, records.append
                    )
                self.assertEqual(records[0]["scheduled_ms"], 1)
                self.assertGreaterEqual(records[0]["started_ms"], 1)
                self.assertIn(diagnostic, records[0]["error"])
                self.assertEqual(summary["classes"][0]["failed"], 1)

    def test_summary_keeps_partial_usage_and_nearest_rank_percentiles(self):
        records = [
            {
                "id": str(index),
                "model": "model",
                "path": "/v1/embeddings",
                "outcome": "completed",
                "request_ms": index + 1,
                "client_lag_ms": 0,
                "usage": {"prompt_tokens": 5} if index < 19 else {},
            }
            for index in range(20)
        ]
        summary = workload.summarize(records, 1000)
        group = summary["classes"][0]
        self.assertEqual(group["completed_requests_per_second"], 20)
        self.assertEqual(
            group["request_ms"],
            {"count": 20, "mean": 10.5, "p50": 10, "p95": 19, "max": 20},
        )
        self.assertEqual(
            group["prompt_tokens"],
            {"reported_requests": 19, "sum": 95, "per_second": None},
        )
        self.assertEqual(
            group["completion_tokens"],
            {"reported_requests": 0, "sum": None, "per_second": None},
        )

    def test_tool_call_identity_is_not_an_output_divergence(self):
        def respond(path, body):
            event = {
                "model": body["model"],
                "choices": [
                    {
                        "index": 0,
                        "delta": {
                            "tool_calls": [
                                {
                                    "index": 0,
                                    "id": body["input"],
                                    "type": "function",
                                    "function": {"name": "read", "arguments": "{}"},
                                }
                            ]
                        },
                        "finish_reason": "tool_calls",
                    }
                ],
                "usage": {"prompt_tokens": 4, "completion_tokens": 8},
            }
            return (
                200,
                "text/event-stream",
                b"data: " + json.dumps(event).encode() + b"\n\ndata: [DONE]\n\n",
            )

        records = []
        requests = plan(
            *(
                request(
                    identity,
                    path="/v1/chat/completions",
                    body={
                        "model": "tools",
                        "input": identity,
                        "stream": True,
                        "stream_options": {"include_usage": True},
                    },
                )
                for identity in ("one", "two")
            )
        )
        with endpoint(respond) as (address, _):
            workload.replay(address, requests, 2, records.append)
        self.assertTrue(all(record["outcome"] == "completed" for record in records))
        self.assertEqual(records[0]["response_sha256"], records[1]["response_sha256"])
        self.assertIsNone(records[0]["first_text_ms"])
        self.assertEqual(len(records[0]["output_chunk_ms"]), 1)
        self.assertEqual(records[0]["usage"]["completion_tokens"], 8)


if __name__ == "__main__":
    unittest.main()
