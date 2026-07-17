# Alpha60 Swarm Media Probe — Code Guide

This documentation describes the C++20 source in `src/`, the standalone cache survey, and the optional filename-only presumed-enrichment workflow.

![System overview](assets/system_overview.svg)

## What the program does

The program accepts a directory of `.torrent` files and produces one enriched JSON report for the collection. For each torrent it:

1. parses the torrent and computes its BitTorrent info hash;
2. reuses an existing partial-media cache entry when possible;
3. otherwise asks libtorrent for verified piece ranges from a selected media file;
4. creates a bounded redux artifact while the sparse backing file still exists, or falls back to a verified contiguous prefix;
5. runs MediaInfo and FFprobe against the cached sample;
6. converts tool output into typed C++ metadata structures; and
7. builds a collection-level JSON document with per-object metadata and pipeline metrics.

The program is deliberately a **probe**, not a complete torrent client. Its output is based on fractional `.sized` media artifacts stored beneath a BTIH-keyed cache directory.

## Documents

- [Requirements and dependencies](requirements.md) — build, runtime, optional, network, and storage requirements.
- [Architecture](architecture.md) — modules, dependency direction, and control flow.
- [Pipeline walkthrough](pipeline_walkthrough.md) — startup through output, including verified-range acquisition and failure paths.
- [Components](components.md) — classes, structs, and important free functions.
- [Media redux](media_redux.md) — bounded remux implementation, validation, and failure statuses.
- [Data model and JSON](data_model.md) — internal metadata structures and serialization boundaries.
- [Media cache survey](media_cache_survey.md) — batch validation and survey report workflow.
- [Media cache survey architecture](media_cache_survey_architecture.md) — survey subprocess and redux design.

## Source map

| Source file | Primary responsibility |
|---|---|
| `main.cpp` | CLI, orchestration, cache decisions, summaries, interruption handling |
| `torrent_parser.*` | `.torrent` discovery, decoding, BTIH computation, file inventory |
| `torrent_downloader.*` | libtorrent session setup, verified piece-range acquisition, sparse-file lifecycle |
| `media_redux.*` | bounded FFmpeg stream-copy artifact creation and FFprobe validation |
| `mediainfo_extractor.*` | MediaInfo/FFprobe execution and RapidJSON parsing |
| `json_enricher.*` | metrics calculation and final JSON construction |
| `media_cache_survey.cpp` | standalone cache inventory, structural checks, MediaInfo validation, and survey redux |

## Language and naming convention

All C++ targets use C++20 with compiler extensions disabled. Project-defined classes, structs, functions, variables, and members use `snake_case`. Existing JSON keys remain unchanged because they form an external data contract.
