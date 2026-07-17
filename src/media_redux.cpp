#include "media_redux.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <csignal>
#include <fcntl.h>
#include <optional>
#include <poll.h>
#include <string_view>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#ifndef FFMPEG_PATH
#define FFMPEG_PATH "ffmpeg"
#endif

#ifndef FFPROBE_PATH
#define FFPROBE_PATH "ffprobe"
#endif

using namespace std::chrono_literals;

namespace
{

struct process_result
{
    bool started{false};
    bool timed_out{false};
    int exit_code{-1};
    std::string standard_output;
    std::string standard_error;
};

struct redux_format
{
    std::string muxer;
    std::string extension;
    bool faststart{false};
};

void close_descriptor(int& descriptor)
{
    if (descriptor >= 0)
    {
        close(descriptor);
        descriptor = -1;
    }
}

void read_available(int descriptor, std::string& destination, bool& open)
{
    std::array<char, 8192> buffer{};
    bool reading{true};

    while (reading && open)
    {
        const ssize_t count = read(descriptor, buffer.data(), buffer.size());
        if (count > 0)
            destination.append(buffer.data(), static_cast<std::size_t>(count));
        else if (count == 0)
            open = false;
        else if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            reading = false;
        else
            open = false;
    }
}

process_result run_process(
    const std::vector<std::string>& arguments,
    std::chrono::seconds timeout)
{
    process_result result;
    int stdout_pipe[2]{-1, -1};
    int stderr_pipe[2]{-1, -1};

    const bool pipes_ready =
        !arguments.empty() &&
        pipe2(stdout_pipe, O_CLOEXEC | O_NONBLOCK) == 0 &&
        pipe2(stderr_pipe, O_CLOEXEC | O_NONBLOCK) == 0;

    if (pipes_ready)
    {
        const auto started_at = std::chrono::steady_clock::now();
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
                argv.push_back(const_cast<char*>(argument.c_str()));
            argv.push_back(nullptr);

            execvp(argv.front(), argv.data());
            _exit(127);
        }
        else if (child > 0)
        {
            result.started = true;
            setpgid(child, child);
            close_descriptor(stdout_pipe[1]);
            close_descriptor(stderr_pipe[1]);

            bool stdout_open{true};
            bool stderr_open{true};
            bool child_running{true};
            bool termination_sent{false};
            int status{0};

            while (child_running || stdout_open || stderr_open)
            {
                if (child_running && !termination_sent &&
                    std::chrono::steady_clock::now() - started_at >= timeout)
                {
                    result.timed_out = true;
                    termination_sent = true;
                    kill(-child, SIGTERM);
                    std::this_thread::sleep_for(200ms);
                    kill(-child, SIGKILL);
                }

                std::array<pollfd, 2> descriptors{{
                    {stdout_pipe[0], static_cast<short>(stdout_open ? POLLIN | POLLHUP : 0), 0},
                    {stderr_pipe[0], static_cast<short>(stderr_open ? POLLIN | POLLHUP : 0), 0}}};

                poll(descriptors.data(), descriptors.size(), 50);

                if (stdout_open &&
                    (descriptors[0].revents & (POLLIN | POLLHUP | POLLERR)))
                    read_available(stdout_pipe[0], result.standard_output, stdout_open);
                if (stderr_open &&
                    (descriptors[1].revents & (POLLIN | POLLHUP | POLLERR)))
                    read_available(stderr_pipe[0], result.standard_error, stderr_open);

                if (child_running && waitpid(child, &status, WNOHANG) == child)
                    child_running = false;
            }

            if (WIFEXITED(status))
                result.exit_code = WEXITSTATUS(status);
            else if (WIFSIGNALED(status))
                result.exit_code = 128 + WTERMSIG(status);
        }
    }

    close_descriptor(stdout_pipe[0]);
    close_descriptor(stdout_pipe[1]);
    close_descriptor(stderr_pipe[0]);
    close_descriptor(stderr_pipe[1]);
    return result;
}

std::string lower_copy(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });
    return value;
}

std::optional<redux_format> format_for(const fs::path& input)
{
    const std::string extension = lower_copy(input.extension().string());
    std::optional<redux_format> result;

    if (extension == ".mkv")
        result = redux_format{"matroska", ".mkv", false};
    else if (extension == ".webm")
        result = redux_format{"webm", ".webm", false};
    else if (extension == ".mp4" || extension == ".m4v")
        result = redux_format{"mp4", ".mp4", true};
    else if (extension == ".mov")
        result = redux_format{"mov", ".mov", true};
    else if (extension == ".avi")
        result = redux_format{"avi", ".avi", false};
    else if (extension == ".ts" || extension == ".m2ts")
        result = redux_format{"mpegts", ".ts", false};

    return result;
}

fs::path temporary_output_path(
    const fs::path& destination,
    std::string_view media_extension)
{
    const fs::path result = destination.parent_path() /
        (destination.filename().string() + ".partial" + std::string(media_extension));
    return result;
}

std::vector<std::string> ffmpeg_arguments(
    const fs::path& input,
    const fs::path& output,
    const redux_format& format,
    std::uintmax_t target_size,
    bool include_subtitles)
{
    std::vector<std::string> result{
        FFMPEG_PATH,
        "-nostdin",
        "-hide_banner",
        "-loglevel", "error",
        "-fflags", "+genpts+discardcorrupt",
        "-err_detect", "ignore_err",
        "-probesize", "64M",
        "-analyzeduration", "20M",
        "-i", input.string(),
        "-map", "0:v:0?",
        "-map", "0:a?"};

    if (include_subtitles)
    {
        result.emplace_back("-map");
        result.emplace_back("0:s?");
    }
    else
        result.emplace_back("-sn");

    result.insert(result.end(), {
        "-dn",
        "-c", "copy",
        "-map_metadata", "0"});

    if (format.faststart)
    {
        result.emplace_back("-movflags");
        result.emplace_back("+faststart");
    }

    result.emplace_back("-fs");
    result.emplace_back(std::to_string(target_size));
    result.emplace_back("-f");
    result.emplace_back(format.muxer);
    result.emplace_back("-y");
    result.emplace_back(output.string());
    return result;
}

bool ffprobe_accepts(const fs::path& path, std::chrono::seconds timeout)
{
    const process_result probe = run_process(
        {FFPROBE_PATH,
         "-v", "error",
         "-show_entries", "stream=index",
         "-of", "csv=p=0",
         path.string()},
        timeout);

    const bool result = probe.started && !probe.timed_out &&
                        probe.exit_code == 0 && !probe.standard_output.empty();
    return result;
}

} // namespace

media_redux_result create_media_redux(
    const fs::path& input,
    const fs::path& destination,
    const media_redux_options& options)
{
    media_redux_result result;
    result.output_path = destination;

    std::error_code error;
    fs::path temporary;
    bool continue_processing{true};

    if (!fs::is_regular_file(input, error) || error)
    {
        result.status = media_redux_status::input_missing;
        result.error = error ? error.message() : "input is not a regular file";
        continue_processing = false;
    }

    const std::optional<redux_format> format = continue_processing
        ? format_for(input)
        : std::optional<redux_format>{};
    if (continue_processing && !format)
    {
        result.status = media_redux_status::unsupported_container;
        result.error = "unsupported media extension: " + input.extension().string();
        continue_processing = false;
    }

    const std::uintmax_t target_size = std::min(
        options.target_output_size,
        options.maximum_output_size);
    if (continue_processing &&
        (options.minimum_output_size == 0 ||
         options.maximum_output_size < options.minimum_output_size ||
         target_size < options.minimum_output_size))
    {
        result.status = media_redux_status::output_invalid;
        result.error = "invalid redux size options";
        continue_processing = false;
    }

    if (continue_processing && !destination.parent_path().empty())
    {
        fs::create_directories(destination.parent_path(), error);
        if (error)
        {
            result.status = media_redux_status::rename_failed;
            result.error = error.message();
            continue_processing = false;
        }
    }

    if (continue_processing)
    {
        temporary = temporary_output_path(destination, format->extension);
        bool output_created{false};
        process_result last_ffmpeg;

        for (const bool include_subtitles : {true, false})
        {
            if (!output_created)
            {
                fs::remove(temporary, error);
                error.clear();

                last_ffmpeg = run_process(
                    ffmpeg_arguments(
                        input,
                        temporary,
                        *format,
                        target_size,
                        include_subtitles),
                    options.timeout);

                result.ffmpeg_exit_code = last_ffmpeg.exit_code;
                result.ffmpeg_timed_out = last_ffmpeg.timed_out;

                output_created =
                    fs::is_regular_file(temporary, error) && !error &&
                    fs::file_size(temporary, error) > 0 && !error &&
                    ffprobe_accepts(
                        temporary,
                        std::min(options.timeout, std::chrono::seconds{30}));
            }
        }

        if (!output_created)
        {
            result.status = media_redux_status::ffmpeg_failed;
            result.error = last_ffmpeg.standard_error.empty()
                ? (last_ffmpeg.timed_out
                    ? "ffmpeg timed out"
                    : "ffmpeg did not create a parseable output")
                : last_ffmpeg.standard_error;
            continue_processing = false;
        }
    }

    if (continue_processing)
    {
        result.output_size = fs::file_size(temporary, error);
        if (error || result.output_size == 0)
        {
            result.status = media_redux_status::output_invalid;
            result.error = error ? error.message() : "redux output is empty";
            continue_processing = false;
        }
        else if (result.output_size < options.minimum_output_size)
        {
            result.status = media_redux_status::output_too_small;
            result.error = "redux output is smaller than the configured cache minimum";
            continue_processing = false;
        }
        else if (result.output_size > options.maximum_output_size)
        {
            result.status = media_redux_status::output_oversized;
            result.error = "redux output exceeds the configured archive maximum";
            continue_processing = false;
        }
    }

    if (continue_processing)
    {
        fs::remove(destination, error);
        error.clear();
        fs::rename(temporary, destination, error);
        if (error)
        {
            result.status = media_redux_status::rename_failed;
            result.error = error.message();
            continue_processing = false;
        }
    }

    if (continue_processing)
    {
        result.status = media_redux_status::succeeded;
        result.error.clear();
    }
    else if (!temporary.empty())
    {
        fs::remove(temporary, error);
    }

    return result;
}
