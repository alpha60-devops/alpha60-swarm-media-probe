# Requirements and Dependencies

This document describes the build-time, runtime, optional, and operational dependencies used by Alpha60 Swarm Media Probe.

## Supported platform

The C++ pipeline and cache survey are Linux-only. They use POSIX APIs including `fork()`, `execvp()`, `pipe2()`, `poll()`, `waitpid()`, process groups, signals, `fsync()`, and sparse files.

A filesystem with sparse-file support is strongly recommended for `download.cache` because libtorrent creates large logical media files while only selected pieces are present on disk.

## Core build requirements

| Dependency | Requirement | Used for |
|---|---|---|
| CMake | 3.20 or newer | Configuring the C++ build |
| C++ compiler | C++20 support | Building all C++ sources |
| GNU Make or Ninja | Any CMake-supported version | Executing the generated build |
| pkg-config | Required | Locating `libtorrent-rasterbar` |
| POSIX threads | Required | libtorrent and application concurrency |
| dynamic loader library | Required on Linux | Linked as `dl` |

The project sets:

```cmake
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
```

## C++ libraries

### libtorrent-rasterbar

The production downloader uses libtorrent 2.x for:

- reading `.torrent` metadata;
- DHT, tracker, LSD, UPnP, and NAT-PMP behavior;
- sparse storage;
- file and piece priorities;
- verified-piece checks through `torrent_handle::have_piece()`;
- mapping selected file byte ranges to torrent pieces; and
- cache flush and resume-data alerts.

The downloader selects `settings_pack::write_through`, which requires libtorrent **2.0.6 or newer**.

Install both the runtime library and development headers. The pkg-config module must be named `libtorrent-rasterbar`.

### Boost

CMake requests these Boost components:

- `system`
- `filesystem`
- `thread`

They are required by the system libtorrent build and are linked into `media_enrichment`.

### OpenSSL

OpenSSL is required for SHA-1 hashing of the canonical bencoded torrent `info` dictionary. The project links the OpenSSL libraries and defines `TORRENT_USE_OPENSSL=1`.

### RapidJSON

RapidJSON is a header-only build dependency. It is used to parse:

- MediaInfo JSON;
- FFprobe JSON; and
- standalone media-cache survey results.

CMake searches for `rapidjson/document.h` under standard include locations.

## External media programs

The following executables must be available through `PATH` when CMake is configured and when the program runs.

| Program | Required by | Purpose |
|---|---|---|
| `mediainfo` | Main pipeline and cache survey | Broad container, video, audio, subtitle, and editorial metadata |
| `ffprobe` | Main extractor, redux validation, and survey | Stream probing and validation of generated artifacts |
| `ffmpeg` | Production redux and survey redux | Bounded stream-copy remuxing |

CMake resolves their absolute paths and defines:

```text
MEDIAINFO_PATH
FFMPEG_PATH
FFPROBE_PATH
```

### FFmpeg capabilities

The installed FFmpeg build must support the demuxers and muxers used by the media collection. The redux implementation explicitly supports:

- Matroska (`.mkv`);
- WebM (`.webm`);
- MP4 and M4V (`.mp4`, `.m4v`);
- QuickTime MOV (`.mov`);
- AVI (`.avi`); and
- MPEG transport streams (`.ts`, `.m2ts`).

Redux performs stream copy rather than transcoding. Codec encoders are therefore not normally required, but the relevant input demuxers, parsers, and output muxers must be enabled in the FFmpeg package.

## Fedora installation

The repository currently targets Fedora package names:

```bash
sudo dnf install -y \
  cmake \
  gcc-c++ \
  make \
  pkgconfig \
  libtorrent-rasterbar-devel \
  boost-devel \
  openssl-devel \
  rapidjson-devel \
  mediainfo \
  ffmpeg
```

On Fedora systems where FFmpeg is not available from the enabled base repositories, enable an appropriate repository that supplies the full FFmpeg CLI and libraries before installing it.

## Build

```bash
cmake -S . -B build
cmake --build build -j"$(nproc)"
```

CMake configuration fails immediately when MediaInfo, FFmpeg, FFprobe, RapidJSON, OpenSSL, Boost, pkg-config, or libtorrent-rasterbar cannot be found.

## Standalone cache survey

`src/media_cache_survey.cpp` is a Linux-only C++20 utility. It requires:

- a C++20 compiler;
- RapidJSON headers;
- `mediainfo`;
- `ffprobe`;
- `ffmpeg`; and
- Linux/POSIX process APIs.

Example:

```bash
g++ -std=c++20 -O2 -Wall -Wextra -pedantic \
  src/media_cache_survey.cpp \
  -o media_cache_survey
```

Run it with:

```bash
./media_cache_survey /path/to/torrents /path/to/download.cache --verbose
```

## Optional presumed enrichment

The filename-only presumed-enrichment path is separate from the C++ media probe. It requires:

- Python 3;
- `pip` or another Python package installer; and
- GuessIt `>=3.8,<4`.

Install into a virtual environment:

```bash
python3 -m venv .venv
source .venv/bin/activate
python -m pip install -r requirements-presumed.txt
```

The Python tests use the standard `unittest` module and do not require pytest.

## Documentation dependencies

The Markdown and SVG documentation can be read directly without additional software.

Optional documentation tooling includes:

- Doxygen, for regenerating `docs/html.doxygen/` from `docs/a60-swarm-media-probe.doxygen`;
- Jekyll and GitHub Pages, for the hosted documentation site; and
- Graphviz, when enabled by the Doxygen configuration for generated diagrams.

The checked-in documentation does not require these tools for normal program builds.

## Shell and utility dependencies

Repository scripts assume a Bourne-compatible shell, generally Bash, plus common GNU/Linux utilities such as:

- `find`, `rm`, `mkdir`, `sed`, and `grep`;
- `nproc`;
- `stat` and filesystem utilities; and
- standard process and networking tools used by diagnostic scripts.

## Network requirements

A download-enabled run requires outbound and inbound networking suitable for BitTorrent. Depending on the torrent and local network, the libtorrent session may use:

- HTTP/HTTPS and UDP trackers;
- DHT over UDP;
- peer TCP or uTP connections;
- local peer discovery;
- UPnP; and
- NAT-PMP.

The configured listen interface is `0.0.0.0:65505`. Firewalls, VPNs, carrier NAT, or blocked UDP can reduce swarm reachability without preventing analysis of existing cache files.

## Storage requirements

The default pipeline policy uses:

- a 16 MiB minimum viable cache artifact;
- up to 512 MiB of selected verified torrent ranges during acquisition;
- a 32 MiB tail request for MP4/M4V/MOV when budget permits;
- a 112 MiB redux target; and
- a 128 MiB redux hard maximum.

Temporary sparse backing files may have the logical size of the original media file even though substantially fewer blocks are physically allocated. Disk monitoring should use allocated size as well as logical size.

The production downloader deletes the temporary sparse backing media after it has created either a validated redux artifact or a verified contiguous-prefix fallback, when deletion is enabled.

## Runtime verification

Useful checks before a large run:

```bash
c++ --version
cmake --version
pkg-config --modversion libtorrent-rasterbar
mediainfo --Version
ffmpeg -version
ffprobe -version
python3 --version
```

CMake also prints the resolved paths and versions for the principal C++ and media dependencies during configuration.
