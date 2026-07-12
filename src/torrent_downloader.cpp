#include "torrent_downloader.hpp"

namespace lt = libtorrent;

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
  if (fs::exists(file_path))
    {
      const auto file_size = fs::file_size(file_path);
      if (file_size >= min_size)
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
/// @param psize is in mb
void
media_downloader::just_a_bit(lt::session& sesh, lt::torrent_handle& handle,
			     const time_limits& tlimits,
			     const lt::file_index_t p_index,
			     const probe_size psize)
{
  using namespace std;

  auto  [ p_mb, target_mb] = psize;

  auto to_seconds = [](auto duration)
  { return std::chrono::duration_cast<std::chrono::seconds>(duration); };

  // Start timeout loop...
  // Ends if:
  /// 1: enough downloaded to make sized_file
  /// 2: timeout
  auto start_time = chrono::steady_clock::now();
  auto last_status_time = start_time;

  int64_t last_downloaded = 0;
  auto last_rate_time = start_time;
  double current_rate_bps = 0.0;
  while (true)
    {
      // Check clock.
      auto now = chrono::steady_clock::now();
      auto elapsed = to_seconds(now - start_time).count();

      // Get status.
      auto status = handle.status();

      // Check if the download has reached the target size and time.
      // status.total_payload_download
      // status.all_time_download
      const double tdownloaded_mb = to_mb(status.total_done);

      // Ask the torrent handle for the download progress of ALL files
      std::vector<std::int64_t> file_progress;
      handle.file_progress(file_progress);
      std::int64_t fdownloaded = file_progress[p_index];
      auto fdownloaded_mb = fdownloaded ? to_mb(fdownloaded) : 0;
      if (tdownloaded_mb >= p_mb || fdownloaded_mb >= target_mb)
	{
	  cerr << "progress (" << fdownloaded_mb << ") of " << target_mb
	       << endl;
	  break;
	}

      // Check for timeout.
      if (elapsed > tlimits.maximum)
	{
	  cerr << "timeout after " << tlimits.maximum << " seconds" << endl;
	  break;
	}

      // Calculate current download rate.
      double rate_elapsed = to_seconds(now - last_rate_time).count();
      if (rate_elapsed >= 5 && status.total_done > 0)
	{
	  double delta_bytes = status.total_done - last_downloaded;
	  current_rate_bps = delta_bytes / rate_elapsed;
	  last_downloaded = status.total_done;
	  last_rate_time = now;
	}

      // Check if stalled.
      if (current_rate_bps == 0 && elapsed >= tlimits.unresponsive)
	{
	  cerr << "stalled in (" << elapsed << ") seconds" << endl;
	  break;
	}

      // Show progress every n second interval.
      const auto status_interval = 5;
      auto status_elapsed = to_seconds(now - last_status_time).count();
      if (status_elapsed >= status_interval)
	{
	  double rate_kbps = current_rate_bps / 1024.0;
	  cout << fixed << setprecision(2);
	  cout << "  Peers: " << status.num_peers
	       << " | Downloaded: " << fdownloaded_mb << ", "
	       << tdownloaded_mb << " MB / " << p_mb << " MB"
	       << " | Speed: " << rate_kbps << " KB/s";
	  cout << endl;
	  last_status_time = now;
	}

      // Force a flush periodically
      if (elapsed % 5 == 0 && elapsed > 0)
	handle.force_reannounce();

      drain_alerts(sesh);
      this_thread::sleep_for(chrono::seconds(1));
    }
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
				 const probe_size psize,
				 const std::string fsuffix)
{
  // Return sized_file_path file whenever possible, as that is the small one.
  using namespace std;
  optional<fs::path> ret(nullopt);
  auto [ min_fsize, max_dlsize ] = psize;

  fs::path prime_file_path;
  fs::path sized_file_path;
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
      priorities[static_cast<int>(largest_file_index)] = lt::download_priority_t{7};
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
	  for (uint index = 0; index < 6 && !serializedp && !stalledp; index++)
	    {
	      uint power_of_two = 1u << index;
	      uint dl_target_mb = target_mb * power_of_two;
	      just_a_bit(sesh, handle, dtlimits, largest_file_index,
			 { dl_target_mb, to_mb(max_dlsize) });
	      is_enough(sesh, handle);

	      // Sync
	      const bool forcesyncp(true);
	      if (forcesyncp)
		{
		  int fd = open(prime_file_path.string().c_str(), O_RDONLY);
		  if (fd != -1)
		    fsync(fd);
		}

	      auto status = handle.status();
	      if (verify_data_on_disk(prime_file_path, min_fsize))
		serializedp = true;
	      else
		{
		  if (status.total_done == 0 && status.download_rate == 0)
		    stalledp = true;
		  else
		    {
		      uint pfile_mb = to_mb(fs::file_size(prime_file_path));
		      cout << "try (" << index << ", "
			   << pfile_mb << " of " << dl_target_mb
			   << ") complete" << endl;

		      // Restart torrent: set auto_managed flag and resume
		      handle.set_flags(lt::torrent_flags::auto_managed, lt::torrent_flags::auto_managed);
		      handle.resume();

		      // Wait for it to actually start
		      const uint max_wait = 20;
		      for (uint attempt = 0; attempt < max_wait; ++attempt)
			{
			  this_thread::sleep_for(chrono::seconds(1));
			  status = handle.status();
			  // Check paused flag via bitmask
			  bool readyp = !(status.flags & lt::torrent_flags::paused) && status.has_metadata;
			  bool managedp = (status.flags & lt::torrent_flags::auto_managed) != 0;
			  bool peersp = status.num_peers > 0;
			  if (readyp && managedp && peersp)
			    {
			      cout << "resumed (" << attempt <<  " sec), "
				   << "peers (" << status.num_peers << ")" << endl;
			      break;
			    }
			  drain_alerts(sesh);
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
      catch (std::exception& e)
	{
	  cout << "almost_nothing:: exception thrown during tear down ";
	  cout << endl;
	  cout << e.what();
	  cout << endl;
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
  const bool ff_created = fs::exists(prime_file_path);
  const auto ff_size = ff_created ? fs::file_size(prime_file_path) : 0;
  const bool ff_size_targetp = ff_size >= ulong(min_fsize);
  if (ff_created && ff_size_targetp)
    {
      if (verify_data_on_disk(prime_file_path, min_fsize))
	{
	  if (!copy_first_n_bytes(prime_file_path, sized_file_path,
				  min_fsize))
	    cerr << "fail: sized file not copied from prime file " << endl
		 << prime_file_path.string() << endl;
	}
      else
	cerr << "fail: verification failed ("
	     << to_mb(ff_size) << ") in " << endl
	     << prime_file_path.string() << endl;
    }

  // Clean up
  // Remove large file and used sized file if possible.
  const bool cleanupp(true);
  if (cleanupp && ff_created)
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
