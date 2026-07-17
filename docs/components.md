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

Directory-level errors throw from `parse_all_torrents()`. Per-file parse errors become `std::nullopt`, allowing the batch to continue.

---

## `media_downloader`

### Public API

```cpp
void is_enough(...);
void just_a_bit(...);
std::optional<fs::path> almost_nothing(...);
```

The class coordinates selective torrent acquisition. `probe_size` contains the minimum viable cache size and the maximum download budget.

### Verified-range helpers

The downloader implements a byte-range planner around these internal concepts:

- `byte_range` — offset and length within the selected torrent file;
- `make_probe_ranges()` — creates a prefix plus an optional ISO base-media tail;
- `piece_span()` — maps a file-relative range to first and last torrent piece indexes;
- `prioritize_probe_ranges()` — sets all piece priorities to zero, then promotes only required spans; and
- `probe_ranges_complete()` — requires `have_piece()` for every piece in every planned range.

MP4, M4V, and MOV receive an optional tail request of up to 32 MiB because the `moov` atom may be stored near the end of the file.

### Supporting functions

- `copy_first_n_bytes()` creates the fallback fractional file only after the prefix has been verified.
- `verify_data_on_disk()` checks size and ensures the beginning is not entirely zero bytes.
- `log_suspect()` records unreachable or no-peer cases.
- overloaded `drain_alerts()` methods consume libtorrent events and wait for flush conditions.
- `is_enough()` pauses the torrent and requests cache flush plus resume data.

`almost_nothing()` invokes `create_media_redux()` before removing the sparse backing file. A failed redux can fall back to a verified contiguous prefix; an unverified sparse prefix is rejected.

---

## `media_redux`

### Public API

```cpp
enum class media_redux_status;

struct media_redux_options
{
    std::uintmax_t minimum_output_size;
    std::uintmax_t maximum_output_size;
    std::uintmax_t target_output_size;
    std::chrono::seconds timeout;
};

struct media_redux_result
{
    media_redux_status status;
    fs::path output_path;
    std::uintmax_t output_size;
    int ffmpeg_exit_code;
    bool ffmpeg_timed_out;
    std::string error;

    [[nodiscard]] bool success() const noexcept;
};

media_redux_result create_media_redux(
    const fs::path& input,
    const fs::path& destination,
    const media_redux_options& options = {});
```

### Internal workflow

- `format_for()` maps supported source extensions to explicit FFmpeg muxers.
- `temporary_output_path()` keeps a real media extension after `.partial`.
- `run_process()` executes FFmpeg and FFprobe in a process group with timeout and captured output.
- `ffmpeg_arguments()` selects video/audio/subtitle streams and imposes a target byte limit.
- `ffprobe_accepts()` requires the candidate to expose at least one stream.
- the first attempt includes subtitles; the second disables them.
- size validation and rename occur only after FFprobe acceptance.

See [Media Redux](media_redux.md) for the full behavior and status table.

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

- `exec_mediainfo()` invokes MediaInfo and captures stdout.
- `exec_ffprobe()` invokes FFprobe and captures stdout.
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

- `compute_pipeline_metrics()` derives collection statistics from cache artifacts and extracted metadata.
- `json_string()` and `escape_json_string()` protect serialized string content.

The serializer is handwritten rather than generated from the C++ structs. Existing JSON key spellings therefore remain independent from C++ identifier changes.

---

## Orchestration structures

### `download_result`

Carries the selected or cached media path, a success flag, and an error string.

### `extract_result`

Carries `media_info_data`, a success flag, and an error string.

### `process_result`

Carries collection-wide aligned result arrays and counters:

```cpp
std::vector<media_info_data> media_data_list;
std::vector<fs::path> downloaded_files;
std::size_t success_count;
std::size_t get_fail;
std::size_t extract_fail;
```

### `pipeline_metrics`

Captures total torrent slots, configured cache sample size, unreachable, partial, and extracted counts, and extraction percentage.
