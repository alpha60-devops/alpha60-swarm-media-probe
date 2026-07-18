#include "mediainfo_extractor.hpp"
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <cstdlib>
#include <array>
#include <memory>
#include <sstream>
#include <iomanip>
#include <cstdio>
#include <algorithm>
#include <iostream>

// Custom deleter for FILE* using pclose
struct pclose_deleter {
    void operator()(FILE* f) const {
	if (f) pclose(f);
    }
};

namespace {

std::string shell_quote(const std::string& value)
{
    std::string quoted("'");
    for (const char c : value) {
	if (c == '\'') {
	    quoted += "'\\''";
	} else {
	    quoted += c;
	}
    }
    quoted += '\'';
    return quoted;
}

std::string escape_lavfi_option_value(const std::string& value)
{
    // The movie filename is parsed once as a filter option and again as part
    // of the complete filtergraph, so it needs two levels of escaping.
    std::string option_escaped;
    for (const char c : value) {
	if (c == '\\' || c == '\'' || c == ':' || c == ' ' || c == '\t') {
	    option_escaped += '\\';
	}
	option_escaped += c;
    }

    std::string graph_escaped;
    for (const char c : option_escaped) {
	if (c == '\\' || c == '\'' || c == '[' || c == ']' ||
	    c == ',' || c == ';' || c == ' ' || c == '\t') {
	    graph_escaped += '\\';
	}
	graph_escaped += c;
    }
    return graph_escaped;
}

} // namespace

media_info_extractor::media_info_extractor(const fs::path& media_file)
    : media_file(media_file) {}

std::optional<media_info_data> media_info_extractor::extract() {
    if (media_file.empty() || !fs::exists(media_file)) {
	std::cerr << "  [MediaInfo] File not found: " << media_file.string() << std::endl;
	return std::nullopt;
    }

    auto file_size = fs::file_size(media_file);
    std::cout << "  [MediaInfo] Analyzing: " << media_file.filename().string()
	      << " (" << file_size / (1024*1024) << " MB)" << std::endl;

    std::string json_output = exec_mediainfo();
    media_info_data data;

    if (json_output.empty() || !parse_json_output(json_output, data)) {
	std::cerr << "  [MediaInfo] Failed to parse JSON output" << std::endl;
	return std::nullopt;
    }

    // FFprobe pass for reliable language tag extraction
    std::cout << "  [FFprobe] Deducing language streams..." << std::endl;
    std::string ffprobe_output = exec_ffprobe();
    if (!ffprobe_output.empty()) {
	parse_ffprobe_output(ffprobe_output, data);
    }

    // Inspect one second of decoded video beginning ten seconds into the file.
    std::cout << "  [FFprobe] Classifying chroma..." << std::endl;
    data.video.chroma_classification = exec_chroma_classification();

    return data;
}

std::string media_info_extractor::exec_mediainfo() {
    std::string cmd = "mediainfo --Output=JSON \"" + media_file.string() + "\" 2>/dev/null";
    std::array<char, 256> buffer;
    std::string result;

    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
	std::cerr << "  [MediaInfo] Failed to execute mediainfo" << std::endl;
	return "";
    }
    std::unique_ptr<FILE, pclose_deleter> pipe_ptr(pipe);

    while (fgets(buffer.data(), buffer.size(), pipe_ptr.get()) != nullptr) {
	result += buffer.data();
    }
    return result;
}

std::string
media_info_extractor::exec_ffprobe()
{
  std::string ffcmd = "ffprobe -v quiet -print_format json -show_streams -analyzeduration 10M -probesize 10M ";
  std::string cmd = ffcmd + "\"" + media_file.string() + "\" 2>/dev/null";
  std::array<char, 256> buffer;
  std::string result;

  FILE* pipe = popen(cmd.c_str(), "r");
  if (!pipe)
    {
      std::cerr << "  [FFprobe] Failed to execute ffprobe" << std::endl;
      return result;
    }
  std::unique_ptr<FILE, pclose_deleter> pipe_ptr(pipe);

  while (fgets(buffer.data(), buffer.size(), pipe_ptr.get()) != nullptr)
    {
      result += buffer.data();
    }
  return result;
}

std::string
media_info_extractor::exec_chroma_classification(const double startsec)
{
    std::ostringstream filter;
    filter << std::fixed << std::setprecision(3)
	   << "movie=filename="
	   << escape_lavfi_option_value(media_file.string())
	   << ":seek_point=" << startsec
	   << ",trim=duration=1.000,signalstats";

    const std::string cmd =
	"ffprobe -v error -f lavfi -i " + shell_quote(filter.str()) +
	" -show_frames"
	" -show_entries frame=pts_time:frame_tags=lavfi.signalstats.SATAVG"
	" -of default=noprint_wrappers=1:nokey=0 2>/dev/null";

    std::array<char, 256> buffer;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
	std::cerr << "  [FFprobe] Failed to execute chroma classification" << std::endl;
	return "";
    }
    std::unique_ptr<FILE, pclose_deleter> pipe_ptr(pipe);

    constexpr const char* satavg_prefix =
	"TAG:lavfi.signalstats.SATAVG=";
    bool found_saturation = false;
    bool has_chroma = false;

    while (fgets(buffer.data(), buffer.size(), pipe_ptr.get()) != nullptr) {
	const std::string line(buffer.data());
	if (line.compare(0, std::char_traits<char>::length(satavg_prefix),
			 satavg_prefix) != 0) {
	    continue;
	}

	const char* value = line.c_str() +
	    std::char_traits<char>::length(satavg_prefix);
	char* end = nullptr;
	const double saturation = std::strtod(value, &end);
	if (end == value) {
	    continue;
	}

	found_saturation = true;
	if (saturation > 0.0) {
	    has_chroma = true;
	}
    }

    if (!found_saturation) {
	return "";
    }
    return has_chroma ? "polychromatic" : "achromatic";
}

bool media_info_extractor::parse_ffprobe_output(const std::string& json_output, media_info_data& data) {
    rapidjson::Document doc;
    if (doc.Parse(json_output.c_str()).HasParseError()) {
	std::cerr << "  [FFprobe] JSON parse error" << std::endl;
	return false;
    }

    if (!doc.HasMember("streams") || !doc["streams"].IsArray()) {
	return false;
    }

    const auto& streams = doc["streams"];
    std::vector<std::string> deduced_audio_langs;
    std::vector<std::string> deduced_subtitle_langs;

    for (rapidjson::SizeType i = 0; i < streams.Size(); ++i) {
	const auto& stream = streams[i];
	if (!stream.HasMember("codec_type") || !stream["codec_type"].IsString()) continue;

	std::string codec_type = stream["codec_type"].GetString();
	std::string lang = "";

	if (stream.HasMember("tags") && stream["tags"].IsObject()) {
	    const auto& tags = stream["tags"];
	    if (tags.HasMember("language") && tags["language"].IsString()) {
		lang = tags["language"].GetString();
	    }
	}

	if (!lang.empty() && lang != "und") { // Ignore 'undefined' language tags
	    if (codec_type == "audio") {
		if (std::find(deduced_audio_langs.begin(), deduced_audio_langs.end(), lang) == deduced_audio_langs.end()) {
		    deduced_audio_langs.push_back(lang);
		}
	    } else if (codec_type == "subtitle") {
		if (std::find(deduced_subtitle_langs.begin(), deduced_subtitle_langs.end(), lang) == deduced_subtitle_langs.end()) {
		    deduced_subtitle_langs.push_back(lang);
		}
	    }
	}
    }

    // Override MediaInfo data if FFprobe successfully deduced concrete tags
    if (!deduced_audio_langs.empty()) {
	data.audio.languages = deduced_audio_langs;
    }
    if (!deduced_subtitle_langs.empty()) {
	data.subtitle.languages = deduced_subtitle_langs;
    }

    return true;
}

bool media_info_extractor::parse_json_output(const std::string& json_output, media_info_data& data) {
    rapidjson::Document doc;
    if (doc.Parse(json_output.c_str()).HasParseError()) {
	std::cerr << "  [MediaInfo] JSON parse error at offset " << doc.GetErrorOffset() << std::endl;
	return false;
    }

    if (!doc.HasMember("media") || !doc["media"].IsObject()) {
	std::cerr << "  [MediaInfo] JSON missing 'media' object" << std::endl;
	return false;
    }

    const auto& media = doc["media"];
    if (!media.HasMember("track") || !media["track"].IsArray()) {
	std::cerr << "  [MediaInfo] JSON missing 'track' array" << std::endl;
	return false;
    }

    const auto& tracks = media["track"];

    // Initialize defaults
    data.file_size = 0;
    data.duration = 0.0;
    data.overall_bit_rate = 0;
    data.video.bitrate = 0;
    data.video.width = 0;
    data.video.height = 0;
    data.audio.bitrate = 0;
    data.audio.sampling_rate = 0;
    data.audio.bit_depth = 0;
    data.subtitle.format = "";

    int text_track_count = 0;

    // Parse each track
    for (rapidjson::SizeType i = 0; i < tracks.Size(); ++i) {
	const auto& track = tracks[i];
	if (!track.HasMember("@type") || !track["@type"].IsString()) continue;

	std::string track_type = track["@type"].GetString();

	if (track_type == "General") {
	    data.originating_source_medium_id = extract_string_value(track, "OriginalSourceMedium_ID");
	    data.originating_source_form = extract_string_value(track, "OriginalSourceForm");
	    data.originating_network_name = extract_string_value(track, "OriginalNetworkName");
	    data.format_commercial_if_any = extract_string_value(track, "Format_Commercial_IfAny");
	    data.file_size = extract_int64_value(track, "FileSize");
	    data.duration = extract_double_value(track, "Duration");
	    data.overall_bit_rate = extract_int64_value(track, "OverallBitRate");
	    data.domain = extract_string_value(track, "Domain");
	    data.collection = extract_string_value(track, "Collection");
	    data.season = extract_string_value(track, "Season");
	    data.distributed_by = extract_string_value(track, "DistributedBy");
	    data.genre = extract_string_value(track, "Genre");
	    data.content_type = extract_string_value(track, "ContentType");
	    data.owner = extract_string_value(track, "Owner");
	    data.country = extract_string_value(track, "Country");
	    data.comment = extract_string_value(track, "Comment");
	    data.language = extract_string_value(track, "Language");
	    data.video_creation_metadata = extract_string_value(track, "Encoded_Date");
	    if (data.video_creation_metadata.empty()) {
		data.video_creation_metadata = extract_string_value(track, "Tagged_Date");
	    }
	}
	else if (track_type == "Video") {
	    data.video.codec_id = extract_string_value(track, "CodecID");
	    data.video.codec_version = extract_string_value(track, "Encoded_Library");
	    if (data.video.codec_version.empty()) {
		data.video.codec_version = extract_string_value(track, "Encoded_Library_String");
	    }
	    data.video.color_space = extract_string_value(track, "ColorSpace");
	    data.video.bitrate = extract_int64_value(track, "BitRate");
	    data.video.width = static_cast<int>(extract_int64_value(track, "Width"));
	    data.video.height = static_cast<int>(extract_int64_value(track, "Height"));
	    data.video.frame_rate = extract_string_value(track, "FrameRate");
	    // Extract color primaries (British spelling from MediaInfo)
	    data.video.color_primaries = extract_string_value(track, "colour_primaries");
	}
	else if (track_type == "Audio") {
	    if (data.audio.codec.empty()) {
		data.audio.codec = extract_string_value(track, "CodecID");
		data.audio.codec_version = extract_string_value(track, "Format_Commercial");
		data.audio.bitrate = extract_int64_value(track, "BitRate");
		data.audio.sampling_rate = static_cast<int>(extract_int64_value(track, "SamplingRate"));
		data.audio.channels = extract_string_value(track, "Channels");
		data.audio.bit_depth = static_cast<int>(extract_int64_value(track, "BitDepth"));
	    }
	}
	else if (track_type == "Text") {
	    text_track_count++;
	    std::string lang = extract_string_value(track, "Language");
	    if (lang.empty()) {
		lang = "text #" + std::to_string(text_track_count);
	    }
	    if (std::find(data.subtitle.languages.begin(), data.subtitle.languages.end(), lang) == data.subtitle.languages.end()) {
		data.subtitle.languages.push_back(lang);
	    }
	    if (data.subtitle.format.empty()) {
		data.subtitle.format = extract_string_value(track, "Format");
	    }
	}
    }

    return true;
}

// Helper function to interpret color_primaries values for human-readable output
std::string media_info_extractor::interpret_color_primaries(const std::string& value)
{
    if (value.empty()) return "unknown";
    if (value == "BT.709") return "HD (Rec.709)";
    if (value == "BT.2020") return "UHD/HDR (Rec.2020)";
    if (value == "BT.470 M") return "Black & White / Monochrome (NTSC)";
    if (value == "BT.470 B,G") return "Black & White / Monochrome (PAL/SECAM)";
    if (value == "SMPTE 170M") return "SD (NTSC)";
    if (value == "SMPTE 240M") return "HD (SMPTE 240M)";
    if (value == "film") return "Film / Cinematic";
    if (value == "EBU Tech. 3213") return "SD (PAL)";
    return value; // Return raw value if not in known list
}

// Helper to detect black & white content
bool media_info_extractor::is_black_and_white(const std::string& color_primaries)
{
    return (color_primaries == "BT.470 M" || color_primaries == "BT.470 B,G");
}

std::string media_info_extractor::extract_string_value(const rapidjson::Value& obj, const char* key) {
    if (obj.HasMember(key) && obj[key].IsString()) {
	return obj[key].GetString();
    }
    return "";
}

std::int64_t media_info_extractor::extract_int64_value(const rapidjson::Value& obj, const char* key) {
    if (obj.HasMember(key)) {
	if (obj[key].IsInt64()) {
	    return obj[key].GetInt64();
	}
	if (obj[key].IsString()) {
	    try { return std::stoll(obj[key].GetString()); }
	    catch (...) { return 0; }
	}
    }
    return 0;
}

double media_info_extractor::extract_double_value(const rapidjson::Value& obj, const char* key) {
    if (obj.HasMember(key)) {
	if (obj[key].IsDouble()) {
	    return obj[key].GetDouble();
	}
	if (obj[key].IsString()) {
	    try { return std::stod(obj[key].GetString()); }
	    catch (...) { return 0.0; }
	}
    }
    return 0.0;
}
