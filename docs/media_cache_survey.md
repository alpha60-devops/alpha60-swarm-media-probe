# Media Cache Survey

## Purpose

`media_cache_survey` is a Linux-only C++20 batch survey utility for
inspecting cached media sample (`*.sized`) files produced by the Alpha60
swarm media probe.

The survey is designed to answer three independent questions for every
sample:

1.  Does the container appear structurally valid?
2.  Can MediaInfo successfully extract metadata?
3.  If the sample is very large, can it be reduced to a bounded archive
    while preserving enough structure for metadata extraction?

The output is a JSON report that records both high-level lists and
detailed per-file observations.

------------------------------------------------------------------------

# Inputs

``` cpp
bool run(
    const fs::path& torrent_directory,
    const fs::path& download_cache_root);
```

-   `torrent_directory`
    -   Directory containing the original `.torrent` files.
-   `download_cache_root`
    -   Root of the `download.cache` hierarchy containing `.sized`
        files.

------------------------------------------------------------------------

# Workflow

## Phase 1 --- Discover Inputs

-   Enumerate `.torrent` files.
-   Recursively enumerate every `.sized` file beneath `download.cache`.

------------------------------------------------------------------------

## Phase 2 --- Container Survey

Each sample is scanned without decoding media.

Supported container markers include:

-   MP4 / MOV (`moov`)
-   Fragmented MP4 (`moof`)
-   Matroska / WebM (EBML header)

Results:

-   `sized-metadata-pass`
-   `sized-metadata-fail`

------------------------------------------------------------------------

## Phase 3 --- MediaInfo Validation

Every sample is processed using MediaInfo.

A configurable timeout (default 20 seconds) prevents hangs.

Success requires:

-   process exit code == 0
-   valid JSON output
-   recognizable media tracks

Results:

-   `sized-metadata-mediainfo-pass`
-   `sized-metadata-mediainfo-fails`

------------------------------------------------------------------------

## Phase 4 --- Archive Reduction

Samples larger than the configurable maximum (default 128 MiB) are
remuxed using FFmpeg.

The implementation estimates a target duration using `ffprobe` bitrate
information.

Typical commands:

### MP4 / MOV

``` bash
ffmpeg -i input.mp4 \
    -map 0 \
    -c copy \
    -t TARGET_SECONDS \
    -movflags +faststart \
    output.mp4
```

### MKV

``` bash
ffmpeg -i input.mkv \
    -map 0 \
    -c copy \
    -t TARGET_SECONDS \
    output.mkv
```

If the reduced archive passes MediaInfo it is added to

-   `sized-metadata-mediainfo-pass-redux`

------------------------------------------------------------------------

# JSON Output

The survey serializes one report similar to:

    download.cache/
        .survey/
            download-cache-survey.json

The report contains

-   configuration
-   discovered files
-   named result lists
-   detailed observations
-   timing information
-   failure diagnostics

------------------------------------------------------------------------

# Design Decisions

## Independent State

Container validity, MediaInfo success, and archive reduction are
intentionally treated as separate observations.

For example, a file may fail the lightweight container scan but still be
readable by MediaInfo.

## Linux-only

The implementation intentionally avoids Win32 support.

Subprocesses use POSIX process management.

## One Return Per Function

The implementation follows the project coding style of a single return
statement per C++ function.

## Atomic Output

Reports are written to a temporary file and renamed into place after
success.

------------------------------------------------------------------------

# Relationship to Earlier Development

This component builds directly on previous work completed for the
project.

Earlier conversations established:

-   Linux-only implementation
-   C++20 coding style
-   single-return functions
-   lightweight container detection
-   removal of MediaInfo dependency for the initial container survey
-   FFmpeg/ffprobe-based inspection
-   snake_case naming conventions
-   JSON-compatible reporting

The cache survey extends those ideas into a complete validation
pipeline.

------------------------------------------------------------------------

# Future Improvements

Potential future enhancements include:

-   libmediainfo integration instead of subprocess execution
-   direct libavformat probing for additional containers
-   parallel worker pools with configurable limits
-   resumable surveys
-   SQLite result database
-   GuessIt filename enrichment correlation
-   automatic comparison against previous swarm probe results
