# Media Cache Survey Architecture

## Scope

`media_cache_survey` is a Linux-only C++20 subsystem that inventories and
validates cached partial-media artifacts. It operates on two filesystem roots:

```cpp
bool run(
    const fs::path& torrent_directory,
    const fs::path& download_cache_root);
```

The torrent directory identifies the media collection. The cache root contains
the previous swarm-probe artifacts, including recursively stored `.sized`
files.

## High-level architecture

```text
┌──────────────────────┐
│ torrent_directory    │
│ *.torrent inventory  │
└──────────┬───────────┘
           │
           │ collection context
           ▼
┌────────────────────────────┐
│ download_cache_root        │
│ recursive *.sized discovery│
└─────────────┬──────────────┘
              │
              ▼
┌────────────────────────────┐
│ lightweight container scan │
│ moov / moof / EBML         │
└─────────────┬──────────────┘
              │
              ▼
┌────────────────────────────┐
│ MediaInfo subprocess       │
│ JSON + timeout validation  │
└─────────────┬──────────────┘
              │
              ▼
       source > size limit?
          │           │
         no          yes
          │           ▼
          │  ┌──────────────────────┐
          │  │ ffprobe estimation   │
          │  │ duration / bitrate   │
          │  └──────────┬───────────┘
          │             ▼
          │  ┌──────────────────────┐
          │  │ FFmpeg stream-copy   │
          │  │ bounded redux output │
          │  └──────────┬───────────┘
          │             ▼
          │  ┌──────────────────────┐
          │  │ redux validation     │
          │  │ marker + MediaInfo   │
          │  └──────────┬───────────┘
          │             │
          └─────────────┴───────────┐
                                    ▼
                         ┌────────────────────┐
                         │ atomic JSON report │
                         └────────────────────┘
```

## Components

### Discovery

Discovery performs two independent scans:

- non-recursive `.torrent` discovery in `torrent_directory`
- recursive `.sized` discovery in `download_cache_root`

The first scan records collection context. The second scan defines the actual
survey workload.

### Container marker scanner

The lightweight scanner answers whether enough structural container metadata
is present to justify deeper probing.

Current checks include:

| Container family | Structural marker |
|---|---|
| MP4 / MOV | `moov` atom |
| fragmented MP4 | one or more `moof` atoms |
| Matroska / WebM | EBML header and recognizable structure |

This stage does not decode media and should remain inexpensive.

### MediaInfo runner

MediaInfo is run as a child process and must satisfy all of the following:

- process started
- process completed before the configured timeout
- exit code was zero
- output parsed as JSON
- output contains recognizable media metadata

The default timeout is 20 seconds.

The process wrapper should use POSIX process APIs rather than `std::system()`
so that stdout, stderr, deadlines, and process-tree termination are controlled.

### Redux planner

Files larger than the configured archive limit are candidates for remuxing.

Default limit:

```text
128 MiB
```

A reduction is not raw byte truncation. It is a stream-copy remux intended to
preserve:

- valid container headers and trailers
- packet boundaries
- existing video keyframes
- stream metadata
- MP4 `moov` placement or fMP4 fragmentation metadata

`ffprobe` provides duration and bitrate data. The implementation estimates a
target duration using a safety margin, invokes FFmpeg, checks the actual output
size, and retries with a shorter duration when necessary.

### Redux validation

A redux artifact is accepted only when:

1. FFmpeg succeeds.
2. The output exists and is nonempty.
3. Its size is at or below the configured maximum.
4. The lightweight structural scan succeeds.
5. MediaInfo successfully extracts metadata.

The original `.sized` file is never modified.

### Report serializer

The serializer writes a single JSON report under the cache survey directory:

```text
download.cache/.survey/download-cache-survey.json
```

The report includes:

- configuration
- torrent and `.sized` inventories
- named result lists
- detailed per-file observations
- subprocess timing
- exit codes
- timeout state
- failure stage and reason
- redux output paths and sizes

The report is written to a temporary file and atomically renamed into place.

## Result dimensions

The design deliberately keeps these results independent:

```text
container structure
MediaInfo extraction
redux generation
redux MediaInfo extraction
```

A sample may fail the lightweight marker scan but pass MediaInfo. That is useful
diagnostic information and should not be collapsed into one boolean.

## Named lists

The report exposes these primary lists:

```text
sized-metadata-pass
sized-metadata-fail
sized-metadata-mediainfo-pass
sized-metadata-mediainfo-fails
sized-metadata-mediainfo-pass-redux
sized-metadata-mediainfo-fails-redux
```

The first pair describes structural scanning. The second pair describes
MediaInfo behavior on the original sample. The final pair describes reduced
archives.

## Process and failure model

Each external command records:

```cpp
struct command_result
{
    int exit_code;
    bool started;
    bool timed_out;
    bool terminated_by_signal;
    std::chrono::milliseconds elapsed;
    std::string standard_output;
    std::string standard_error;
};
```

Recommended timeout behavior:

1. Start the child in its own process group.
2. Drain stdout and stderr with `poll()`.
3. Check completion with `waitpid(..., WNOHANG)`.
4. Send `SIGTERM` after the deadline.
5. Send `SIGKILL` after a short grace period.
6. Reap the child and store diagnostics.

## Concurrency model

The workload naturally separates into three resource classes:

| Work type | Cost | Suggested concurrency |
|---|---:|---:|
| container marker scan | low CPU and I/O | hardware concurrency |
| MediaInfo | moderate process and I/O | up to 4 |
| FFmpeg stream copy | high I/O | 1 or 2 |

Bounded queues are preferable to spawning one child process per discovered
file.

## Repository integration

Expected documentation layout:

```text
index.md
docs/
├── media_cache_survey.md
├── media_cache_survey_architecture.md
└── media_cache_survey_sequence.svg
```

The corresponding implementation file is:

```text
media_cache_survey.cpp
```

## Development conclusions

The most reusable conclusions from this work are:

- Structural container detection and semantic metadata extraction are separate
  signals.
- MP4 `+faststart` relocates metadata; it does not enforce an output byte limit.
- Exact byte limits require estimation, safety margin, output measurement, and
  bounded retries.
- `-c copy` preserves existing keyframes but cannot invent new ones.
- Container-specific FFmpeg flags must be selected from an explicit whitelist.
- Timeout-capable subprocess control is required for untrusted partial media.
- Relative paths and stable IDs make reports and redux artifacts portable.
- Atomic report replacement prevents partially written survey results.
- A single-return C++ style can be preserved without weakening error handling.
