# Pipeline Walkthrough

![Pipeline sequence](assets/pipeline_sequence.svg)

## Startup

`main()` installs handlers for `SIGINT` and `SIGTERM`. The handler sets the atomic `g_interrupted` flag, allowing the collection loop to stop between torrents.

The CLI accepts:

```text
program <input_directory> [output_file] [cache_dir]
```

Defaults are:

- output: `media_objects_medium_info.json`
- cache: `download.cache`

The program validates the input directory and creates missing output/cache directories.

## Step 1: parse torrents

`parse_torrents()` constructs `torrent_parser` and calls `parse_all_torrents()`.

For each `.torrent` file:

1. `parse_single_torrent()` opens it through `libtorrent::torrent_info`.
2. `read_torrent_file()` loads the original bencoded bytes.
3. `compute_info_hash()` decodes the root dictionary, extracts `info`, re-bencodes it, and calculates SHA-1.
4. The parser records every payload file path and size.

Invalid torrents return `std::nullopt` and are omitted from the result vector.

## Step 2: process each torrent

`process_all_torrents()` preserves one output slot per input torrent.

![Cache decision tree](assets/cache_decision.svg)

### Cache hit

`find_cache_file()` locates the first regular file whose extension is `.sized`. When its size is at least `min_fsize`, the pipeline skips the network and proceeds directly to extraction.

### Cache miss with downloads disabled

The pipeline appends an empty `media_info_data`, appends an empty download path, increments `get_fail`, and continues.

### Cache miss with downloads enabled

`download_torrent_media()` creates `<cache_dir>/<btih>`, constructs `media_downloader`, and calls `almost_nothing()` using the configured `(minimum_file_size, maximum_download_size)` tuple.

The current values in `main.cpp` are:

```text
minimum viable sample: 16 MiB
maximum requested data: 512 MiB
```

A successful acquisition returns the `.sized` path. A failed acquisition records placeholders and increments `get_fail`.

## Metadata extraction

`extract_media_info()` constructs `media_info_extractor` for the cached path and calls `extract()`.

The extractor:

1. executes MediaInfo in JSON mode;
2. parses general and track metadata;
3. executes FFprobe in JSON mode;
4. merges useful stream fields; and
5. returns `std::optional<media_info_data>`.

On extraction failure, the orchestration removes the bad cached path, appends an empty metadata record, and increments `extract_fail`. This prevents a corrupt cache entry from being reused indefinitely.

## Step 3: build and write JSON

`write_enriched_output()` creates `enrichment`, then calls:

```cpp
build_output(torrents, process_result, collection_key,
             min_fsize, cache_dir_size_mb, torrent_total_size_mb)
```

`compute_pipeline_metrics()` classifies each torrent/cache slot as unreachable, partial, or extracted. The resulting string is passed to `write_output()`.

## Completion and interruption

A normal run prints totals for:

- torrents discovered;
- successful extractions;
- acquisition failures; and
- extraction failures.

If interrupted, the loop stops and `main()` exits with status `130` before writing a potentially misleading final report.
