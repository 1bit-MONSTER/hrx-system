# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Bounded HTTP replay with frozen inputs and observable client-side backlog."""

import hashlib
import heapq
import http.client
import json
import math
import re
import time
from concurrent.futures import FIRST_COMPLETED, ThreadPoolExecutor, wait
from dataclasses import dataclass
from urllib.parse import urlsplit

from loom_serve_testing import chat


@dataclass(frozen=True)
class Request:
    # Unique manifest identity, independent of any server session identity.
    identity: str
    # Origin-relative inference endpoint; never a model administration action.
    path: str
    # Frozen JSON encoding; replay never substitutes generated output.
    body: str
    # Additional HTTP headers, including optional retained-session identity.
    headers: tuple
    # Earlier request index, or None for an arrival relative to workload start.
    predecessor: int | None
    # Nanoseconds from workload start or predecessor HTTP completion.
    delay_ns: int


def parse(lines):
    """Validate a complete JSONL plan before allowing any HTTP side effects."""
    requests = []
    identities = {}
    endpoints = {
        "/v1/chat/completions",
        "/v1/embeddings",
        "/v1/images/generations",
        "/v1/systemone",
    }
    for line_number, line in enumerate(lines, 1):
        if not line.strip():
            continue
        try:
            value = json.loads(line)
            if not isinstance(value, dict):
                raise ValueError("request must be an object")
            unknown = value.keys() - {
                "id",
                "path",
                "body",
                "headers",
                "at_ms",
                "after",
                "delay_ms",
            }
            if unknown:
                raise ValueError(f"unknown request fields: {sorted(unknown)}")
            identity = value.get("id")
            if not isinstance(identity, str) or not identity or identity in identities:
                raise ValueError("id must be a unique nonempty string")
            path = value.get("path")
            if not isinstance(path, str) or path not in endpoints:
                raise ValueError("path must name a supported inference endpoint")
            body = value.get("body")
            if not isinstance(body, dict) or not isinstance(body.get("model"), str):
                raise ValueError("body must contain a string model")
            if not body["model"]:
                raise ValueError("model must not be empty")
            # These restrictions are measurement contracts, not model policy.
            # Single-choice usage cannot be attributed to several streamed paths.
            if path == "/v1/chat/completions":
                if body.get("stream") is not True or body.get("n", 1) != 1:
                    raise ValueError("chat replay requires stream=true and n=1")
                if body.get("stream_options", {}).get("include_usage") is not True:
                    raise ValueError(
                        "chat replay requires stream_options.include_usage"
                    )
            elif body.get("stream", False) is not False:
                raise ValueError(
                    "finite replay requires a JSON response, not streaming"
                )
            headers = value.get("headers", {})
            if not isinstance(headers, dict):
                raise ValueError("headers must be an object")
            lowered = set()
            for name, content in headers.items():
                if (
                    not re.fullmatch(r"[!#$%&'*+.^_`|~0-9A-Za-z-]+", name)
                    or not isinstance(content, str)
                    or any(
                        ord(character) < 32 or ord(character) > 126
                        for character in content
                    )
                    or name.lower() in lowered
                    or name.lower()
                    in {"host", "content-length", "content-type", "transfer-encoding"}
                ):
                    raise ValueError(f"invalid or transport-owned header: {name}")
                lowered.add(name.lower())
            predecessor = None
            if "after" in value:
                if "at_ms" in value:
                    raise ValueError("after and at_ms are mutually exclusive")
                if (
                    not isinstance(value["after"], str)
                    or value["after"] not in identities
                ):
                    raise ValueError("after must name an earlier request")
                predecessor = identities[value["after"]]
                delay = value.get("delay_ms", 0)
            else:
                if "delay_ms" in value or "at_ms" not in value:
                    raise ValueError("independent requests require at_ms, not delay_ms")
                delay = value["at_ms"]
            if type(delay) not in (int, float) or not math.isfinite(delay) or delay < 0:
                raise ValueError("arrival or think time must be finite and nonnegative")
            encoded = json.dumps(
                body, sort_keys=True, separators=(",", ":"), allow_nan=False
            )
            identities[identity] = len(requests)
            requests.append(
                Request(
                    identity,
                    path,
                    encoded,
                    tuple(headers.items()),
                    predecessor,
                    int(delay * 1e6),
                )
            )
        except (TypeError, ValueError, AttributeError, OverflowError) as error:
            raise ValueError(f"line {line_number}: {error}") from error
    if not requests:
        raise ValueError("workload must contain at least one request")
    return tuple(requests)


def origin(value):
    """Parse an HTTP(S) origin, rejecting silently ignored URL components."""
    address = urlsplit(value)
    if (
        address.scheme not in ("http", "https")
        or not address.hostname
        or address.path not in ("", "/")
        or address.query
        or address.fragment
        or address.username
        or address.password
    ):
        raise ValueError(
            "server must be an HTTP(S) origin without credentials or a path"
        )
    # Force validation now, not after another request has already started.
    address.port
    return address


def digest(value):
    """Hash canonical JSON, excluding transport IDs/timestamps at the caller."""
    return hashlib.sha256(
        json.dumps(
            value, sort_keys=True, separators=(",", ":"), allow_nan=False
        ).encode()
    ).hexdigest()


def _json_request(address, request, body):
    connection_type = (
        http.client.HTTPSConnection
        if address.scheme == "https"
        else http.client.HTTPConnection
    )
    connection = connection_type(address.hostname, address.port)
    try:
        connection.request(
            "POST",
            request.path,
            request.body,
            {"Content-Type": "application/json", **dict(request.headers)},
        )
        response = connection.getresponse()
        payload = response.read()
        completed = time.monotonic_ns()
        if response.status != 200:
            raise RuntimeError(f"HTTP {response.status}: {payload[:1024]!r}")
        result = json.loads(payload)
        if not isinstance(result, dict) or "error" in result:
            raise ValueError(f"invalid successful response: {str(result)[:1024]}")
        if "model" in result and result["model"] != body["model"]:
            raise ValueError("response was routed to the wrong model")
        return result, completed
    finally:
        connection.close()


def _execute(address, request, scheduled, start):
    body = json.loads(request.body)
    request_hash = digest([request.path, body, dict(request.headers)])
    launched = time.monotonic_ns()
    record = {
        "event": "request",
        "id": request.identity,
        "model": body["model"],
        "path": request.path,
        "scheduled_ms": (scheduled - start) / 1e6,
        "started_ms": (launched - start) / 1e6,
        "client_lag_ms": (launched - scheduled) / 1e6,
        "request_sha256": request_hash,
    }
    try:
        if request.path == "/v1/chat/completions":
            result = chat.request_body(address, body, dict(request.headers))
            completed = result["completed_ns"]
            offset = (result["started_ns"] - launched) / 1e6
            chunks = [offset + chunk for chunk in result["output_chunk_ms"]]
            output = {
                name: result[name]
                for name in ("text", "reasoning_content", "finish_reason")
            }
            output["tool_calls"] = [
                {name: value for name, value in call.items() if name != "id"}
                for call in result["tool_calls"]
            ]
            record.update(
                {
                    "first_output_ms": chunks[0] if chunks else None,
                    "first_text_ms": (
                        offset + result["first_text_ms"]
                        if result["first_text_ms"] is not None
                        else None
                    ),
                    "output_chunk_ms": chunks,
                    "finish_reason": result["finish_reason"],
                    "response_sha256": digest(output),
                }
            )
        else:
            result, completed = _json_request(address, request, body)
            # These are transport metadata, not generated image/vector/decision data.
            record["response_sha256"] = digest(
                {
                    name: value
                    for name, value in result.items()
                    if name not in ("id", "created", "model", "usage")
                }
            )
        usage = result.get("usage", {})
        if not isinstance(usage, dict):
            raise ValueError("response usage must be an object")
        for name in ("prompt_tokens", "completion_tokens", "total_tokens"):
            if name in usage and (type(usage[name]) is not int or usage[name] < 0):
                raise ValueError(f"invalid reported {name}")
        details = usage.get("prompt_tokens_details", {})
        if not isinstance(details, dict):
            raise ValueError("prompt_tokens_details must be an object")
        cached = details.get("cached_tokens")
        if cached is not None and (
            type(cached) is not int
            or cached < 0
            or ("prompt_tokens" in usage and cached > usage["prompt_tokens"])
        ):
            raise ValueError("invalid reported cached_tokens")
        record.update(outcome="completed", usage=usage)
    except (
        OSError,
        http.client.HTTPException,
        ValueError,
        RuntimeError,
        KeyError,
        TypeError,
        AttributeError,
    ) as error:
        completed = time.monotonic_ns()
        record.update(outcome="failed", error=f"{type(error).__name__}: {error}")
    record.update(
        completed_ms=(completed - start) / 1e6,
        request_ms=(completed - launched) / 1e6,
        arrival_to_completion_ms=(completed - scheduled) / 1e6,
    )
    return record, completed


def replay(address, requests, concurrency, emit):
    """Replay a validated plan, emitting completed/failed/blocked records.

    The caller owns the server and any outer deadline. At most concurrency
    requests have been submitted but not collected. A predecessor failure blocks
    its descendants; unrelated work continues without retries. The callback is
    serialized on this scheduler, and its cost is visible in subsequent lag.
    """
    if type(concurrency) is not int or concurrency < 1:
        raise ValueError("concurrency must be a positive integer")
    start = time.monotonic_ns()
    ready = []
    children = [[] for request in requests]
    for index, request in enumerate(requests):
        if request.predecessor is None:
            heapq.heappush(ready, (start + request.delay_ns, index))
        else:
            children[request.predecessor].append(index)
    records = []
    pending = {}
    with ThreadPoolExecutor(max_workers=concurrency) as workers:
        while ready or pending:
            now = time.monotonic_ns()
            while ready and ready[0][0] <= now and len(pending) < concurrency:
                scheduled, index = heapq.heappop(ready)
                future = workers.submit(
                    _execute, address, requests[index], scheduled, start
                )
                pending[future] = index
            delay = None
            if ready and len(pending) < concurrency:
                delay = max(0, (ready[0][0] - time.monotonic_ns()) / 1e9)
            if not pending:
                # This is the declared arrival delay, not a readiness guess.
                time.sleep(delay)
                continue
            done, _ = wait(pending, timeout=delay, return_when=FIRST_COMPLETED)
            for future in done:
                index = pending.pop(future)
                record, completed = future.result()
                records.append(record)
                emit(record)
                if record["outcome"] == "completed":
                    for child in children[index]:
                        heapq.heappush(
                            ready, (completed + requests[child].delay_ns, child)
                        )
                else:
                    blocked = list(children[index])
                    while blocked:
                        child = blocked.pop()
                        request = requests[child]
                        record = {
                            "event": "request",
                            "id": request.identity,
                            "model": json.loads(request.body)["model"],
                            "path": request.path,
                            "outcome": "blocked",
                            "blocked_by": requests[index].identity,
                        }
                        records.append(record)
                        emit(record)
                        blocked.extend(children[child])
    return summarize(records, (time.monotonic_ns() - start) / 1e6)


def _distribution(values):
    values = sorted(value for value in values if value is not None)
    if not values:
        return None
    return {
        "count": len(values),
        "mean": sum(values) / len(values),
        "p50": values[math.ceil(0.50 * len(values)) - 1],
        "p95": values[math.ceil(0.95 * len(values)) - 1],
        "max": values[-1],
    }


def summarize(records, elapsed_ms):
    """Report per-class results; absent token usage never becomes zero tokens."""
    groups = {}
    for record in records:
        groups.setdefault((record["model"], record["path"]), []).append(record)
    classes = []
    for (model, path), samples in sorted(groups.items()):
        completed = [sample for sample in samples if sample["outcome"] == "completed"]
        result = {
            "model": model,
            "path": path,
            "requests": len(samples),
            **{
                outcome: sum(sample["outcome"] == outcome for sample in samples)
                for outcome in ("completed", "failed", "blocked")
            },
            "completed_requests_per_second": len(completed) * 1000 / elapsed_ms,
        }
        # Completion latency is success-only. All attempted requests contribute
        # launch lag, so overload failures cannot hide a saturated client.
        for name in (
            "client_lag_ms",
            "request_ms",
            "arrival_to_completion_ms",
            "first_output_ms",
            "first_text_ms",
        ):
            population = samples if name == "client_lag_ms" else completed
            result[name] = _distribution(sample.get(name) for sample in population)
        for name in ("prompt_tokens", "completion_tokens"):
            values = [
                sample["usage"][name] for sample in completed if name in sample["usage"]
            ]
            result[name] = {
                "reported_requests": len(values),
                "sum": sum(values) if values else None,
                "per_second": (
                    sum(values) * 1000 / elapsed_ms
                    if values and len(values) == len(completed)
                    else None
                ),
            }
        cached = [
            sample["usage"].get("prompt_tokens_details", {}).get("cached_tokens")
            for sample in completed
        ]
        result["cached_prompt_tokens"] = {
            "reported_requests": sum(value is not None for value in cached),
            "sum": sum(value for value in cached if value is not None)
            if any(value is not None for value in cached)
            else None,
        }
        classes.append(result)
    return {"event": "summary", "elapsed_ms": elapsed_ms, "classes": classes}
