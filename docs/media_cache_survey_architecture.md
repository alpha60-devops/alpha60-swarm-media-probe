# Media Cache Survey Architecture

## Scope

`media_cache_survey` is a Linux-only C++20 subsystem that inventories and validates cached partial-media artifacts. It operates on a torrent collection directory and a recursive `download.cache` root.

The survey is separate from production acquisition. It works only with bytes already present in `.sized` artifacts and cannot ask libtorrent for missing ranges.

## High-level flow

```text
torrent directory
    │
    ├── inventory *.torrent
    │
download.cache
    │
    ├── recursively discover *.sized
    │
    ├── sampled container-marker scan
    │      moov / moof / EBML / RIFF-AVI / TS sync
    │
    ├── MediaInfo subprocess and JSON validation
    │
    ├── source larger than archive limit?
    │      │
    │      └── FFprobe demux check
    │             │
    │             └── FFmpeg bounded stream-copy redux
    │                    explicit muxer
    │                    extension-preserving temporary path
    │                    size limit
    │                    subtitle retry
    │
    ├── redux marker and MediaInfo validation
    │
    └── atomic JSON report
```

## Discovery

Discovery performs two scans:

- non-recursive `.torrent` discovery for collection context;
- recursive `.sized` discovery for the survey workload.

All report paths are stored relative to the relevant roots where possible.

## Container marker scanner

The scanner reads bounded head and tail samples and recognizes:

| Family | Structural signal |
|---|---|
| MP4 / MOV | `moov` |
| fragmented MP4 | `moof` |
| Matroska / WebM | EBML signature |
| AVI | RIFF header with `AVI ` form type |
| MPEG-TS | sync byte `0x47` |

The marker scan is deliberately independent from semantic extraction. MediaInfo success is authoritative for whether useful metadata can be recovered.

## MediaInfo runner

MediaInfo runs as a controlled child process. A pass requires process startup, no timeout, exit code zero, parseable JSON, and a General track.

The default timeout is 20 seconds. stdout and stderr are drained concurrently with `poll()` to avoid deadlock.

## Redux planner

Files larger than the default 128 MiB maximum are candidates. The survey maps the classified container to an explicit muxer and invokes FFprobe first.

When duration and bitrate are available, the survey estimates a target duration from the safety-margin byte target. Missing duration or bitrate does not change the `-fs` byte limit, but an input that FFprobe cannot demux is rejected before FFmpeg.

## Redux subprocess

Redux uses stream copy and selects only useful stream classes:

```text
video: first optional video stream
audio: optional audio streams
subtitles: optional on attempt one, omitted on attempt two
data: disabled
```

Temporary output paths preserve the media extension:

```text
output.partial.mkv
output.partial.mp4
```

An explicit `-f` muxer is also supplied. This avoids the former failure mode where a path ending only in `.partial` left FFmpeg unable to select an output format.

MP4 and MOV use `+faststart`. Fragmented MP4 uses empty-moov and fragment flags. `-fs` bounds the candidate toward the configured archive target.

## Redux validation

A survey redux is accepted only when:

1. FFmpeg succeeds before timeout.
2. The temporary output exists and is nonempty.
3. The output does not exceed the hard maximum.
4. The temporary file is renamed successfully.
5. The structural marker check passes.
6. MediaInfo extracts valid metadata.

The original `.sized` artifact is never modified.

## Report model

The report keeps four dimensions independent:

```text
original marker result
original MediaInfo result
redux generation result
redux MediaInfo result
```

Primary named lists are:

```text
sized-metadata-pass
sized-metadata-fail
sized-metadata-mediainfo-pass
sized-metadata-mediainfo-fails
sized-metadata-mediainfo-pass-redux
sized-metadata-mediainfo-fails-redux
```

Each observation also records command exit codes, elapsed times, timeout state, paths, byte sizes, and diagnostic text.

## Failure model

Each subprocess records whether it started, timed out, exited normally, or was terminated. On timeout, the survey sends `SIGTERM` to the process group, waits briefly, sends `SIGKILL`, and reaps the child.

Typical redux failure categories are:

- unsupported container;
- FFprobe cannot demux the partial source;
- FFmpeg timeout or mux failure;
- missing temporary output;
- invalid or oversized output;
- structural marker failure; or
- MediaInfo validation failure.

## Production relationship

The production `media_redux` component and the survey redux share the same design principles, but operate at different points:

- **Production redux** runs before the sparse torrent backing file is deleted and benefits from verified, container-aware head/tail acquisition.
- **Survey redux** runs later against a previously archived `.sized` artifact and cannot fill missing pieces.

For this reason, production redux should normally be preferred for creating future cache artifacts, while the survey remains useful for auditing historical cache data.

## Concurrency

The survey currently processes files serially. Future parallelization should use separate bounded queues because marker scans, MediaInfo, and FFmpeg have different resource profiles. FFmpeg redux should remain the most tightly limited because it is I/O intensive.
