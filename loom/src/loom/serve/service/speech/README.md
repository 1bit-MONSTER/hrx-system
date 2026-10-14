# Stream source-defined speech

A `speech` deployment serves `POST /v1/audio/speech` from a source model package.
An explicit `response_format: "pcm"` produces signed little-endian PCM16 at
24 kHz, mono. Audio is delivered incrementally; request completion closes the
response. TCP read boundaries need not coincide with waveform blocks.

```json
{
  "model": "speech",
  "input": "Good day.",
  "voice": "alba",
  "response_format": "pcm"
}
```

Voice identifiers belong to the loaded package. A custom-voice object
`{"id":"alba"}` passes the same identifier to source preparation. Optional
`instructions`, `speed` (0.25–4, default 1) and `seed` are also passed to source;
the package rejects controls it cannot implement. `stream_format` accepts
`"audio"`. Encoded-file formats, SSE and an omitted response format reject;
they are not silently converted to raw PCM. This endpoint does not register or
persist voice samples. Package assets supply its conditioning inputs.

The [waveform fixture](../../fixture/speech/README.md) is an executable example
of the model contract. It generates deterministic samples, not learned speech.

## Model ownership

The [model ABI](model.h) has separate cold preparation, CPU request preparation,
bounded generation and retired-feedback interpretation. Source preparation
chooses checkpoint domains, stages, capacities, waveform geometry and initial
conditioning. The native model does not encode a particular vocoder, speaker
vector, latent width, number of diffusion steps or waveform block size.

Four dynamic roots carry immutable request bytes, reusable model conditioning,
mutable per-request state and bounded device output. State crossing commands
lives in the stream bank. Compiler-private workspace belongs to one complete
command and is released by the shared runner. Explicit request objects can
retain their own source state without VM-global stream cursors.

`generate` can advance a stage without publishing feedback. When it publishes
feedback, `complete` returns an absolute frame position, byte range, live frame
count and EOF. Zero frames are independent of EOF. Native code validates this
external range once before encoding; subsequent consumers use those facts.
Conditioning becomes reusable only after its producer retires. A cancelled
partial computation cannot publish a valid cache entry.

The [generator](generator.h) carries native sample rate and channel count.
The PCM HTTP service currently requires 24 kHz mono at registration; this wire
format does not redefine the model ABI's geometry. Supporting a package with a
different native geometry requires an explicit continuous audio conversion
path, including filter history and final tail handling.

## Scheduling and retirement

One request per registration holds residency; a bounded host queue owns pending
requests. Source preparation and feedback interpretation/encoding run on the
shared CPU executor. Queue submission and admission run on the serialized
service owner. Device work and feedback have independent completion frontiers,
waited together by the shared proactor. No worker waits for a device or socket.

At most one encoded block waits outside copied transport sends. The next model
unit starts after that block acquires send credit. A slow peer therefore cannot
create an unbounded waveform queue. Peer reset and shutdown suppress subsequent
model work, join accepted frontiers and queue request-storage release. Pins and
request handles are released only after that cleanup retires. Failed readiness
leaves ownership with the model's actual buffer-retirement path.

Dynamic registration and removal use the same constructors and teardown as
startup model documents. Protocol rejection preserves the prepared service;
terminal execution failures retain accepted consumers until registration
retirement. Discovery reports text input and audio output. Common telemetry
reports request outcomes and completed model units without treating waveform
frames as text tokens.
