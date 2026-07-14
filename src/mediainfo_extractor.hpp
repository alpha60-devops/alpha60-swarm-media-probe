#ifndef MEDIAINFO_EXTRACTOR_HPP
#define MEDIAINFO_EXTRACTOR_HPP

#include <string>
#include <optional>
#include <vector>
#include <filesystem>
#include <rapidjson/document.h>

namespace fs = std::filesystem;

struct video_metadata
{
    std::string codec_id;
    std::string codec_version;
    std::string color_space;
    std::int64_t bitrate{0};
    int width{0};
    int height{0};
    std::string frame_rate;
    std::string color_primaries;   // BT.709, BT.2020, BT.470 M (B&W), etc.
};

struct audio_metadata
{
    std::string codec;
    std::string codec_version;
    std::int64_t bitrate{0};
    int sampling_rate{0};
    std::string channels;
    int bit_depth{0};
    std::vector<std::string> languages;
};

struct subtitle_metadata
{
    std::vector<std::string> languages;
    std::string format;
};

struct media_info_data
{
    // General metadata
    std::string originating_source_medium_id;
    std::string originating_source_form;
    std::string originating_network_name;
    std::string format_commercial_if_any;
    std::int64_t file_size{0};
    double duration{0.0};
    std::int64_t overall_bit_rate{0};
    std::string domain;
    std::string collection;
    std::string season;
    std::string distributed_by;
    std::string genre;
    std::string content_type;
    std::string owner;
    std::string country;
    std::string comment;
    std::string language;
    std::string video_creation_metadata;

    // Track metadata
    video_metadata video;
    audio_metadata audio;
    subtitle_metadata subtitle;
};

class media_info_extractor
{
public:
    explicit media_info_extractor(const fs::path& media_file);

    std::optional<media_info_data> extract();

    // Helper function to interpret color primaries for human-readable output
    static std::string interpret_color_primaries(const std::string& value);

    // Helper to detect black & white content
    static bool is_black_and_white(const std::string& color_primaries);

private:
    fs::path media_file;

    std::string exec_mediainfo();
    std::string exec_ffprobe();
    bool parse_json_output(const std::string& json_output, media_info_data& data);
    bool parse_ffprobe_output(const std::string& json_output, media_info_data& data);

    // JSON extraction helpers
    std::string extract_string_value(const rapidjson::Value& obj, const char* key);
    std::int64_t extract_int64_value(const rapidjson::Value& obj, const char* key);
    double extract_double_value(const rapidjson::Value& obj, const char* key);
};

#endif // MEDIAINFO_EXTRACTOR_HPP
