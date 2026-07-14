# Architecture

## Layered view

![Layered architecture](assets/layered_architecture.svg)

The program is a compact pipeline rather than a framework. `main.cpp` owns the lifecycle and calls four source-level modules in sequence.

### 1. Input and torrent model

`torrent_parser` scans the input directory for `.torrent` files. Each valid file becomes a `torrent_file` containing:

- the BTIH as a 40-character hexadecimal SHA-1 value;
- the torrent display name;
- paths and sizes of files described by the torrent;
- the aggregate payload size; and
- the local path of the `.torrent` file.

The parser uses libtorrent to decode torrent metadata, but computes the BTIH itself by bencoding the `info` dictionary and hashing those exact bytes with OpenSSL SHA-1.

### 2. Acquisition and cache

`media_downloader` wraps the libtorrent session used for probing. The surrounding orchestration creates a cache directory per torrent:

```text
<cache_dir>/<btih>/.../<media-name>.sized
```

Before contacting the swarm, `find_cache_file()` searches that directory recursively for a `.sized` file. A cached file is accepted only when it meets the configured minimum size.

When no viable cache exists, `media_downloader::almost_nothing()` configures libtorrent, selects a media file/piece range, downloads a bounded amount of data, waits for disk-related alerts, and materializes a fractional `.sized` file.

### 3. Metadata extraction

`media_info_extractor` executes two external programs:

- **MediaInfo** supplies broad container, editorial, video, audio, and subtitle metadata.
- **FFprobe** supplements or validates stream-level technical fields.

Both commands return JSON. RapidJSON parses that output into `media_info_data`, whose nested members are `video_metadata`, `audio_metadata`, and `subtitle_metadata`.

### 4. Enrichment and reporting

`enrichment` combines the parallel arrays in `process_result` with the original torrent inventory. It computes aggregate `pipeline_metrics`, serializes collection and object data, and writes the final JSON file.

## Dependency direction

```text
main.cpp
 ├── torrent_parser
 ├── media_downloader
 ├── media_info_extractor
 └── enrichment
      ├── torrent_file
      └── media_info_data
```

The leaf modules do not call back into `main.cpp`. Communication is primarily through value objects and `std::optional` results.

## Architectural characteristics

**Strengths**

- Responsibilities are separated by source file.
- Torrent, acquisition, extraction, and report models are explicit.
- Cache reuse is a first-class path rather than an afterthought.
- Failures are represented without aborting the whole collection.
- External JSON keys are isolated in the enrichment layer.

**Important coupling**

- `process_result.media_data_list` and `process_result.downloaded_files` are index-aligned.
- The downloader is coupled to libtorrent behavior and alert ordering.
- The extractor depends on the MediaInfo and FFprobe executables being present.
- `main.cpp` currently owns probe-size policy as compile-time constants.
