# Alpha60 Swarm Media Probe Documentation

## Intro

Welcome to the documentation for the **Swarm Media Probe**. This tool is a designed to be used with a specific workflow that takes as input a collection of hashes for media files (BitTorrent .torrent file BTIH), downloads a minimal portion of their
associated media content, extracts a minimal sample, and uses that media sample to derive technical metadata about video, audio, subtitles that are present (using ffprobe and mediainfo), and outputs a structured JSON data file.

These probe runs can build on results from previous probes,  filling in more complete data over time with longer timeouts, or requesting larger sample sizes for the existing cache. The generated cache files can be archived for future re-analysis. The workflow permits running the probe without any downloading, as an analysis-only pass for generating structured JSON.

This report file can be saved, analyzed and compared to other input collections over time.

Naturally, we call it this whole workflow by the acryonym SMiP. Use like: take a sip of the smip!

## Documentation Sections

- [API Specifications](/docs/api_specifications.md) – The schema for the final JSON output.
- [Architecture Overview](/docs/architecture_overview.md) – A high-level description of the system's components and design.
- [Pipeline Diagrams](/docs/pipeline_diagram.md) – Visual representations of the system's flow and deployment.
- Cache Survey
  - [Workflow and development summary](docs/media_cache_survey.md)
  - [Architecture](docs/media_cache_survey_architecture.md)
  - [Single-file processing sequence](docs/media_cache_survey_sequence.svg)


- [Analysis](/docs/analysis.md) – Analysis of results.
- [Sources](/docs/html.doxygen/index.html) – Source code documentation

## Quick Start (tl;dr)

#### Build the project
cd alpha60-swarm-media-probe
mkdir build && cd build
cmake .. && make -j$(nproc)

#### Run the pipeline
./media_enrichment /path/to/torrent/dir ./output.json ./cache_dir


- Minimal Downloading: Only downloads the first 10MB of the media file—enough to extract all technical metadata.
- Persistent Cache: Stores downloaded partial files to avoid re-downloading on subsequent runs.
- Comprehensive Metadata: Extracts codecs, resolution, bitrate, audio languages, and more.
- Robust API: Outputs a strictly versioned JSON schema (1.0).

## Visuals

The architecture and workflow are illustrated using Mermaid and SVG diagrams found in this directory:

- [Build and runtime flow](/docs/build-runtime-flow.svg)
- [Sequence diagram of the main pipeline](/docs/component-sequence-diagram.svg)
- [Container diagram](/docs/container-diagram.svg)
- [Core data models](/docs/data-structure-diagram.svg)
- [Deployment workflow](/docs/deployment-diagram.svg)
- [Legend](/docs/legend.svg)
