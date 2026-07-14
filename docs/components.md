# Components

## `torrent_parser`

### Public API

```cpp
explicit torrent_parser(const fs::path& idir);
std::vector<torrent_file> parse_all_torrents();
std::optional<torrent_file> parse_single_torrent(const fs::path& torrent_file);
```

### Internal helpers

- `read_torrent_file()` reads raw bytes so the info hash is based on the original torrent data.
- `compute_info_hash()` hashes the canonical bencoded `info` dictionary.

### Error behavior

Directory-level errors throw from `parse_all_torrents()`. Per-file parse errors are caught and converted to `std::nullopt`, allowing the batch to continue.

---

## `media_downloader`

### Public API

```cpp
void is_enough(...);
void just_a_bit(...);
std::optional<fs::path> almost_nothing(...);
```

The class coordinates selective torrent acquisition. `probe_size` is a tuple:

```cpp
using probe_size = std::tuple<std::size_t, std::size_t>;
```

Its values represent the minimum viable cached file size and the maximum amount to download.

### Supporting types and functions

- `time_limits` defines unresponsive, minimum, and maximum waits.
- `dtlimits` supplies default timeout values.
- `copy_first_n_bytes()` creates the final fractional file.
- `verify_data_on_disk()` checks size and ensures the beginning is not all zero bytes.
- `log_suspect()` appends unreachable/no-peer cases to an adjacent log.
- overloaded `drain_alerts()` methods consume libtorrent events and wait for completion/flush conditions.

---

## `media_info_extractor`

### Public API

```cpp
explicit media_info_extractor(const fs::path& media_file);
std::optional<media_info_data> extract();
static std::string interpret_color_primaries(const std::string& value);
static bool is_black_and_white(const std::string& color_primaries);
```

### Internal workflow

- `exec_mediainfo()` invokes MediaInfo and captures stdout through a `FILE*` managed by `pclose_deleter`.
- `exec_ffprobe()` does the same for FFprobe.
- `parse_json_output()` maps MediaInfo JSON to the typed model.
- `parse_ffprobe_output()` maps FFprobe JSON and enriches technical fields.
- extraction helpers normalize string, integer, and floating-point access from RapidJSON values.

---

## `enrichment`

### Public API

```cpp
std::string build_output(...);
bool write_output(const std::string& output_path,
                  const std::string& json_content);
```

### Private responsibilities

- `compute_pipeline_metrics()` derives collection statistics from cache files and extracted metadata.
- `json_string()` and `escape_json_string()` protect serialized string content.

The serializer is handwritten rather than generated from the C++ structs. This makes the existing JSON key spellings explicit and keeps them independent from C++ identifier renames.

---

## Orchestration structures

### `download_result`

Carries the selected/cached media path, a success flag, and an error string.

### `extract_result`

Carries `media_info_data`, a success flag, and an error string.

### `process_result`

Carries collection-wide aligned result arrays and counters:

```cpp
std::vector<media_info_data> media_data_list;
std::vector<fs::path> downloaded_files;
size_t success_count;
size_t get_fail;
size_t extract_fail;
```

### `pipeline_metrics`

Captures total torrent slots, configured cache sample size, unreachable/partial/extracted counts, and extraction percentage.
