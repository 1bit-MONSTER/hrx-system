# Observe a model server

`loom-serve` exposes current state, cumulative counters and bounded event history
through separate HTTP views. Reading them neither runs inference nor activates
cold weights. Slow observers can miss events without blocking inference or
preventing model removal.

The [serving walkthrough](serve-models.md) provides an arithmetic model for
exercising these endpoints. [Operations](operate-model-server.md) explains the
registration, admission and residency states they describe.

## Start with status and counters

```sh
curl --fail --silent --show-error http://127.0.0.1:8080/v1/status
curl --fail --silent --show-error http://127.0.0.1:8080/metrics
```

Status includes the current registrations plus a `server` object, even when
no models are loaded. That object reports actual worker count, admission limits,
bound addresses and the ordered device inventory. Worker count is provisioned
capacity, not merely the requested maximum.

Each model's `loom.status` contains phase, queued/active counts and the oldest
age in each ownership class. Active age includes earlier queue residence,
preparation, capacity waiting and output encoding. A disconnected peer can
remain counted until its accepted work retires. These are scheduler ownership
observations, not device busy time or completed-request latency percentiles.

`/metrics` uses Prometheus text format with `model`, `kind` and registration
`incarnation` labels. Gauges report current phase and ownership counts.
Work counters include requests, completed, rejected,
cancelled, failed, input tokens, output tokens and batches.
`loom_serve_server_*_total` includes final counters from removed registrations.
Reusing a name creates a new incarnation rather than resetting the old series.

Counters are maintained independently of log retention. Service counters include
model-specific controls such as text checkpoints, but exclude router-owned raw
tokenization, health/observation calls and unknown-route rejections. Unavailable
counters are reported as unavailable, not zero. Disabling periodic heartbeats
with `--heartbeat_ms=0` does not disable counting.

## Distinguish parameter pressure from queued work

Each prepared model's `loom.parameters` reports accepted-consumer `pins` and an
`admission` snapshot: `waiting`, `count`, `duration_seconds`, and `age_seconds`.
The snapshot describes the model's single outstanding interest in shared
parameter admission. A wait can reflect physical backing pressure, an older
waiting model, or asynchronous reclamation that must retire first. It is not
one observation per request, and it ends before admitted parameter I/O and
execution complete.

The corresponding Prometheus series are:

| Metric suffix after `loom_serve_model_` | Meaning |
| --- | --- |
| `parameter_pins` | Accepted-consumer and explicit-retention pins protecting parameters. |
| `parameter_admission_waiting` | One when new work is waiting for parameter admission; otherwise zero. |
| `parameter_admission_waits_total` | Distinct wait episodes, not repeated scheduler polls. |
| `parameter_admission_wait_seconds_total` | Cumulative waiting, including cancelled waits and the currently open interval. |
| `parameter_admission_wait_age_seconds` | Age of the current wait, or zero when none is open. |

Accepted consumers can retain pins while new work for the same model waits;
their nested execution does not queue behind the work it must unblock. These
pins are separate from the persistent `loom.parameter_pinned` eviction policy.
Releasing a consumer pin does not clear that policy, and cancelling a new-work
interest does not release another consumer's pin.

Parameter observations have the model's incarnation identity and disappear on
removal. Unlike completed-request and work totals, they have no server-lifetime
aggregate. A registration without parameter observations has no samples, not
synthetic zero values. Polling snapshots neither admits work nor activates
parameters. Current wait age and cumulative duration advance while a wait is
open even when no request has completed.

## Measure completed-request latency

`loom_serve_model_request_duration_seconds` is a cumulative histogram with the
same model identity labels plus `outcome`: `completed`, `rejected`, `cancelled`
or `failed`. Its `_bucket`, `_count` and `_sum` series count each terminal
request once. `loom_serve_server_request_duration_seconds` retains durations
from removed registrations, just like server work counters. Both are independent
of event-ring retention and heartbeat frequency.

For example, this PromQL expression estimates the recent p95 for successful
requests, keeping each model incarnation separate:

```promql
histogram_quantile(0.95,
  sum by (model, kind, incarnation, le) (
    rate(loom_serve_model_request_duration_seconds_bucket{outcome="completed"}[5m])
  )
)
```

Duration starts when a request reaches its model protocol and ends when that
owner retires it. It includes preparation, model admission waits, execution and
encoding. Successful retirement means the response was accepted by the HTTP
carrier, not received by the peer. Time held by a drained router before model
admission is outside this measurement. Cancellation durations include any work
that must retire after the peer leaves. Router-owned utilities and unknown-route
rejections are outside the model protocol, as with work counters.

Buckets span 1 millisecond through 1000 seconds plus an unbounded bucket. Bucket
percentiles are estimates; `_sum / _count` gives the mean without bucket
rounding. An empty distribution is not a zero-latency observation. If a borrowed
service does not publish durations, its histogram is absent and
`loom_serve_server_latency_available` is zero instead of exposing an incomplete
server total. Client completion still requires client measurement; this
histogram does not claim to be TTFT or device execution time.

## Measure first meaningful output

`loom_serve_model_first_output_seconds` measures model protocol receipt through
the first meaningful result accepted by the HTTP carrier. It has the same
identity labels and duration buckets, without an `outcome` label: a streaming
request can publish output and later fail or be cancelled. The server-scoped
`loom_serve_server_first_output_seconds` retains samples from removed models.
Each request contributes at most one sample, independently of log retention.

For streaming text, content, reasoning, or tool calls count as output; HTTP
headers, assistant-role announcements, empty deltas, usage, and errors do not.
An EOS-only empty completion has no sample. Nonstreaming text contributes when
its nonempty response is accepted, not when internal generation first produces
a token. Image, embedding, and decision requests contribute when their complete
result is accepted. Checkpoint controls and raw tokenization are not inference
output. These distinctions make the distributions useful across modalities
without calling every response a token.

The corresponding `first_output` event carries the request ID and `duration_ns`
for correlation with `request_begin` and `request_end`. It reports server-side
publication, not network delivery or client-visible TTFT. Text's separate
`request_stats.first_token_ns` measures admission to host processing of the first
selected token, including EOS; it is not the first-output metric. Router-held
time remains outside both server-side latency distributions.

## Poll bounded history

Without a selector, `/logz` reads server scope. `X-Loom-Model` selects a model:

```sh
curl --fail --silent --show-error http://127.0.0.1:8080/logz \
  -H 'X-Loom-Model: vectors'
```

The response is newline-delimited JSON containing stream metadata, at most 64
retained records, an available current counter snapshot, and a cursor. Metadata
includes the stream `identity`, oldest retained position, publication head and
ring capacity. `partial:true` means this is not a complete view from the start.
A `gap` gives the skipped absolute position interval when publication has
overwritten unread history.

The last `cursor` record's `value` is an opaque resume token. Pass it unchanged
as `X-Loom-Cursor` on the next poll. Each reader owns its cursor independently;
polling does not consume or clear records for another reader. A malformed or
ahead-of-publication cursor returns 400. A cursor from another stream incarnation
returns 409; rediscover the model and start a new stream instead of combining
unrelated histories.

## Stream the same records over SSE

```sh
curl --no-buffer --fail --silent --show-error \
  http://127.0.0.1:8080/logz/stream -H 'X-Loom-Model: vectors'
```

Each `telemetry` SSE event contains the JSONL batch in its data and the next
cursor in its `id`. Reconnect with `Last-Event-ID` to resume. `X-Loom-Cursor`
also works; if both are present they must agree. The client can stop reading
without holding producer slots. Resuming after a wrap reports a gap and current
counters rather than inventing missing events.

Removal emits `stream_end` when output credit permits and closes that model's
observation connection. A new registration with the same name has a new identity.
Readers can rediscover registrations through `/v1/status`; observing a name does
not keep its model alive. Ctrl-C in curl stops the observer, not the server.

The same stream works over a Unix listener:

```sh
curl --unix-socket /srv/run/loom-serve.sock --no-buffer \
  http://localhost/logz/stream -H 'X-Loom-Model: vectors'
```

## Inspect requests and retained sessions

```sh
curl --fail --silent --show-error http://127.0.0.1:8080/v1/requests \
  -H 'X-Loom-Model: vectors'
curl --fail --silent --show-error http://127.0.0.1:8080/v1/sessions \
  -H 'X-Loom-Model: scout'
```

The second command requires a text deployment named `scout`. Stateless image,
embedding and decision services return 501 for session inspection. An absent
name returns 404; an unprepared or retiring registration returns 409. Inspection
does not load a cold model or reopen its gate.

Each page examines at most 64 owner slots. A non-null `next_offset` becomes
`X-Loom-Offset` on the next request; an empty page can still have a next offset.
Pages are live captures, not a frozen snapshot. Deduplicating IDs and restarting
when `incarnation` or telemetry `identity` changes keeps a client from confusing
two registration lifetimes. Router-held and tokenizer requests are reported
separately from model-admitted entries.

Session IDs survive movement between `storage:"row"` and `storage:"cache"`.
`request:null` indicates idle retained history. Positions describe completed,
owner-published work rather than in-flight device writes. Referenced resident
tokens may include shared prefixes, so summing per-session counts does not
measure unique physical memory. Reserved tokens describe completion capacity,
not additional resident tokens. These responses expose no prompts or generated
content.

## Interpret memory and packing evidence

`server.resources.devices[].memory` separates elastic retained backing from
invocation workspace. Retained values cover serving-managed parameters/state
on that device; fixed retained accounting is `null`, not zero. An elastic
`limit_bytes:null` means unbounded by serving policy.

Workspace values are independent HAL observations, not one coherent device-wide
snapshot. Its cumulative acquire/release/reuse counters survive model removal
and pool trimming. `wait_count` counts reservations needing a reuse prerequisite,
not blocked host threads or elapsed wait time. An exhausted attempt can be
followed by growth and success; it is not a failed-request counter.

Neither scope includes all host allocations, fixed buffers, executable/driver
storage or physical padding. Their sum is not peak process RAM, whole-device
VRAM or immediately available memory. Status does not poll hardware sensors.

| Question | Useful evidence |
| --- | --- |
| Is work waiting outside model admission? | Router-held counts versus queued/active model requests and their oldest ages. |
| Are parameters preventing new work from admitting? | `parameters.admission.waiting`, its age, and peer consumer pins or persistent parameter pinning. |
| Is logical KV capacity full? | `text_memory.pool` resident plus reserved token positions versus capacity. |
| Is output backpressure retaining rows? | `text_activity.backpressured_rows`, separate from prefill/decode rows. |
| Are smaller requests bypassing a blocked head? | `admit.queue_rank`, `overtakes` and `overtake_budget`. |
| Is reuse reducing prompt work? | Response `prompt_tokens_details.cached_tokens` and text-work counters. |
| Is packing completing more useful work? | Client throughput/latency together with epoch token slots, committed work and draft acceptance. |

An `epoch` event covers a completed host-visible cohort including device-fed
continuation. `token_capacity` describes its first bucket; `token_slots` sums
the capacities actually issued across the cohort. Drafts rejected by the
verifier consume slots without producing committed tokens. Dividing continuation
work by only the first bucket misstates fill. A null capacity observation means
the selected execution path does not provide that measure.

## Measure the user-visible result

For text, record client send time, first emitted token and completion. Report
time to first token and the distribution of completion times alongside useful
output tokens per second across the full concurrent cohort. Image latency ends
when the encoded image is available; embedding/decision latency ends when the
complete response is available. Keeping these boundaries explicit is more useful
than assigning every modality a token-rate score.

A controlled comparison uses the same prompts, output limits, cache policy,
arrival pattern, image geometry or vector dimensions. Cold preparation and warm
inference are separate measurements. Draft proposals are not delivered tokens;
cached input is not newly computed prefill. Event gaps preserve counter totals
but prevent exact event-by-event latency reconstruction. Hardware timing,
bandwidth, power and peak-memory claims require corresponding measurements;
queue phase, packed occupancy and allocated-byte accounting do not establish them.

## Inspect host work in Tracy

A Tracy-enabled `loom-serve` exposes serving zones alongside IREE's task,
I/O, and HAL instrumentation. Start the server normally and attach the Tracy
profiler. Worker lanes retain their actual thread identities; a model does not
create a dedicated thread.

Serving zones distinguish scheduler turns, source compilation, input preparation,
output processing, queue submission, and completion handling. Image output has
separate PNG and JSON encoding zones. Model turns carry the deployment name and
registration incarnation. Service work carries the deployment name, the ordinal
from its `/logz` stream identity, and the relevant request or cohort IDs. These
numeric annotations appear in that order; CPU preparation and encoding zones
also include the phase name. These serving annotations contain no prompts or
generated content.

Each CPU zone stays on the worker that began it and ends before the instrumented
call returns. Asynchronous work gets its own zone on the worker that runs it.
The gap between submission and completion handling can include device execution
and readiness delivery; it is not a CPU zone or a device-duration measurement.
Comparing those boundaries with `/logz` helps distinguish preparation, submission,
and encoding from waiting. Tracing changes timing, so throughput comparisons use
uninstrumented runs of the same workload.
