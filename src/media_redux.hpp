#ifndef MEDIA_REDUX_HPP
#define MEDIA_REDUX_HPP

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

enum class media_redux_status
{
    succeeded,
    unsupported_container,
    input_missing,
    ffmpeg_failed,
    output_invalid,
    output_too_small,
    output_oversized,
    rename_failed
};

struct media_redux_options
{
    std::uintmax_t minimum_output_size{16ULL * 1024ULL * 1024ULL};
    std::uintmax_t maximum_output_size{128ULL * 1024ULL * 1024ULL};
    std::uintmax_t target_output_size{112ULL * 1024ULL * 1024ULL};
    std::chrono::seconds timeout{120};
};

struct media_redux_result
{
    media_redux_status status{media_redux_status::ffmpeg_failed};
    fs::path output_path;
    std::uintmax_t output_size{0};
    int ffmpeg_exit_code{-1};
    bool ffmpeg_timed_out{false};
    std::string error;

    [[nodiscard]] bool success() const noexcept
    {
        return status == media_redux_status::succeeded;
    }
};

// Creates a bounded, independently parseable media artifact. The destination
// may end in ".sized"; a temporary filename with the actual media extension is
// used so FFmpeg can always select the correct muxer.
[[nodiscard]]
media_redux_result create_media_redux(
    const fs::path& input,
    const fs::path& destination,
    const media_redux_options& options = {});

#endif // MEDIA_REDUX_HPP
