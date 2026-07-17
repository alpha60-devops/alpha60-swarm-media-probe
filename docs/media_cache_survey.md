# Media Cache Survey

## Purpose

`media_cache_survey` is a Linux-only C++20 batch utility for inspecting `*.sized` artifacts produced by Alpha60 Swarm Media Probe.

It records three independent results for every sample:

1. whether lightweight structural container markers are present;
2. whether MediaInfo can extract metadata; and
3. for artifacts above the archive threshold, whether a bounded redux can be created and validated.

The output is a JSON report containing named result lists and detailed per-file observations.

## Inputs

```cpp
bool survey_media_cache(
    const fs::path& torrent_directory,
    const fs::path& download_cache_root,
    bool verbose = false);
```

- `torrent_directory` contains the collection’s `.torrent` files.
- `download_cache_root` is the root of the recursive `download.cache` hierarchy.

## Workflow

### 1. Discovery

- Enumerate `.torrent` files non-recursively.
- Enumerate every `.sized` file recursively.

### 2. Structural scan

The survey samples the beginning and end of each artifact and classifies supported container markers:

| Container | Marker |
|---|---|
| MP4 / MOV | `moov` |
| fragmented MP4 | `moof` |
| Matroska / WebM | EBML header |
| AVI | `RIFF....AVI ` header |
| MPEG-TS | leading sync byte `0x47` |

Results are written to:

```text
sized-metadata-pass
sized-metadata-fail
```

Marker results are diagnostic signals, not a substitute for MediaInfo. A partial artifact can fail the lightweight scan and still be readable by a mature demuxer.

### 3. MediaInfo validation

Every sample is executed through MediaInfo with a configurable timeout, defaulting to 20 seconds. Success requires:

- process startup;
- completion before timeout;
- exit code zero;
- parseable JSON; and
- a recognizable General track.

Results are written to:

```text
sized-metadata-mediainfo-pass
sized-metadata-mediainfo-fails
```

### 4. Survey redux

Artifacts above the default 128 MiB archive maximum are redux candidates. The current redux path:

1. maps the detected container to an explicit FFmpeg muxer;
2. requires FFprobe to demux the source;
3. estimates a target duration when duration and bitrate are available;
4. writes to `name.partial.ext`, preserving the media extension;
5. maps one optional video stream plus audio streams;
6. attempts subtitles first and retries without them;
7. stream-copies packets;
8. applies `-fs` using the archive safety-margin target;
9. applies `+faststart` for MP4/MOV or fragmentation flags for fragmented MP4;
10. validates size, structural markers, and MediaInfo output; and
11. atomically renames the candidate into the survey redux directory.

A representative command is:

```bash
ffmpeg \
  -nostdin -hide_banner -loglevel error \
  -fflags +genpts+discardcorrupt \
  -err_detect ignore_err \
  -i input.mkv \
  -map 0:v:0? -map 0:a? -map 0:s? \
  -dn -c copy \
  -fs 123480309 \
  -f matroska \
  -y output.partial.mkv
```

Survey redux results are written to:

```text
sized-metadata-mediainfo-pass-redux
sized-metadata-mediainfo-fails-redux
```

The standalone survey examines artifacts that already exist. It cannot request missing torrent pieces. Production redux is more capable because `media_downloader` performs it while verified head and tail ranges still exist in the sparse libtorrent backing file. See [Media Redux](media_redux.md).

## JSON output

The report is written under:

```text
download.cache/.survey/download-cache-survey.json
```

Redux files are stored under stable, path-derived directories beneath:

```text
download.cache/.survey/redux/
```

The report includes:

- configuration;
- discovered torrent and cache files;
- named result lists;
- source and redux sizes;
- detected containers and marker results;
- MediaInfo exit codes, timeout states, elapsed times, and diagnostics; and
- redux paths and validation state.

The report is written to a temporary file and renamed after successful serialization.

## Process control

The survey uses POSIX subprocess control rather than `std::system()`:

- separate stdout and stderr pipes;
- nonblocking reads with `poll()`;
- process groups;
- timeout detection;
- `SIGTERM` followed by `SIGKILL`; and
- child reaping through `waitpid()`.

## Build and runtime requirements

See [Requirements and Dependencies](requirements.md). The standalone binary requires C++20, RapidJSON, MediaInfo, FFmpeg, FFprobe, and Linux/POSIX process APIs.

## Known limitations

- A sparse or truncated source that FFprobe cannot demux cannot be remuxed by the survey.
- Stream copy cannot invent missing packets, indexes, or keyframes.
- The standalone survey cannot acquire additional torrent pieces.
- Some subtitle streams cannot be copied into every destination container.
- Structural marker validation remains intentionally lightweight.
