// media_cache_survey.cpp
//
// Linux-only C++20 survey utility for alpha60 download.cache artifacts.
//
// The public entry point is:
//
//     bool survey_media_cache(
//         const std::filesystem::path& torrent_directory,
//         const std::filesystem::path& download_cache_root,
//         bool verbose = false);
//
// It:
//   1. inventories *.torrent files in torrent_directory;
//   2. recursively inventories *.sized files below download_cache_root;
//   3. classifies each sized file by lightweight container metadata markers;
//   4. runs MediaInfo with a configurable timeout;
//   5. for oversized files, uses ffprobe + ffmpeg stream-copy remuxing to create
//      a bounded reduced archive and validates that archive with MediaInfo;
//   6. atomically serializes all named lists plus per-file diagnostics to JSON.
//
// External runtime requirements:
//   mediainfo, ffprobe, ffmpeg
//
// Build example:
//   g++ -std=c++20 -O2 -Wall -Wextra -pedantic media_cache_survey.cpp -o media_cache_survey
//
// This file uses RapidJSON (header‑only) for all JSON operations.

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cctype>
#include <ctime>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <functional>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <poll.h>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#include <rapidjson/document.h>
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/error/en.h>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace
{

struct survey_config
{
    std::chrono::seconds mediainfo_timeout{20};
    std::chrono::seconds ffprobe_timeout{20};
    std::chrono::seconds ffmpeg_timeout{120};
    std::uintmax_t max_archive_size{128ULL * 1024ULL * 1024ULL};
    double archive_size_margin{0.92};
    std::size_t max_redux_attempts{3};
};

struct command_result
{
    bool started{false};
    bool timed_out{false};
    bool terminated_by_signal{false};
    int exit_code{-1};
    std::chrono::milliseconds elapsed{0};
    std::string standard_output;
    std::string standard_error;
};

struct media_probe
{
    bool valid{false};
    double duration_seconds{0.0};
    double bitrate_bits_per_second{0.0};
    std::string format_name;
    command_result command;
};

struct file_observation
{
    fs::path source_path;
    fs::path relative_path;
    std::uintmax_t source_size{0};
    std::string detected_container{"unknown"};
    bool container_metadata_present{false};

    bool mediainfo_passed{false};
    bool mediainfo_timed_out{false};
    int mediainfo_exit_code{-1};
    std::chrono::milliseconds mediainfo_elapsed{0};
    std::string mediainfo_error;

    bool redux_required{false};
    bool redux_created{false};
    fs::path redux_path;
    std::uintmax_t redux_size{0};
    bool redux_container_metadata_present{false};
    bool redux_mediainfo_passed{false};
    bool redux_mediainfo_timed_out{false};
    int redux_mediainfo_exit_code{-1};
    std::string redux_error;
};

struct survey_report
{
    std::string schema{"alpha60.download-cache-survey.v1"};
    std::string started_at;
    std::string completed_at;
    fs::path torrent_directory;
    fs::path download_cache_root;
    survey_config config;
    std::vector<fs::path> torrent_files;
    std::vector<fs::path> sized_files;
    std::vector<fs::path> sized_metadata_pass;
    std::vector<fs::path> sized_metadata_fail;
    std::vector<fs::path> sized_metadata_mediainfo_pass;
    std::vector<fs::path> sized_metadata_mediainfo_fails;
    std::vector<fs::path> sized_metadata_mediainfo_pass_redux;
    std::vector<fs::path> sized_metadata_mediainfo_fails_redux;
    std::vector<file_observation> observations;
};

std::string lower_copy(std::string value)
{
    std::transform(
	value.begin(),
	value.end(),
	value.begin(),
	[](unsigned char character)
	{
	    return static_cast<char>(std::tolower(character));
	});
    return value;
}

bool has_suffix_case_insensitive(const fs::path& path, std::string_view suffix)
{
    const std::string value = lower_copy(path.filename().string());
    const std::string expected = lower_copy(std::string(suffix));
    const bool result = value.size() >= expected.size() &&
			value.compare(value.size() - expected.size(), expected.size(), expected) == 0;
    return result;
}

std::string utc_timestamp()
{
    const std::time_t now = std::time(nullptr);
    std::tm value{};
    gmtime_r(&now, &value);
    std::ostringstream stream;
    stream << std::put_time(&value, "%Y-%m-%dT%H:%M:%SZ");
    return stream.str();
}

fs::path relative_or_original(const fs::path& path, const fs::path& root)
{
    std::error_code error;
    fs::path result = fs::relative(path, root, error);
    if (error || result.empty())
    {
	result = path;
    }
    return result;
}

std::vector<fs::path> find_regular_files(
    const fs::path& root,
    std::string_view suffix,
    bool recursive)
{
    std::vector<fs::path> result;
    std::error_code error;

    if (recursive)
    {
	fs::recursive_directory_iterator iterator(
	    root,
	    fs::directory_options::skip_permission_denied,
	    error);
	const fs::recursive_directory_iterator end;

	while (!error && iterator != end)
	{
	    const fs::directory_entry& entry = *iterator;
	    std::error_code type_error;
	    if (entry.is_regular_file(type_error) &&
		!type_error &&
		has_suffix_case_insensitive(entry.path(), suffix))
	    {
		result.push_back(entry.path());
	    }
	    iterator.increment(error);
	}
    }
    else
    {
	fs::directory_iterator iterator(
	    root,
	    fs::directory_options::skip_permission_denied,
	    error);
	const fs::directory_iterator end;

	while (!error && iterator != end)
	{
	    const fs::directory_entry& entry = *iterator;
	    std::error_code type_error;
	    if (entry.is_regular_file(type_error) &&
		!type_error &&
		has_suffix_case_insensitive(entry.path(), suffix))
	    {
		result.push_back(entry.path());
	    }
	    iterator.increment(error);
	}
    }

    std::sort(result.begin(), result.end());
    return result;
}

std::vector<std::uint8_t> read_sampled_bytes(
    const fs::path& path,
    std::size_t head_size,
    std::size_t tail_size)
{
    std::vector<std::uint8_t> result;
    std::ifstream stream(path, std::ios::binary);

    if (stream)
    {
	stream.seekg(0, std::ios::end);
	const std::streamoff length = stream.tellg();
	stream.seekg(0, std::ios::beg);

	if (length > 0)
	{
	    const std::size_t total = static_cast<std::size_t>(length);
	    const std::size_t head = std::min(head_size, total);
	    result.resize(head);
	    stream.read(reinterpret_cast<char*>(result.data()), static_cast<std::streamsize>(head));
	    result.resize(static_cast<std::size_t>(stream.gcount()));

	    if (total > head && tail_size > 0)
	    {
		const std::size_t tail = std::min(tail_size, total - head);
		const std::size_t old_size = result.size();
		result.resize(old_size + tail);
		stream.clear();
		stream.seekg(static_cast<std::streamoff>(total - tail), std::ios::beg);
		stream.read(
		    reinterpret_cast<char*>(result.data() + old_size),
		    static_cast<std::streamsize>(tail));
		result.resize(old_size + static_cast<std::size_t>(stream.gcount()));
	    }
	}
    }

    return result;
}

bool contains_bytes(const std::vector<std::uint8_t>& data, std::string_view needle)
{
    bool result{false};
    if (!needle.empty() && data.size() >= needle.size())
    {
	const auto iterator = std::search(
	    data.begin(),
	    data.end(),
	    needle.begin(),
	    needle.end(),
	    [](std::uint8_t left, char right)
	    {
		return left == static_cast<std::uint8_t>(right);
	    });
	result = iterator != data.end();
    }
    return result;
}

bool has_ebml_header(const std::vector<std::uint8_t>& data)
{
    const std::array<std::uint8_t, 4> ebml{0x1A, 0x45, 0xDF, 0xA3};
    const bool result = data.size() >= ebml.size() &&
			std::equal(ebml.begin(), ebml.end(), data.begin());
    return result;
}

std::string classify_container(const fs::path& sized_path, const std::vector<std::uint8_t>& data)
{
    fs::path media_path = sized_path;
    media_path.replace_extension();
    const std::string extension = lower_copy(media_path.extension().string());
    std::string result{"unknown"};

    if (has_ebml_header(data))
    {
	result = extension == ".webm" ? "webm" : "matroska";
    }
    else if (contains_bytes(data, "ftyp") ||
	     contains_bytes(data, "moov") ||
	     contains_bytes(data, "moof"))
    {
	result = contains_bytes(data, "moof") ? "fragmented-mp4" :
		 extension == ".mov" ? "mov" : "mp4";
    }
    else if (extension == ".mkv")
    {
	result = "matroska";
    }
    else if (extension == ".webm")
    {
	result = "webm";
    }
    else if (extension == ".mp4" || extension == ".m4v")
    {
	result = "mp4";
    }
    else if (extension == ".mov")
    {
	result = "mov";
    }
    else if (extension == ".ts" || extension == ".m2ts")
    {
	result = "mpeg-ts";
    }

    return result;
}

bool has_container_metadata(const std::string& container, const std::vector<std::uint8_t>& data)
{
    bool result{false};

    if (container == "mp4" || container == "mov")
    {
	result = contains_bytes(data, "moov");
    }
    else if (container == "fragmented-mp4")
    {
	result = contains_bytes(data, "moof");
    }
    else if (container == "matroska" || container == "webm")
    {
	result = has_ebml_header(data);
    }
    else if (container == "mpeg-ts")
    {
	result = !data.empty() && data.front() == 0x47;
    }

    return result;
}

void close_fd(int& descriptor)
{
    if (descriptor >= 0)
    {
	close(descriptor);
	descriptor = -1;
    }
}

void append_pipe_data(int descriptor, std::string& destination, bool& open)
{
    std::array<char, 8192> buffer{};
    bool continue_reading{true};

    while (continue_reading && open)
    {
	const ssize_t count = read(descriptor, buffer.data(), buffer.size());
	if (count > 0)
	{
	    destination.append(buffer.data(), static_cast<std::size_t>(count));
	}
	else if (count == 0)
	{
	    open = false;
	}
	else if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
	{
	    continue_reading = false;
	}
	else
	{
	    open = false;
	}
    }
}

command_result run_command(
    const std::vector<std::string>& arguments,
    std::chrono::seconds timeout)
{
    command_result result;
    int stdout_pipe[2]{-1, -1};
    int stderr_pipe[2]{-1, -1};
    const auto start = std::chrono::steady_clock::now();

    if (!arguments.empty() &&
	pipe2(stdout_pipe, O_CLOEXEC | O_NONBLOCK) == 0 &&
	pipe2(stderr_pipe, O_CLOEXEC | O_NONBLOCK) == 0)
    {
	const pid_t child = fork();
	if (child == 0)
	{
	    setpgid(0, 0);
	    dup2(stdout_pipe[1], STDOUT_FILENO);
	    dup2(stderr_pipe[1], STDERR_FILENO);
	    close(stdout_pipe[0]);
	    close(stdout_pipe[1]);
	    close(stderr_pipe[0]);
	    close(stderr_pipe[1]);

	    std::vector<char*> argv;
	    argv.reserve(arguments.size() + 1);
	    for (const std::string& argument : arguments)
	    {
		argv.push_back(const_cast<char*>(argument.c_str()));
	    }
	    argv.push_back(nullptr);
	    execvp(argv.front(), argv.data());
	    _exit(127);
	}
	else if (child > 0)
	{
	    result.started = true;
	    setpgid(child, child);
	    close_fd(stdout_pipe[1]);
	    close_fd(stderr_pipe[1]);

	    bool stdout_open{true};
	    bool stderr_open{true};
	    bool child_running{true};
	    int status{0};

	    while (child_running || stdout_open || stderr_open)
	    {
		const auto now = std::chrono::steady_clock::now();
		if (child_running && now - start >= timeout)
		{
		    result.timed_out = true;
		    kill(-child, SIGTERM);
		    std::this_thread::sleep_for(200ms);
		    kill(-child, SIGKILL);
		}

		std::array<pollfd, 2> poll_descriptors{{
		    {stdout_pipe[0], static_cast<short>(stdout_open ? POLLIN | POLLHUP : 0), 0},
		    {stderr_pipe[0], static_cast<short>(stderr_open ? POLLIN | POLLHUP : 0), 0}}};
		poll(poll_descriptors.data(), poll_descriptors.size(), 50);

		if (stdout_open && (poll_descriptors[0].revents & (POLLIN | POLLHUP | POLLERR)))
		{
		    append_pipe_data(stdout_pipe[0], result.standard_output, stdout_open);
		}
		if (stderr_open && (poll_descriptors[1].revents & (POLLIN | POLLHUP | POLLERR)))
		{
		    append_pipe_data(stderr_pipe[0], result.standard_error, stderr_open);
		}

		if (child_running)
		{
		    const pid_t waited = waitpid(child, &status, WNOHANG);
		    if (waited == child)
		    {
			child_running = false;
		    }
		}
	    }

	    if (child_running)
	    {
		waitpid(child, &status, 0);
	    }

	    if (WIFEXITED(status))
	    {
		result.exit_code = WEXITSTATUS(status);
	    }
	    else if (WIFSIGNALED(status))
	    {
		result.terminated_by_signal = true;
		result.exit_code = 128 + WTERMSIG(status);
	    }
	}
    }

    close_fd(stdout_pipe[0]);
    close_fd(stdout_pipe[1]);
    close_fd(stderr_pipe[0]);
    close_fd(stderr_pipe[1]);
    result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
	std::chrono::steady_clock::now() - start);
    return result;
}

// -----------------------------------------------------------------------------
// MediaInfo JSON validation using RapidJSON
// -----------------------------------------------------------------------------
bool mediainfo_succeeded(const command_result& result)
{
    if (!result.started || result.timed_out || result.exit_code != 0)
	return false;

    rapidjson::Document doc;
    doc.Parse(result.standard_output.c_str());
    if (doc.HasParseError() || !doc.IsObject())
	return false;

    auto media_it = doc.FindMember("media");
    if (media_it == doc.MemberEnd() || !media_it->value.IsObject())
	return false;

    auto track_it = media_it->value.FindMember("track");
    if (track_it == media_it->value.MemberEnd() || !track_it->value.IsArray())
	return false;

    for (const auto& track : track_it->value.GetArray())
    {
	if (!track.IsObject())
	    continue;
	auto type_it = track.FindMember("@type");
	if (type_it != track.MemberEnd() && type_it->value.IsString())
	{
	    if (std::string_view(type_it->value.GetString(), type_it->value.GetStringLength()) == "General")
		return true;
	}
    }
    return false;
}

command_result run_mediainfo(const fs::path& path, const survey_config& config)
{
    const std::vector<std::string> arguments{
	"mediainfo",
	"--Output=JSON",
	"--Full",
	path.string()};
    command_result result = run_command(arguments, config.mediainfo_timeout);
    return result;
}

// -----------------------------------------------------------------------------
// FFprobe JSON parsing using RapidJSON
// -----------------------------------------------------------------------------
media_probe run_ffprobe(
    const fs::path& path,
    std::uintmax_t source_size,
    const survey_config& config)
{
    media_probe result;
    const std::vector<std::string> arguments{
	"ffprobe",
	"-v", "error",
	"-show_format",
	"-show_streams",
	"-of", "json",
	path.string()};

    result.command = run_command(arguments, config.ffprobe_timeout);

    if (!result.command.started || result.command.timed_out || result.command.exit_code != 0)
	return result;

    rapidjson::Document doc;
    doc.Parse(result.command.standard_output.c_str());
    if (doc.HasParseError() || !doc.IsObject())
	return result;

    auto format_it = doc.FindMember("format");
    if (format_it == doc.MemberEnd() || !format_it->value.IsObject())
	return result;

    const auto& format = format_it->value;

    auto duration_it = format.FindMember("duration");
    if (duration_it != format.MemberEnd() && duration_it->value.IsString())
    {
	try
	{
	    result.duration_seconds = std::stod(duration_it->value.GetString());
	}
	catch (...) {}
    }

    auto bitrate_it = format.FindMember("bit_rate");
    if (bitrate_it != format.MemberEnd() && bitrate_it->value.IsString())
    {
	try
	{
	    result.bitrate_bits_per_second = std::stod(bitrate_it->value.GetString());
	}
	catch (...) {}
    }

    auto format_name_it = format.FindMember("format_name");
    if (format_name_it != format.MemberEnd() && format_name_it->value.IsString())
    {
	result.format_name = format_name_it->value.GetString();
    }

    // If bitrate is missing, estimate from duration and file size.
    if (result.duration_seconds > 0.0 && result.bitrate_bits_per_second <= 0.0)
    {
	result.bitrate_bits_per_second = static_cast<double>(source_size) * 8.0 / result.duration_seconds;
    }

    result.valid = (result.duration_seconds > 0.0 && result.bitrate_bits_per_second > 0.0);
    return result;
}

std::string stable_path_id(const fs::path& relative_path)
{
    const std::string value = relative_path.generic_string();
    const std::uint64_t hash = static_cast<std::uint64_t>(std::hash<std::string>{}(value));
    std::ostringstream stream;
    stream << std::hex << std::setw(16) << std::setfill('0') << hash;
    return stream.str();
}

fs::path original_media_filename(const fs::path& sized_path)
{
    fs::path result = sized_path.filename();
    result.replace_extension();
    if (result.extension().empty())
    {
	result += ".bin";
    }
    return result;
}

std::vector<std::string> make_ffmpeg_arguments(
    const fs::path& input,
    const fs::path& output,
    const std::string& container,
    double target_seconds)
{
    std::ostringstream duration;
    duration << std::fixed << std::setprecision(3) << std::max(0.1, target_seconds);

    std::vector<std::string> result{
	"ffmpeg",
	"-nostdin",
	"-hide_banner",
	"-loglevel", "error",
	"-i", input.string(),
	"-map", "0",
	"-c", "copy",
	"-t", duration.str()};

    if (container == "mp4" || container == "mov")
    {
	result.emplace_back("-movflags");
	result.emplace_back("+faststart");
    }
    else if (container == "fragmented-mp4")
    {
	result.emplace_back("-movflags");
	result.emplace_back("+empty_moov+frag_keyframe+default_base_moof");
    }

    result.emplace_back("-y");
    result.emplace_back(output.string());
    return result;
}

bool container_supported_for_redux(const std::string& container)
{
    const bool result = container == "mp4" ||
			container == "mov" ||
			container == "fragmented-mp4" ||
			container == "matroska" ||
			container == "webm" ||
			container == "mpeg-ts";
    return result;
}

bool create_redux(
    file_observation& observation,
    const fs::path& download_cache_root,
    const survey_config& config,
    bool verbose)
{
    bool result{false};
    auto log = [&](const std::string& msg) { if (verbose) std::cout << msg << std::endl; };

    if (container_supported_for_redux(observation.detected_container))
    {
	log("  - Running ffprobe on " + observation.relative_path.generic_string());
	const media_probe probe = run_ffprobe(
	    observation.source_path,
	    observation.source_size,
	    config);

	if (probe.valid)
	{
	    const fs::path redux_directory =
		download_cache_root /
		".survey" /
		"redux" /
		stable_path_id(observation.relative_path);
	    std::error_code error;
	    fs::create_directories(redux_directory, error);

	    if (!error)
	    {
		observation.redux_path = redux_directory / original_media_filename(observation.source_path);
		fs::path temporary_path = observation.redux_path;
		temporary_path += ".partial";

		const double target_bytes =
		    static_cast<double>(config.max_archive_size) * config.archive_size_margin;
		double target_seconds = std::min(
		    probe.duration_seconds,
		    target_bytes * 8.0 / probe.bitrate_bits_per_second);

		for (std::size_t attempt = 0;
		     attempt < config.max_redux_attempts && !result;
		     ++attempt)
		{
		    log("  - Redux attempt " + std::to_string(attempt+1) + " for " + observation.relative_path.generic_string());
		    fs::remove(temporary_path, error);
		    error.clear();
		    const command_result ffmpeg = run_command(
			make_ffmpeg_arguments(
			    observation.source_path,
			    temporary_path,
			    observation.detected_container,
			    target_seconds),
			config.ffmpeg_timeout);

		    if (ffmpeg.started &&
			!ffmpeg.timed_out &&
			ffmpeg.exit_code == 0 &&
			fs::is_regular_file(temporary_path, error))
		    {
			const std::uintmax_t output_size = fs::file_size(temporary_path, error);
			if (!error && output_size > 0 && output_size <= config.max_archive_size)
			{
			    fs::rename(temporary_path, observation.redux_path, error);
			    if (!error)
			    {
				observation.redux_created = true;
				observation.redux_size = output_size;
				const std::vector<std::uint8_t> bytes = read_sampled_bytes(
				    observation.redux_path,
				    8ULL * 1024ULL * 1024ULL,
				    8ULL * 1024ULL * 1024ULL);
				observation.redux_container_metadata_present =
				    has_container_metadata(observation.detected_container, bytes);

				log("  - Running mediainfo on redux " + observation.redux_path.generic_string());
				const command_result mediainfo = run_mediainfo(
				    observation.redux_path,
				    config);
				observation.redux_mediainfo_passed = mediainfo_succeeded(mediainfo);
				observation.redux_mediainfo_timed_out = mediainfo.timed_out;
				observation.redux_mediainfo_exit_code = mediainfo.exit_code;
				observation.redux_error = mediainfo.standard_error;
				result = observation.redux_container_metadata_present &&
					 observation.redux_mediainfo_passed;
				if (result)
				    log("  - Redux succeeded for " + observation.relative_path.generic_string());
				else
				    log("  - Redux failed validation for " + observation.relative_path.generic_string());
			    }
			}
			else if (!error && output_size > config.max_archive_size)
			{
			    target_seconds *=
				static_cast<double>(config.max_archive_size) /
				static_cast<double>(output_size) *
				0.95;
			    log("  - Redux oversize, retrying with adjusted duration");
			}
		    }
		    else
		    {
			observation.redux_error = ffmpeg.standard_error;
			log("  - Redux ffmpeg error: " + ffmpeg.standard_error);
		    }
		}

		fs::remove(temporary_path, error);
	    }
	    else
	    {
		observation.redux_error = error.message();
		log("  - Failed to create redux directory: " + error.message());
	    }
	}
	else
	{
	    observation.redux_error = probe.command.standard_error.empty()
		? "ffprobe could not determine duration and bitrate"
		: probe.command.standard_error;
	    log("  - ffprobe failed: " + observation.redux_error);
	}
    }
    else
    {
	observation.redux_error = "container is not supported for redux remuxing";
	log("  - Container not supported for redux: " + observation.detected_container);
    }

    return result;
}

file_observation inspect_file(
    const fs::path& path,
    const fs::path& download_cache_root,
    const survey_config& config,
    bool verbose)
{
    auto log = [&](const std::string& msg) { if (verbose) std::cout << msg << std::endl; };

    file_observation result;
    result.source_path = path;
    result.relative_path = relative_or_original(path, download_cache_root);

    log("Processing " + result.relative_path.generic_string());

    std::error_code error;
    result.source_size = fs::file_size(path, error);
    if (error)
    {
	result.mediainfo_error = error.message();
	log("  - Error getting file size: " + error.message());
    }
    else
    {
	log("  - Size: " + std::to_string(result.source_size) + " bytes");
	const std::vector<std::uint8_t> bytes = read_sampled_bytes(
	    path,
	    8ULL * 1024ULL * 1024ULL,
	    8ULL * 1024ULL * 1024ULL);
	result.detected_container = classify_container(path, bytes);
	result.container_metadata_present = has_container_metadata(
	    result.detected_container,
	    bytes);
	log("  - Container: " + result.detected_container +
	    (result.container_metadata_present ? " (metadata present)" : " (metadata missing)"));

	log("  - Running mediainfo on " + path.generic_string());
	const command_result mediainfo = run_mediainfo(path, config);
	result.mediainfo_passed = mediainfo_succeeded(mediainfo);
	result.mediainfo_timed_out = mediainfo.timed_out;
	result.mediainfo_exit_code = mediainfo.exit_code;
	result.mediainfo_elapsed = mediainfo.elapsed;
	result.mediainfo_error = mediainfo.standard_error;
	log("  - mediainfo " + std::string(result.mediainfo_passed ? "passed" : "failed"));

	result.redux_required = result.source_size > config.max_archive_size;
	if (result.redux_required)
	{
	    log("  - Redux required (oversized)");
	    create_redux(result, download_cache_root, config, verbose);
	}
	else
	{
	    log("  - Redux not required (within size limit)");
	}
    }

    return result;
}

void append_named_lists(survey_report& report, const file_observation& observation)
{
    if (observation.container_metadata_present)
    {
	report.sized_metadata_pass.push_back(observation.relative_path);
    }
    else
    {
	report.sized_metadata_fail.push_back(observation.relative_path);
    }

    if (observation.mediainfo_passed)
    {
	report.sized_metadata_mediainfo_pass.push_back(observation.relative_path);
    }
    else
    {
	report.sized_metadata_mediainfo_fails.push_back(observation.relative_path);
    }

    if (observation.redux_required)
    {
	const fs::path redux_relative = observation.redux_path.empty()
	    ? observation.relative_path
	    : relative_or_original(observation.redux_path, report.download_cache_root);
	if (observation.redux_created &&
	    observation.redux_container_metadata_present &&
	    observation.redux_mediainfo_passed)
	{
	    report.sized_metadata_mediainfo_pass_redux.push_back(redux_relative);
	}
	else
	{
	    report.sized_metadata_mediainfo_fails_redux.push_back(redux_relative);
	}
    }
}

// -----------------------------------------------------------------------------
// JSON serialization with RapidJSON
// -----------------------------------------------------------------------------
bool
serialize_report(const survey_report& report, const fs::path& output_path)
{
  rapidjson::StringBuffer buffer;
  fs::path temporary_path = output_path.string() + ".tmp";

  std::error_code error;
  fs::create_directories(output_path.parent_path(), error);
  if (!error)
    {
      rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);

      writer.StartObject();

      // Schema
      writer.Key("schema");
      writer.String(report.schema.c_str());

      writer.Key("started_at");
      writer.String(report.started_at.c_str());

      writer.Key("completed_at");
      writer.String(report.completed_at.c_str());

      writer.Key("torrent_directory");
      writer.String(report.torrent_directory.generic_string().c_str());

      writer.Key("download_cache_root");
      writer.String(report.download_cache_root.generic_string().c_str());

      // Config
      writer.Key("config");
      writer.StartObject();
      writer.Key("mediainfo_timeout_seconds");
      writer.Int64(report.config.mediainfo_timeout.count());
      writer.Key("ffprobe_timeout_seconds");
      writer.Int64(report.config.ffprobe_timeout.count());
      writer.Key("ffmpeg_timeout_seconds");
      writer.Int64(report.config.ffmpeg_timeout.count());
      writer.Key("max_archive_size_bytes");
      writer.Uint64(report.config.max_archive_size);
      writer.Key("archive_size_margin");
      writer.Double(report.config.archive_size_margin);
      writer.Key("max_redux_attempts");
      writer.Uint64(static_cast<uint64_t>(report.config.max_redux_attempts));
      writer.EndObject();

      // Lists
      writer.Key("lists");
      writer.StartObject();

      auto write_path_array = [&](const char* key, const std::vector<fs::path>& paths)
      {
	writer.Key(key);
	writer.StartArray();
	for (const auto& p : paths)
	  writer.String(p.generic_string().c_str());
	writer.EndArray();
      };

      write_path_array("torrent-files", report.torrent_files);
      write_path_array("sized-files", report.sized_files);
      write_path_array("sized-metadata-pass", report.sized_metadata_pass);
      write_path_array("sized-metadata-fail", report.sized_metadata_fail);
      write_path_array("sized-metadata-mediainfo-pass", report.sized_metadata_mediainfo_pass);
      write_path_array("sized-metadata-mediainfo-fails", report.sized_metadata_mediainfo_fails);
      write_path_array("sized-metadata-mediainfo-pass-redux", report.sized_metadata_mediainfo_pass_redux);
      write_path_array("sized-metadata-mediainfo-fails-redux", report.sized_metadata_mediainfo_fails_redux);

      writer.EndObject(); // lists

      // Observations
      writer.Key("observations");
      writer.StartArray();
      for (const auto& obs : report.observations)
	{
	  writer.StartObject();

	  writer.Key("source_path");
	  writer.String(obs.relative_path.generic_string().c_str());

	  writer.Key("source_size");
	  writer.Uint64(obs.source_size);

	  writer.Key("detected_container");
	  writer.String(obs.detected_container.c_str());

	  writer.Key("container_metadata_present");
	  writer.Bool(obs.container_metadata_present);

	  writer.Key("mediainfo_passed");
	  writer.Bool(obs.mediainfo_passed);

	  writer.Key("mediainfo_timed_out");
	  writer.Bool(obs.mediainfo_timed_out);

	  writer.Key("mediainfo_exit_code");
	  writer.Int(obs.mediainfo_exit_code);

	  writer.Key("mediainfo_elapsed_ms");
	  writer.Int64(obs.mediainfo_elapsed.count());

	  writer.Key("mediainfo_error");
	  writer.String(obs.mediainfo_error.c_str());

	  writer.Key("redux_required");
	  writer.Bool(obs.redux_required);

	  writer.Key("redux_created");
	  writer.Bool(obs.redux_created);

	  writer.Key("redux_path");
	  writer.String(obs.redux_path.generic_string().c_str());

	  writer.Key("redux_size");
	  writer.Uint64(obs.redux_size);

	  writer.Key("redux_container_metadata_present");
	  writer.Bool(obs.redux_container_metadata_present);

	  writer.Key("redux_mediainfo_passed");
	  writer.Bool(obs.redux_mediainfo_passed);

	  writer.Key("redux_mediainfo_timed_out");
	  writer.Bool(obs.redux_mediainfo_timed_out);

	  writer.Key("redux_mediainfo_exit_code");
	  writer.Int(obs.redux_mediainfo_exit_code);

	  writer.Key("redux_error");
	  writer.String(obs.redux_error.c_str());

	  writer.EndObject();
	}
      writer.EndArray(); // observations
      writer.EndObject(); // root
    }

  // Write buffer to temporary file
  std::ofstream out(temporary_path, std::ios::binary | std::ios::trunc);
  if (!out)
    {
      out.write(buffer.GetString(), buffer.GetSize());
      out.flush();
      out.close();
      if (out.good())
	fs::rename(temporary_path, output_path, error);
    }

  return out.good() && !error;
}

class download_cache_survey
{
public:
    explicit download_cache_survey(survey_config config = {},
				   bool verbose = false)
    : config(std::move(config)), verbose_(verbose) { }

    bool
    run(const fs::path& torrent_directory, const fs::path& download_cache_root)
    {
      auto log = [&](const std::string& msg) { if (verbose_) std::cout << msg << std::endl; };

      bool result{false};
      std::error_code error;
      const bool valid_directories =
	fs::is_directory(torrent_directory, error) &&
	!error &&
	fs::is_directory(download_cache_root, error) &&
	!error;

      if (!valid_directories)
	{
	  log("ERROR: One or both directories are invalid.");
	  return false;
	}

      log("Starting survey at " + utc_timestamp());
      log("Torrent directory: " + torrent_directory.generic_string());
      log("Download cache root: " + download_cache_root.generic_string());

      survey_report report;
      report.started_at = utc_timestamp();
      report.torrent_directory = fs::absolute(torrent_directory, error);
      if (error)
	{
	  report.torrent_directory = torrent_directory;
	  error.clear();
	}
      report.download_cache_root = fs::absolute(download_cache_root, error);
      if (error)
	{
	  report.download_cache_root = download_cache_root;
	  error.clear();
	}
      report.config = config;

      log("Scanning torrent directory for .torrent files...");
      report.torrent_files = find_regular_files(report.torrent_directory,
						".torrent", false);
      for (fs::path& path : report.torrent_files)
	{
	  path = relative_or_original(path, report.torrent_directory);
	}
      log("Found " + std::to_string(report.torrent_files.size()) + " torrent files.");

      log("Scanning download cache for .sized files...");
      const std::vector<fs::path> sized_absolute = find_regular_files(
								      report.download_cache_root,
	    ".sized",
	    true);
      report.sized_files.reserve(sized_absolute.size());
      report.observations.reserve(sized_absolute.size());

      log("Found " + std::to_string(sized_absolute.size()) + " sized files.");

      for (const fs::path& path : sized_absolute)
	{
	  report.sized_files.push_back(relative_or_original(path, report.download_cache_root));
	  file_observation observation = inspect_file(
						      path,
						      report.download_cache_root,
						      config,
						      verbose_);
	  append_named_lists(report, observation);
	  report.observations.push_back(std::move(observation));
	}

      report.completed_at = utc_timestamp();
      const fs::path output_path = report.download_cache_root / ".survey" / "download-cache-survey.json";
      log("Writing report to " + output_path.generic_string());
      result = serialize_report(report, output_path);

      if (result)
	log("Survey completed successfully at " + report.completed_at);
      else
	log("ERROR: Failed to write report.");

      return result;
    }

private:
  survey_config config;
  bool verbose_;
};

} // namespace

bool
survey_media_cache(const fs::path& torrent_directory,
		   const fs::path& download_cache_root, bool verbose)
{
  download_cache_survey survey({}, verbose);
  const bool result = survey.run(torrent_directory, download_cache_root);
  return result;
}



int main(int argc, char** argv)
{
  bool verbose = false;
  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i)
    {
      std::string arg = argv[i];
      if (arg == "--verbose")
	{
	  verbose = true;
	}
      else
	{
	  args.push_back(arg);
	}
    }

  if (args.size() == 2)
    {
      bool success = survey_media_cache(args[0], args[1], verbose);
      return success ? 0 : 1;
    }
  else
    {
      std::cerr
	<< "usage: media_cache_survey [torrent-directory] [download.cache] [--verbose]";
      return 1;
    }
}
