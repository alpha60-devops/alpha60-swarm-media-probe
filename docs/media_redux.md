# Media Redux

`media_redux` creates a bounded, independently parseable media artifact from the temporary sparse media file while the libtorrent session data still exists.

## Source files

```text
src/media_redux.hpp
src/media_redux.cpp
```

The production target compiles `media_redux.cpp` and calls it from `media_downloader::almost_nothing()`.

## Public API

```cpp
struct media_redux_options
{
    std::uintmax_t minimum_output_size{16_MiB};
    std::uintmax_t maximum_output_size{128_MiB};
    std::uintmax_t target_output_size{112_MiB};
    std::chrono::seconds timeout{120};
};

media_redux_result create_media_redux(
    const fs::path& input,
    const fs::path& destination,
    const media_redux_options& options = {});
```

The returned `media_redux_result` contains the status, destination path, generated size, FFmpeg exit code, timeout state, and diagnostic text. `success()` is true only for `media_redux_status::succeeded`.

## Supported containers

The helper chooses an explicit muxer from the source media extension:

| Input | Output muxer | Temporary suffix |
|---|---|---|
| `.mkv` | `matroska` | `.mkv` |
| `.webm` | `webm` | `.webm` |
| `.mp4`, `.m4v` | `mp4` | `.mp4` |
| `.mov` | `mov` | `.mov` |
| `.avi` | `avi` | `.avi` |
| `.ts`, `.m2ts` | `mpegts` | `.ts` |

Unsupported extensions return `unsupported_container` without starting FFmpeg.

## Temporary output naming

The public destination generally ends in `.sized`, which is not a media-container extension. Redux therefore writes to an extension-preserving temporary file such as:

```text
movie.mkv.sized.partial.mkv
```

This allows FFmpeg to infer and validate the intended output format even though an explicit muxer is also passed. The temporary file is atomically renamed to the requested `.sized` destination only after validation.

## FFmpeg strategy

Redux uses tolerant input probing and stream copy:

```text
-fflags +genpts+discardcorrupt
-err_detect ignore_err
-probesize 64M
-analyzeduration 20M
-map 0:v:0?
-map 0:a?
-map 0:s?
-dn
-c copy
-map_metadata 0
-fs <target-size>
-f <explicit-muxer>
```

For MP4 and MOV, `-movflags +faststart` moves the completed index metadata toward the beginning of the output.

The first attempt includes subtitles. If the selected subtitle streams cannot be copied into the destination container, redux retries with subtitles disabled. Video, audio, and data mappings remain bounded and explicit rather than using `-map 0`.

## Validation and commit

A candidate output is accepted only when:

1. FFmpeg created a nonempty temporary file.
2. FFprobe can enumerate at least one stream.
3. The result is at least `minimum_output_size`.
4. The result does not exceed `maximum_output_size`.
5. The final rename succeeds.

Failed temporary files are removed. Existing destinations are replaced only after a candidate has passed validation.

## Status values

| Status | Meaning |
|---|---|
| `succeeded` | A validated bounded artifact replaced the destination |
| `unsupported_container` | No explicit muxer mapping exists |
| `input_missing` | The sparse backing file is absent or not regular |
| `ffmpeg_failed` | FFmpeg timed out, failed, or produced an unparseable output |
| `output_invalid` | The size policy or generated output is invalid |
| `output_too_small` | The candidate is below the cache minimum |
| `output_oversized` | The candidate exceeds the archive maximum |
| `rename_failed` | Directory creation or final replacement failed |

## Relationship to piece acquisition

Redux is useful only when the sparse source contains complete pieces covering enough container structure and media packets. The downloader now:

1. begins with all file priorities at zero;
2. maps requested byte ranges to torrent pieces;
3. promotes only those piece spans;
4. verifies every required piece with `have_piece()`;
5. requests a growing contiguous prefix;
6. requests an additional tail for MP4/M4V/MOV when budget permits; and
7. calls redux before the sparse source is removed.

If redux cannot create a valid artifact but the minimum contiguous prefix is verified, the downloader falls back to copying that verified prefix into the `.sized` cache file.

## Limitations

Redux cannot reconstruct bytes that were never downloaded. In particular:

- MP4 index data may reference media chunks outside the acquired ranges;
- sparse holes inside a required element can make a container undemuxable;
- stream copy cannot invent keyframes or repair corrupt packets; and
- some subtitle codecs cannot be represented in every output container.

The current implementation improves the probability of a valid probe artifact; it is not a general damaged-media repair system.
