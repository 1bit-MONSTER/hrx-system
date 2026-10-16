# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Independent, bounded snapshot and SSE readers for model harnesses.

Each selector owns a cursor. Live streams retain one transport connection but
never ring slots or model lifetime. A slow consumer can lose records without
blocking publication. Incarnation changes are explicit, never silently spliced.
"""

import contextlib
import dataclasses
import http.client
import io
import json
import select
import socket
import ssl
import threading
from urllib.parse import urlsplit


@dataclasses.dataclass(frozen=True)
class LogBatch:
    # Routing and incarnation metadata shared by every record in this response.
    stream: dict
    # Copied wire records, including stream header, counters and cursor footer.
    records: list


class ModelNotFound(RuntimeError):
    """A selected model disappeared or has not been registered yet."""

    def __init__(self, model):
        super().__init__(f"GET /logz ({model!r}): HTTP 404: model is not registered")
        self.model = model


class _StreamReader(io.RawIOBase):
    """Keep HTTP buffering above a read that can wait on data or cancellation."""

    def __init__(self, source, transport, cancelled, wakeup):
        self.source = source
        self.transport = transport
        self.cancelled = cancelled
        self.wakeup = wakeup

    def readable(self):
        return True

    def readinto(self, buffer):
        while not self.cancelled.is_set():
            writing = False
            try:
                count = self.source.readinto(buffer)
                if count is not None:
                    return count
            except ssl.SSLWantReadError:
                pass
            except ssl.SSLWantWriteError:
                writing = True
            # After response headers, the transport is nonblocking. A partial
            # line, chunk, or TLS record must not strand cancellation in recv.
            select.select(
                [self.wakeup] if writing else [self.wakeup, self.transport],
                [self.transport] if writing else [],
                [],
            )
        return 0

    def close(self):
        try:
            self.source.close()
        finally:
            super().close()


class _StreamSocket:
    """Adapt HTTPResponse's socket input without replacing its HTTP framing."""

    def __init__(self, transport, cancelled, wakeup):
        self.transport = transport
        self.cancelled = cancelled
        self.wakeup = wakeup

    def makefile(self, mode):
        return io.BufferedReader(
            _StreamReader(
                self.transport.makefile(mode, buffering=0),
                self.transport,
                self.cancelled,
                self.wakeup,
            )
        )


class LogStream:
    """One reader's SSE connection; cancel before joining a blocked read.

    The enclosing LogClient.stream context owns transport destruction and must
    outlive the reader. Only cancel may be called concurrently with read.
    """

    def __init__(self, client, model, response, transport, cancelled, wakeup):
        self.client = client
        self.model = model
        self.response = response
        self.transport = transport
        self.cancelled = cancelled
        self.wakeup = wakeup
        self.ended = False

    def cancel(self):
        self.cancelled.set()
        # Peer closure wakes select on every transport, including Windows where
        # shutdown need not interrupt an outstanding blocking socket receive.
        self.wakeup.close()

    def read(self):
        """Return one validated batch, or None after cancellation/stream_end."""
        if self.cancelled.is_set() or self.ended:
            return None
        try:
            return self._read_frame()
        except (OSError, ValueError, http.client.HTTPException):
            if self.cancelled.is_set():
                return None
            raise

    def _read_frame(self):
        remaining = 1024 * 1024
        fields = {}
        records = []
        while True:
            line = self.response.readline(remaining + 1)
            if not line:
                raise ValueError("/logz/stream: EOF before a complete stream_end")
            remaining -= len(line)
            if remaining < 0:
                raise ValueError("/logz/stream: frame exceeds 1 MiB")
            if not line.endswith(b"\n"):
                raise ValueError("/logz/stream: truncated frame")
            line = line.rstrip(b"\r\n")
            if not line:
                break
            if line.startswith(b":"):
                continue
            name, separator, value = line.partition(b":")
            if value.startswith(b" "):
                value = value[1:]
            if not separator or name not in (b"event", b"id", b"data"):
                raise ValueError("/logz/stream: invalid telemetry field")
            if name == b"data":
                records.append(json.loads(value))
            else:
                if name in fields:
                    raise ValueError("/logz/stream: duplicate telemetry field")
                fields[name] = value.decode()
        if fields.get(b"event") == "stream_end":
            previous = self.client.cursors.get(self.model)
            if (
                len(records) != 1
                or not isinstance(records[0], dict)
                or records[0].get("reason") != "model_unregistered"
                or not isinstance(records[0].get("identity"), str)
                or (previous and records[0]["identity"] != previous.rsplit(":", 1)[0])
            ):
                raise ValueError("/logz/stream: invalid stream_end")
            self.ended = True
            return LogBatch(
                {
                    "scope": "model",
                    "model": self.model,
                    "identity": records[0]["identity"],
                },
                [{"event": "stream_end", **records[0]}],
            )
        if fields.get(b"event") != "telemetry" or b"id" not in fields:
            raise ValueError("/logz/stream: expected telemetry with a resume id")
        return self.client._accept_batch(
            self.model,
            records,
            self.client.cursors.get(self.model),
            event_id=fields[b"id"],
        )


class LogClient:
    def __init__(self, url, timeout=10.0, *, unix_socket=None):
        """Read HTTP logs, optionally through a raw filesystem socket path.

        The origin still supplies the HTTP Host header. Unix connections use
        plain HTTP and never resolve the origin's host or fall back to TCP.
        """
        address = urlsplit(url)
        if (
            address.scheme not in ("http", "https")
            or not address.hostname
            or address.username is not None
            or address.password is not None
            or address.path not in ("", "/")
            or address.query
            or address.fragment
        ):
            raise ValueError("server URL must be an HTTP(S) origin")
        if unix_socket is not None:
            if address.scheme != "http":
                raise ValueError("Unix socket observation requires a plain HTTP origin")
            if (
                not isinstance(unix_socket, str)
                or not unix_socket
                or "\0" in unix_socket
            ):
                raise ValueError("Unix socket path must be a nonempty filesystem path")
            if not hasattr(socket, "AF_UNIX"):
                raise ValueError("Unix sockets are unavailable on this platform")
        self.address = address
        # Literal local filesystem path, or None for the origin's TCP endpoint.
        self.unix_socket = unix_socket
        # Snapshot/connection deadline, never a deadline on an idle SSE stream.
        self.timeout = timeout
        self.cursors = {}

    @contextlib.contextmanager
    def _connection(self):
        connection_type = (
            http.client.HTTPSConnection
            if self.address.scheme == "https"
            else http.client.HTTPConnection
        )
        connection = connection_type(
            self.address.hostname, self.address.port, timeout=self.timeout
        )
        try:
            if self.unix_socket is not None:
                connection.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                connection.sock.settimeout(self.timeout)
                connection.sock.connect(self.unix_socket)
            yield connection
        finally:
            connection.close()

    def _get(self, path, headers=None):
        with self._connection() as connection:
            connection.request("GET", path, headers=headers or {})
            response = connection.getresponse()
            # /logz currently renders at most 64 fixed-size records per poll.
            # Bound memory even when pointed at a different or broken endpoint.
            payload = response.read(1024 * 1024 + 1)
            if len(payload) > 1024 * 1024:
                raise ValueError(f"{path}: observation response exceeds 1 MiB")
            return response.status, payload

    @contextlib.contextmanager
    def stream(self, model=None):
        """Follow one incarnation, resuming its cursor without polling/retries.

        One reader at a time owns this selector's cursor. Rejected resumes are
        errors; the caller decides whether to forget the previous incarnation.
        Cancellation does not advance past an incomplete frame.
        """
        headers = {} if model is None else {"X-Loom-Model": model}
        previous = self.cursors.get(model)
        if previous is not None:
            headers["Last-Event-ID"] = previous
        with self._connection() as connection:
            connection.request("GET", "/logz/stream", headers=headers)
            transport = connection.sock
            cancelled = threading.Event()
            wakeup_read, wakeup_write = socket.socketpair()
            with wakeup_read, wakeup_write:

                def response_class(transport, **kwargs):
                    return http.client.HTTPResponse(
                        _StreamSocket(transport, cancelled, wakeup_read), **kwargs
                    )

                connection.response_class = response_class
                with connection.getresponse() as response:
                    if response.status == 404 and model is not None:
                        raise ModelNotFound(model)
                    if response.status != 200:
                        raise RuntimeError(
                            f"GET /logz/stream ({model!r}): HTTP {response.status}"
                        )
                    if response.getheader("Content-Type") != "text/event-stream":
                        raise ValueError("/logz/stream: expected text/event-stream")
                    # Header reads retain the connection deadline. Idle streams
                    # wait indefinitely on data or the cancellation wake socket.
                    transport.setblocking(False)
                    yield LogStream(
                        self, model, response, transport, cancelled, wakeup_write
                    )

    def models(self):
        """List registered models, including preparation and retirement.

        Observation follows the administrative inventory rather than the
        inference catalog so draining/removal cannot hide the final records.
        """
        status, payload = self._get("/v1/status")
        if status != 200:
            raise RuntimeError(f"GET /v1/status: HTTP {status}: {payload!r}")
        catalog = json.loads(payload)
        models = [item["id"] for item in catalog["data"]]
        if any(not isinstance(model, str) or not model for model in models):
            raise ValueError("/v1/status: expected nonempty model identifiers")
        if len(set(models)) != len(models):
            raise ValueError("/v1/status: duplicate model identifier")
        return models

    def forget(self, model):
        """Release a no-longer-observed cursor, returning its last position."""
        return self.cursors.pop(model, None)

    def poll(self, model=None):
        """Copy one batch, or raise ModelNotFound for an absent model selector.

        Absence preserves the last cursor until the caller records it and
        forgets the model, or attempts to resume a replacement incarnation.
        A missing server stream remains an endpoint error, not model churn.
        """
        headers = {} if model is None else {"X-Loom-Model": model}
        previous = self.cursors.get(model)
        if previous is not None:
            headers["X-Loom-Cursor"] = previous
        status, payload = self._get("/logz", headers)
        reset = status == 409 and previous is not None
        if reset:
            del headers["X-Loom-Cursor"]
            status, payload = self._get("/logz", headers)
        if status == 404 and model is not None:
            raise ModelNotFound(model)
        if status != 200:
            raise RuntimeError(f"GET /logz ({model!r}): HTTP {status}: {payload!r}")
        if not payload.endswith(b"\n"):
            raise ValueError("/logz: incomplete observation batch")
        records = [json.loads(line) for line in payload.splitlines()]
        return self._accept_batch(model, records, previous, reset=reset)

    def _accept_batch(self, model, records, previous, *, reset=False, event_id=None):
        if (
            len(records) < 2
            or any(not isinstance(record, dict) for record in records)
            or records[0].get("event") != "stream"
            or records[-1].get("event") != "cursor"
            or any(not isinstance(record.get("event"), str) for record in records)
        ):
            raise ValueError("/logz: incomplete observation batch")
        header, footer = records[0], records[-1]
        identity = header.get("identity")
        if (
            header.get("scope") != ("server" if model is None else "model")
            or header.get("model") != model
            or not isinstance(identity, str)
            or len(identity.split(":")) != 2
            or not all(
                part.isascii() and part.isdecimal() for part in identity.split(":")
            )
        ):
            raise ValueError("/logz: invalid stream identity or selector")
        position = 0 if previous is None or reset else int(previous.rsplit(":", 1)[1])
        if (
            previous is not None
            and not reset
            and not previous.startswith(identity + ":")
        ):
            raise ValueError("/logz: incarnation changed without a cursor rejection")
        for record in records[1:-1]:
            if record["event"] == "gap":
                end = record.get("to")
                if (
                    record.get("from") != position
                    or type(end) is not int
                    or end <= position
                ):
                    raise ValueError("/logz: invalid gap extent")
                position = end
            elif record["event"] != "counters":
                if (
                    type(record.get("sequence")) is not int
                    or record["sequence"] != position
                ):
                    raise ValueError("/logz: discontinuity without a gap record")
                position += 1
        cursor = footer.get("value")
        if footer.get("next") != position or cursor != f"{identity}:{position}":
            raise ValueError("/logz: cursor does not cover the copied records")
        if event_id is not None and event_id != cursor:
            raise ValueError("/logz/stream: event id does not match batch cursor")
        stream = {"scope": header["scope"], "identity": identity}
        if model is not None:
            stream["model"] = model
        if reset:
            records.insert(0, {"event": "stream_reset", "previous_cursor": previous})
        self.cursors[model] = cursor
        return LogBatch(stream, records)
