#include "torrent_downloader.hpp"
#include "media_redux.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace lt = libtorrent;

namespace
{

constexpr std::uint64_t bytes_per_megabyte = 1024ULL * 1024ULL;
constexpr std::uint64_t iso_base_media_tail_megabytes = 32ULL;

struct byte_range
{
  std::uint64_t offset{0};
  std::uint64_t size{0};
};

bool
is_iso_base_media_file(const fs::path& path)
{
  std::string extension = path.extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(),
    [](unsigned char character)
    { return static_cast<char>(std::tolower(character)); });
  const bool result = extension == ".mp4" || extension == ".m4v" ||
    extension == ".mov";
  return result;
}

std::vector<byte_range>
make_probe_ranges(const lt::torrent_info& torrent,
                  const lt::file_index_t file_index,
                  const std::uint64_t prefix_bytes,
                  const std::uint64_t tail_bytes)
{
  const auto& files = torrent.files();
  const auto file_size = static_cast<std::uint64_t>(files.file_size(file_index));
  const std::uint64_t prefix_size = std::min(prefix_bytes, file_size);
  const std::uint64_t tail_size = std::min(tail_bytes, file_size - prefix_size);
  std::vector<byte_range> result;
  if (prefix_size > 0)
    result.push_back({0, prefix_size});
  if (tail_size > 0)
    result.push_back({file_size - tail_size, tail_size});
  return result;
}

std::pair<int, int>
piece_span(const lt::torrent_info& torrent,
           const lt::file_index_t file_index,
           const byte_range& range)
{
  const auto& files = torrent.files();
  const auto first = files.map_file(
    file_index, static_cast<std::int64_t>(range.offset), 1);
  const auto last = files.map_file(
    file_index,
    static_cast<std::int64_t>(range.offset + range.size - 1),
    1);
  const std::pair<int, int> result{
    static_cast<int>(first.piece),
    static_cast<int>(last.piece)};
  return result;
}

void
prioritize_probe_ranges(lt::torrent_handle& handle,
                        const lt::torrent_info& torrent,
                        const lt::file_index_t file_index,
                        const std::uint64_t prefix_bytes,
                        const std::uint64_t tail_bytes)
{
  std::vector<lt::download_priority_t> priorities(
    static_cast<std::size_t>(torrent.num_pieces()),
    lt::download_priority_t{0});
  const std::vector<byte_range> ranges = make_probe_ranges(
    torrent, file_index, prefix_bytes, tail_bytes);
  for (const byte_range& range : ranges)
    {
      const auto [first_piece, last_piece] = piece_span(torrent, file_index, range);
      for (int piece = first_piece; piece <= last_piece; ++piece)
        priorities[static_cast<std::size_t>(piece)] = lt::download_priority_t{7};
    }
  handle.prioritize_pieces(priorities);
}

std::pair<std::size_t, std::size_t>
probe_piece_progress(const lt::torrent_handle& handle,
                     const lt::torrent_info& torrent,
                     const lt::file_index_t file_index,
                     const std::uint64_t prefix_bytes,
                     const std::uint64_t tail_bytes)
{
  const std::vector<byte_range> ranges = make_probe_ranges(
    torrent, file_index, prefix_bytes, tail_bytes);
  std::size_t verified{0};
  std::size_t required{0};
  for (const byte_range& range : ranges)
    {
      const auto [first_piece, last_piece] = piece_span(torrent, file_index, range);
      for (int piece = first_piece; piece <= last_piece; ++piece)
        {
          required++;
          if (handle.have_piece(lt::piece_index_t{piece}))
            verified++;
        }
    }
  return {verified, required};
}

bool
probe_ranges_complete(const lt::torrent_handle& handle,
                      const lt::torrent_info& torrent,
                      const lt::file_index_t file_index,
                      const std::uint64_t prefix_bytes,
                      const std::uint64_t tail_bytes)
{
  const auto [verified, required] = probe_piece_progress(
    handle, torrent, file_index, prefix_bytes, tail_bytes);
  const bool result = required > 0 && verified == required;
  return result;
}

} // namespace

/// Configure torrent session.
lt::settings_pack
make_settings_pack()
{
  using string = std::string;
  using settings_pack = lt::settings_pack;

  settings_pack settings;

  settings.set_str(settings_pack::listen_interfaces, "0.0.0.0:65505");

  string ua(LIBTORRENT_VERSION);
  settings.set_str(settings_pack::user_agent, ua);

  settings.set_bool(settings_pack::enable_lsd, true);
  settings.set_bool(settings_pack::enable_dht, true);
  settings.set_bool(settings_pack::enable_upnp, true);
  settings.set_bool(settings_pack::enable_natpmp, true);

  settings.set_bool(settings_pack::announce_to_all_tiers, true);
  settings.set_bool(settings_pack::announce_to_all_trackers, true);

  settings.set_int(settings_pack::active_limit, 8);

  settings.set_int(settings_pack::upload_rate_limit, 0);
  settings.set_int(settings_pack::download_rate_limit, 0);

  settings.set_int(settings_pack::auto_scrape_interval, 60);
  settings.set_int(settings_pack::auto_scrape_min_interval, 30);

  settings.set_int(settings_pack::max_pex_peers, 5000);

  settings.set_int(settings_pack::num_want, 1600);
  settings.set_int(settings_pack::connections_limit, 5000);

  settings.set_int(settings_pack::peer_timeout, 30);
  settings.set_int(settings_pack::inactivity_timeout, 30);

  // Disk I/O settings for libtorrent 2.0
#if 0
  settings.set_int(settings_pack::disk_io_read_mode, 0);
  settings.set_int(settings_pack::disk_io_write_mode, 0);
  settings.set_bool(settings_pack::no_atime_storage, true);
#endif

  // For downloading and trying to clip for small files, enable this. Don't do it in general.
  // Force immediate, synchronous writes to disk
  // This requires libtorrent version >= 2.0.6
  settings.set_int(settings_pack::disk_io_write_mode, settings_pack::write_through);

  // Setup alerts.
  using namespace lt::alert_category;
  auto pack_cat(error | storage | status | tracker | dht);
  settings.set_int(lt::settings_pack::alert_mask, pack_cat);

  return settings;
}


void
media_downloader::drain_alerts(lt::session& sesh)
{
  std::vector<lt::alert*> alerts;
  sesh.pop_alerts(&alerts);
  for (lt::alert* alert : alerts)
    {
      // alert_category_storage
      if (lt::alert_cast<lt::torrent_error_alert>(alert))
	std::cerr << "  [ERROR] " << alert->message() << std::endl;
    }
}

// Returns downloaded, waits for cache flushes
bool
media_downloader::drain_alerts(lt::session& sesh,
			       lt::torrent_handle& /*handle*/)
{
  // Wait for up to 1 second for a libtorrent alert
  bool ret(false);
  uint flushed(0);
  lt::alert const* a = sesh.wait_for_alert(lt::seconds(1));
  if (a != nullptr)
    {
      std::vector<lt::alert*> alerts;
      sesh.pop_alerts(&alerts);
      for (lt::alert* alert : alerts)
	{
	  if (lt::alert_cast<lt::torrent_removed_alert>(alert))
	    {
	      std::cout << "torrent removed " << std::endl;
	      break;
	    }

	  if (lt::alert_cast<lt::torrent_finished_alert>(alert))
	    {
	      std::cout << "torrent finished " << alert->message() << std::endl;
	      ret = true;
	      break;
	    }

	  if (lt::alert_cast<lt::torrent_error_alert>(alert))
	    {
	      std::cout << "torrent error: " << alert->message() << std::endl;
	      ret = true;
	      break;
	    }

	  if (lt::alert_cast<lt::cache_flushed_alert>(alert))
	    {
	      std::cout << "cache flushed ";
	      flushed++;
	    }

	  if (lt::alert_cast<lt::save_resume_data_alert>(alert))
	     {
	       std::cout << "save resume ";
	       flushed++;
	     }

	  if (lt::alert_cast<lt::save_resume_data_failed_alert>(alert))
	     {
	       std::cout << "save resume failed: " << alert->message();
	     }
	}
    }
  return ret ? true : flushed == 2;
}

/// Copy the first N bytes from source file to destination file
bool
copy_first_n_bytes(const fs::path& source, const fs::path& destination,
		   const std::int64_t num_bytes)
{
  std::ifstream src(source, std::ios::binary);
  if (!src.is_open())
    {
      std::cerr << "[ERROR] Cannot open source file: " << source << std::endl;
      return false;
    }

  // Read.
  //  std::int64_t buffer_size = 1024 * 1024; // 1MB buffers
  std::int64_t buffer_size = num_bytes;
  std::vector<char> buffer(buffer_size);
  std::int64_t remaining = num_bytes;
  std::int64_t total_copied = 0;
  while (remaining > 0)
    {
      size_t to_read = static_cast<size_t>(std::min(remaining, buffer_size));
      src.read(buffer.data(), to_read);
      std::streamsize bytes_read = src.gcount();
      if (bytes_read != 0)
	{
	  remaining -= bytes_read;
	  total_copied += bytes_read;
	}
      else
	break;
    }
  src.close();

  // Ensure data is written to disk before returning
  bool verifiedp = total_copied >= num_bytes;
  if (verifiedp)
    {
      // Write.
      std::ofstream dst(destination, std::ios::binary);
      if (!dst.is_open())
	{
	  std::cerr << "cannot create destination file: " << destination << std::endl;
	  return false;
	}
      dst.write(buffer.data(), total_copied);
      dst.flush();
      dst.close();

      // Verify.
      int fd = open(destination.string().c_str(), O_WRONLY);
      if (fd != -1)
	{
	  if (fsync(fd) == 0)
	    std::cout << "fsync() confirmed for .sized file";
	  else
	    std::cout << "fsync() failed for .sized file: " << strerror(errno);
	  std::cout << std::endl;
	  close(fd);
	}
    }
  else
    {
      std::cout << "incomplete (" << total_copied <<") copied of "
		<< num_bytes << std::endl;
    }

  return verifiedp;
}


/// @param file_path   path to the file to verify
/// @param min_size    minimum required file size (bytes)
/// @param bytes_to_check number of bytes to read from start (default 1024)
/// @return true if file exists, size >= min_size, and first bytes_to_check are non-zero
bool
verify_data_on_disk(const fs::path& file_path,
		    const std::uint64_t min_size,
		    const std::size_t bytes_to_check = 1024)
{
  bool result = false;
  std::error_code error;
  const bool regular_file = fs::is_regular_file(file_path, error);
  if (regular_file && !error)
    {
      const auto file_size = fs::file_size(file_path, error);
      if (!error && file_size >= min_size)
	{
	  std::ifstream file(file_path, std::ios::binary);
	  if (file.is_open())
	    {
	      std::vector<char> buffer(bytes_to_check);
	      file.read(buffer.data(), bytes_to_check);
	      std::streamsize bytes_read = file.gcount();
	      file.close();

	      if (bytes_read > 0)
		{
		  // Check if at least one byte is non‑zero
		  bool found_nonzero = false;
		  for (std::streamsize i = 0; i < bytes_read; ++i)
		    {
		      if (buffer[i] != 0)
			{
			  found_nonzero = true;
			  break;
			}
		    }
		  if (found_nonzero)
		    result = true;
		}
	    }
	}
    }
  return result;
}


/// Log if no peers, no downloads to file.
/// Output log is intended to be filesystem adjacent to download.cache
std::ofstream&
log_suspect(const std::string odir)
{
  const std::string fname("download.suspect-or-no-peers.log");
  const std::string ofname(odir + "/" + fname);
  const std::ios_base::openmode ofm = std::ios_base::out | std::ios_base::app;
  static std::ofstream ofsus(ofname, ofm);
  std::cout << "suspect and unreachable logging: " << ofname << std::endl;
  return ofsus;
}


/// Timeout loop for the downloader, active downloading until exit.
/// Completion is based on verified pieces in the requested head/tail ranges,
/// not aggregate torrent byte counters.
probe_wait_result
media_downloader::just_a_bit(lt::session& sesh,
                             lt::torrent_handle& handle,
                             const time_limits& tlimits,
                             const lt::torrent_info& torrent,
                             const lt::file_index_t p_index,
                             const std::uint64_t prefix_bytes,
                             const std::uint64_t tail_bytes,
                             const probe_size psize)
{
  using namespace std;

  const auto [planned_mb, target_mb] = psize;
  auto to_seconds = [](auto duration)
  { return std::chrono::duration_cast<std::chrono::seconds>(duration); };

  const auto start_time = chrono::steady_clock::now();
  auto last_status_time = start_time;
  auto last_progress_time = start_time;

  auto status = handle.status();
  std::int64_t last_payload_download = status.total_payload_download;
  auto [last_verified, required] = probe_piece_progress(
    handle, torrent, p_index, prefix_bytes, tail_bytes);

  probe_wait_result result;
  result.verified_pieces = last_verified;
  result.required_pieces = required;

  while (true)
    {
      const auto now = chrono::steady_clock::now();
      const auto elapsed = to_seconds(now - start_time).count();
      status = handle.status();

      const auto [verified, required_now] = probe_piece_progress(
        handle, torrent, p_index, prefix_bytes, tail_bytes);
      result.verified_pieces = verified;
      result.required_pieces = required_now;

      if (required_now > 0 && verified == required_now)
	{
	  result.status = probe_wait_status::complete;
	  cerr << "verified probe pieces (" << verified << "/"
	       << required_now << ")" << endl;
	  break;
	}

      if (status.errc)
	{
	  result.status = probe_wait_status::torrent_error;
	  cerr << "torrent error while waiting for probe ranges: "
	       << status.errc.message() << endl;
	  break;
	}

      const bool piece_progress = verified > last_verified;
      const bool payload_progress =
        status.total_payload_download > last_payload_download;
      if (piece_progress || payload_progress)
	{
	  last_progress_time = now;
	  last_verified = verified;
	  last_payload_download = status.total_payload_download;
	}

      if (elapsed >= tlimits.maximum)
	{
	  result.status = probe_wait_status::timed_out;
	  cerr << "timeout after " << tlimits.maximum << " seconds" << endl;
	  break;
	}

      const auto quiet_seconds = to_seconds(now - last_progress_time).count();
      if (elapsed >= tlimits.minimum &&
          quiet_seconds >= tlimits.unresponsive)
	{
	  result.status = probe_wait_status::stalled;
	  cerr << "stalled after " << quiet_seconds
	       << " seconds without probe-piece progress" << endl;
	  break;
	}

      const auto status_elapsed = to_seconds(now - last_status_time).count();
      if (status_elapsed >= 5)
	{
	  const double payload_mb = to_mb(status.total_payload_download);
	  const double rate_kbps = status.download_payload_rate / 1024.0;
	  cout << fixed << setprecision(2);
	  cout << "  Peers: " << status.num_peers
	       << " | Verified pieces: " << verified << "/" << required_now
	       << " | Payload: " << payload_mb << " MB / " << planned_mb
	       << " MB"
	       << " | Target file: " << target_mb << " MB"
	       << " | Speed: " << rate_kbps << " KB/s" << endl;
	  last_status_time = now;
	}

      if (elapsed % 30 == 0 && elapsed > 0)
	handle.force_reannounce();

      drain_alerts(sesh);
      this_thread::sleep_for(chrono::seconds(1));
    }

  return result;
}


/// Quiet session to measure progress.
void
media_downloader::is_enough(lt::session& sesh, lt::torrent_handle& handle,
			    const uint max_wait)
{
  // Pause torrent.
  // Disable auto_managed so the session doesn't restart it
  using namespace std;
  // Clear auto_managed flag (disable automatic management)
  handle.set_flags({}, lt::torrent_flags::auto_managed);
  handle.pause();

  // Wait for it to actually stop
  for (uint attempt = 0; attempt < max_wait; ++attempt)
    {
      auto status = handle.status();
      // Check paused flag via flags bitmask
      if (status.flags & lt::torrent_flags::paused)
	{
	  cout << "paused " << to_string(attempt) << endl;
	  break;
	}
      this_thread::sleep_for(chrono::seconds(1));
      drain_alerts(sesh);
    }

  // Flush
  // Request resume data (this also forces dirty blocks to disk)
  handle.flush_cache();
  handle.save_resume_data(lt::torrent_handle::save_info_dict);

  // Session shutdown handled later.
  double downloaded = handle.status().total_done;
  if (downloaded != 0)
    {
      cout << "handle tear down: ";
      bool donep(false);
      for (uint i = 0; i < max_wait && !donep; ++i)
	{
	  cout << i << ", ";
	  donep = drain_alerts(sesh, handle);
	}
      cout << endl;
    }
}


/// Download a minimum-sized chunk of the largest media file.
/// So that ffmpeg, mediainfo, and others can be used to determine the
/// frame rate, frame size, audio and subtitles.
///
/// Download the largest file in the @parm torrent_path given as an
/// argument, but stop at 10MB (or @param bytes_to_download) and
/// archive the smaller sized file.
///
/// @param timeout_seconds the number of seconds to loop while wating for data.
/// @param output_dir result files
/// @param fsuffix the suffix used on the minimal media file, default ".sized"
std::optional<fs::path>
media_downloader::almost_nothing(const std::string& ifile,
				 const std::string& output_dir,
				 const probe_size psize, const bool delete_p,
				 const std::string fsuffix)
{
  // Return sized_file_path file whenever possible, as that is the small one.
  using namespace std;
  optional<fs::path> ret(nullopt);
  auto [ min_fsize, max_dlsize ] = psize;

  fs::path prime_file_path;
  fs::path sized_file_path;
  bool prefix_verifiedp(false);
  bool redux_createdp(false);
  try
    {
      fs::create_directories(output_dir);

      auto ti = make_shared<lt::torrent_info>(ifile);
      if (ti->num_files() == 0)
	return ret;

      // Find the largest file, and prepare to download that.
      const auto& files = ti->files();
      lt::file_index_t largest_file_index{0};
      int64_t max_size = -1;
      for (int i = 0; i < ti->num_files(); ++i)
	{
	  lt::file_index_t idx{i};
	  if (files.file_size(idx) > max_size)
	    {
	      max_size = files.file_size(idx);
	      largest_file_index = idx;
	    }
	}
      const double max_mb = to_mb(max_size);
      const double target_mb = to_mb(min_fsize);
      const uint max_download_mb = to_mb(max_dlsize);

      auto target_file_path = files.file_path(largest_file_index);
      prime_file_path = fs::path(output_dir) / target_file_path;
      sized_file_path = prime_file_path.string() + fsuffix;
      fs::create_directories(prime_file_path.parent_path());

      cout << "  target file: (" << target_mb << "/" << max_mb << ")"
		<< "\t" << target_file_path << endl;

      // Set up parameters.
      lt::add_torrent_params params = { };
      params.ti = ti;
      params.save_path = output_dir;
      params.flags = lt::torrent_flags_t{};
      params.flags |= lt::torrent_flags::auto_managed;
      //params.storage_mode = lt::storage_mode_allocate;
      params.storage_mode = lt::storage_mode_sparse;

      // Set up download priorities.
      vector<lt::download_priority_t> priorities;
      priorities.resize(ti->num_files(), lt::download_priority_t{0});
      // Keep the selected file materialized on disk. Piece priorities below
      // still limit network acquisition to the requested probe ranges.
      priorities[static_cast<int>(largest_file_index)] = lt::download_priority_t{1};
      params.file_priorities = priorities;

      // Start BTIH in session...
      lt::session sesh(make_settings_pack());
      lt::torrent_handle handle = sesh.add_torrent(move(params));
      cout << "starting, waiting for metadata...";
      for (int attempt = 0; attempt < 60; ++attempt)
	{
	  if (handle.status().has_metadata)
	    break;
	  drain_alerts(sesh);
	  this_thread::sleep_for(chrono::milliseconds(500));
	}
      if (!handle.status().has_metadata)
	{
	  sesh.remove_torrent(handle);
	  return ret;
	}
      cout << "  ...metadata received." << endl;

      try
	{
	  // Loop with increasing download target sizes until prime fills in.
	  // 16, 32, 64, 128, 256, 512 (aka exponential).
	  bool serializedp(false);
	  bool stalledp(false);
	  bool exhaustedp(false);
	  for (uint index = 0;
	       index < 6 && !serializedp && !stalledp && !exhaustedp;
	       index++)
	    {
	      const uint power_of_two = 1u << index;
	      const uint dl_target_mb = std::min<uint>(
		max_download_mb, static_cast<uint>(target_mb) * power_of_two);
	      const uint tail_target_mb =
		is_iso_base_media_file(target_file_path) && max_download_mb > dl_target_mb
		? std::min<uint>(iso_base_media_tail_megabytes,
				 max_download_mb - dl_target_mb)
		: 0;
	      const uint probe_target_mb = dl_target_mb + tail_target_mb;
	      const std::uint64_t requested_prefix =
		static_cast<std::uint64_t>(dl_target_mb) * bytes_per_megabyte;
	      const std::uint64_t requested_tail =
		static_cast<std::uint64_t>(tail_target_mb) * bytes_per_megabyte;
	      prioritize_probe_ranges(handle, *ti, largest_file_index,
			      requested_prefix, requested_tail);
	      const probe_wait_result wait_result = just_a_bit(
		sesh, handle, dtlimits, *ti, largest_file_index,
		requested_prefix, requested_tail,
		{ probe_target_mb, probe_target_mb });
	      is_enough(sesh, handle);

	      // Sync a materialized file, if libtorrent created one.
	      int fd = open(prime_file_path.string().c_str(), O_RDONLY);
	      if (fd != -1)
		{
		  if (fsync(fd) != 0)
		    cerr << "fsync failed for " << prime_file_path << ": "
			 << strerror(errno) << endl;
		  close(fd);
		}

	      auto status = handle.status();
	      const bool prefix_complete = probe_ranges_complete(
		handle, *ti, largest_file_index, requested_prefix, 0);
	      prefix_verifiedp = prefix_verifiedp || prefix_complete;
	      const bool ranges_complete = probe_ranges_complete(
		handle, *ti, largest_file_index, requested_prefix, requested_tail);

	      if (ranges_complete && verify_data_on_disk(prime_file_path, min_fsize))
		{
		  media_redux_options redux_options;
		  redux_options.minimum_output_size = min_fsize;
		  const media_redux_result redux = create_media_redux(
		    prime_file_path, sized_file_path, redux_options);
		  redux_createdp = redux.success();
		  serializedp = redux_createdp;
		  if (redux_createdp)
		    cout << "redux created: " << sized_file_path << " ("
			 << to_mb(redux.output_size) << " MB)" << endl;
		  else
		    cerr << "redux attempt failed after " << probe_target_mb
			 << " MB of verified ranges: " << redux.error << endl;
		}

	      if (!serializedp)
		{
		  const bool wait_failed =
		    wait_result.status != probe_wait_status::complete;
		  if (wait_failed)
		    {
		      stalledp = true;
		      cerr << "probe range acquisition stopped with "
			   << wait_result.verified_pieces << "/"
			   << wait_result.required_pieces
			   << " pieces verified" << endl;
		    }
		  else if (dl_target_mb >= max_download_mb)
		    exhaustedp = true;
		  else
		    {
		      std::error_code file_error;
		      const auto prime_size = fs::file_size(
			prime_file_path, file_error);
		      if (file_error)
			{
			  cerr << "probe file is not available: "
			       << prime_file_path << ": "
			       << file_error.message() << endl;
			  stalledp = true;
			}
		      else
			{
			  const uint pfile_mb = to_mb(prime_size);
			  cout << "try (" << index << ", "
			       << pfile_mb << " logical MB, " << probe_target_mb
			       << " MB planned) complete" << endl;

			  // Restart torrent: set auto_managed flag and resume.
			  handle.set_flags(lt::torrent_flags::auto_managed,
					   lt::torrent_flags::auto_managed);
			  handle.resume();

			  const uint max_wait = 20;
			  for (uint attempt = 0; attempt < max_wait; ++attempt)
			    {
			      this_thread::sleep_for(chrono::seconds(1));
			      status = handle.status();
			      const bool readyp =
				!(status.flags & lt::torrent_flags::paused) &&
				status.has_metadata;
			      const bool managedp =
				(status.flags & lt::torrent_flags::auto_managed) != 0;
			      if (readyp && managedp)
				{
				  cout << "resumed (" << attempt << " sec), "
				       << "peers (" << status.num_peers << ")"
				       << endl;
				  break;
				}
			      drain_alerts(sesh);
			    }
			}
		    }
		}
	    }

	  sesh.remove_torrent(handle);
	  const uint max_wait = 10;
	  bool removedp(false);
	  for (uint i = 0; i < max_wait && !removedp; ++i)
	    {
	      cout << i << ", ";
	      removedp = drain_alerts(sesh, handle);
	    }
	  cout << endl;
	}
      catch (const std::exception& e)
	{
	  cerr << "almost_nothing: exception during download/probe lifecycle"
	       << endl << e.what() << endl;
	}

      // Initiate session shutdown.
      lt::session_proxy proxy = sesh.abort(); // just this is scope async

      // ...but force the proxy to destroy itself right here.  This line
      // blocks this background thread until shutdown is 100% finished.
      proxy = lt::session_proxy();
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      cout << "session done" << endl;
    }
  catch (const exception& e)
    {
      cerr << "almost_nothing: error, exception thrown " << e.what() << endl;
      return ret;
    }

  // Settle.
  this_thread::sleep_for(chrono::seconds(5));

  // Create sized archive file from prime download file.
  // Confirm prime_file data on disk, create sized file.
  // Check actual file on disk for non-zero data.
  // Create a specially-sized small file for the media archive.
  std::error_code prime_error;
  const bool ff_created = fs::is_regular_file(prime_file_path, prime_error);
  const auto ff_size = ff_created && !prime_error
    ? fs::file_size(prime_file_path, prime_error)
    : 0;
  if (prime_error)
    {
      cerr << "probe file is not materialized: " << prime_file_path
	   << ": " << prime_error.message() << endl;
      prime_error.clear();
    }
  const bool ff_size_targetp = ff_size >= ulong(min_fsize);
  if (!redux_createdp && ff_created && ff_size_targetp)
    {
      if (prefix_verifiedp && verify_data_on_disk(prime_file_path, min_fsize))
	{
	  if (!copy_first_n_bytes(prime_file_path, sized_file_path,
				  min_fsize))
	    cerr << "fail: sized file not copied from prime file " << endl
		 << prime_file_path.string() << endl;
	}
      else if (!prefix_verifiedp)
	cerr << "fail: no verified contiguous prefix in " << endl
	     << prime_file_path.string() << endl;
      else
	cerr << "fail: verification failed ("
	     << to_mb(ff_size) << ") in " << endl
	     << prime_file_path.string() << endl;
    }

  // Clean up
  // Remove large file and used sized file if possible.
  if (delete_p && ff_created)
    {
      error_code ec;
      if (!fs::remove(prime_file_path, ec))
	cout << "error: failed to remove file: " << ec.message() << endl;
    }

  if (verify_data_on_disk(sized_file_path, min_fsize))
    return sized_file_path;
  else
    {
      if (ff_size == 0)
	{
	  // Log in working directory.
	  fs::path ppath = fs::path(output_dir).parent_path().parent_path();
	  ofstream& ofno = log_suspect(ppath.lexically_normal().string());
	  ofno << ifile << endl;
	  ofno.flush();
	}
      return ret;
    }
}
