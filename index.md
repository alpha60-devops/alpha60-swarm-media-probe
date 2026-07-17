# Alpha60 Swarm Media Probe Documentation

## Introduction

Alpha60 Swarm Media Probe, or SMiP, accepts collections of BitTorrent `.torrent` files, acquires small verified ranges from selected media objects, creates bounded cache artifacts, extracts technical media metadata, and writes a structured JSON report.

Probe runs can reuse previous cache artifacts, run without networking, or expand acquisition budgets over time. The workflow is intended for collection-level media research rather than complete torrent downloads.

## Documentation

- [Code guide](docs/README.md)
- [Requirements and dependencies](docs/requirements.md)
- [Architecture](docs/architecture.md)
- [Pipeline walkthrough](docs/pipeline_walkthrough.md)
- [Components](docs/components.md)
- [Media redux](docs/media_redux.md)
- [Data model and JSON](docs/data_model.md)
- [Media cache survey](docs/media_cache_survey.md)
- [Media cache survey architecture](docs/media_cache_survey_architecture.md)
- [Generated source documentation](docs/html.doxygen/index.html)

## Quick start

```bash
cmake -S . -B build
cmake --build build -j"$(nproc)"

./build/media_enrichment \
  /path/to/torrent/directory \
  ./media_objects_medium_info.json \
  ./download.cache
```

The production build requires C++20, libtorrent-rasterbar 2.x, Boost, OpenSSL, RapidJSON, MediaInfo, FFmpeg, and FFprobe. See the requirements document for package and operational details.

## Cache behavior

The downloader requests verified piece spans rather than accepting aggregate sparse-file progress. It grows a contiguous prefix and, for MP4/M4V/MOV, can also request a tail range containing end-positioned container metadata.

While the sparse source still exists, `media_redux` attempts to create a bounded, parseable `.sized` artifact. If redux cannot succeed, the downloader can retain a verified contiguous-prefix fallback. Existing cache artifacts can be audited with the standalone media-cache survey.

## Visual documentation

The repository includes SVG diagrams and Doxygen-generated source pages under `docs/`.
