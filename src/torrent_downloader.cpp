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

  // 16MB cache (16KiB blocks * 1024)
  // Set cache settings to trigger flushes with desired minimum file size.
  settings.set_int(settings_pack::cache_size, 1024);

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
media_downloader::drain_alerts(lt::session& sesh, lt::torrent_handle& handle)
{
  // Wait for up to 1 second for a libtorrent alert
  bool ret(false);
  lt::alert const* a = sesh.wait_for_alert(lt::seconds(1));
  if (a != nullptr)
    {
      std::vector<lt::alert*> alerts;
      sesh.pop_alerts(&alerts);

      for (lt::alert* alert : alerts)
	{
#if 0
	  if (auto at = lt::alert_cast<lt::add_torrent_alert>(alert))
	    handle = at->handle;
#endif

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
	      std::cout << "cache flushed " << std::endl;
	    }

	  if (lt::alert_cast<lt::save_resume_data_alert>(a))
	     {
	       std::cout << "save resume " << std::endl;
	     }
	}
    }
  return ret;
}

/// Copy the first N bytes from source file to destination file
static bool
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
static bool
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
  const std::string ofname("download.suspect-or-no-peers.log");
  const std::ios_base::openmode ofm = std::ios_base::out | std::ios_base::app;
  static std::ofstream ofsus(odir + "/" + ofname, ofm);
  return ofsus;
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
				   const std::int64_t bytes_to_download,
				   const int timeout_seconds,
				   const std::string fsuffix)
{
  // Return sized_file_path file whenever possible, as that is the small one.
  using namespace std;
  optional<fs::path> ret(nullopt);

  // Amount of time before downloading is considered futile.
  // This could be for a number of factors: no peers, private, unreachable.
  const int unresponsive_seconds(30);
  const int minimum_seconds(5);

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
      const double target_mb = to_mb(bytes_to_download);

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

      // Start timeout loop...
      // Ends if:
      /// 1: enough downloaded to make sized_file
      /// 2: timeout
      auto start_time = chrono::steady_clock::now();
      auto last_status_time = start_time;

      auto to_seconds = [](auto duration)
      { return std::chrono::duration_cast<std::chrono::seconds>(duration); };

      //// XXXX make all of this restartable if written bytes are not enough.
      //// fail: verification failed (4327) in

      int64_t last_downloaded = 0;
      auto last_rate_time = start_time;
      double current_rate_bps = 0.0;
      while (true)
	{
	  // Start clock.
	  auto now = chrono::steady_clock::now();

	  // Check for timeout.
	  uint timeout_xtra(1);
	  auto elapsed = to_seconds(now - start_time).count();
	  if (elapsed > timeout_seconds * timeout_xtra)
	    {
	      // Extend once.
	      // Heuristic to continue if probablity for completion is high.
	      bool almostp((to_mb(last_downloaded) / target_mb) >= 0.5);
	      if (almostp && timeout_xtra == 1)
		{
		  timeout_xtra = 2;
		  cout << "timeout extended" << endl;
		}
	      else
		{
		  cerr << "timeout after " << timeout_seconds << " seconds" << endl;
		  break;
		}
	    }

	  // Get status.
	  auto status = handle.status();

	  // Check if the download has reached the target size and time.
	  // status.total_payload_download
	  // status.all_time_download
	  const double tdownloaded_mb = to_mb(status.total_done);

	  // Ask the torrent handle for the download progress of ALL files
	  std::vector<std::int64_t> file_progress;
	  handle.file_progress(file_progress);
	  std::int64_t fdownloaded = file_progress[largest_file_index];
	  auto fdownloaded_mb = fdownloaded ? to_mb(fdownloaded) : 0;
	  if (elapsed >= minimum_seconds && fdownloaded_mb >= target_mb)
	    break;

	  // Check if stalled.
	  if (elapsed > unresponsive_seconds && status.download_rate == 0)
	    break;

	  // Calculate current download rate
	  double rate_elapsed = to_seconds(now - last_rate_time).count();
	  if (rate_elapsed >= 5 && status.total_payload_download > 0)
	    {
	      double delta_bytes = status.total_payload_download - last_downloaded;
	      current_rate_bps = delta_bytes / rate_elapsed;
	      last_downloaded = status.total_payload_download;
	      last_rate_time = now;
	    }

	  // Show progress every n second interval.
	  const auto status_interval = 5;
	  auto status_elapsed = to_seconds(now - last_status_time).count();
	  if (status_elapsed >= status_interval)
	    {
	      double rate_kbps = current_rate_bps / 1024.0;
	      cout << fixed << setprecision(2);
	      cout << "  Peers: " << status.num_peers
		   << " | Downloaded: " << tdownloaded_mb << " MB / "
		   << max_mb << " MB"
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

      // Tear down.
      // Pause session, flush data, remove torrent.
      try
	{
	  const uint max_wait(10);

	  // Pause torrent.
	  // Disable auto_managed so the session doesn't restart it
	  handle.auto_managed(false);
	  handle.pause();

	  // Wait for it to actually stop
	  for (int attempt = 0; attempt < max_wait; ++attempt)
	    {
	      auto status = handle.status();
	      if (status.paused)
		{
		  cout << "paused " << to_string(attempt) << endl;
		  break;
		}
	      std::this_thread::sleep_for(chrono::seconds(1));
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
	      for (uint i = 0; i < max_wait; ++i)
		{
		  cout << i << ", ";
		  drain_alerts(sesh, handle);
		}
	      cout << endl;
	    }

	  if (verify_data_on_disk(prime_file_path, bytes_to_download))
	    cout << "tear down prime file validated" << endl;

	  sesh.remove_torrent(handle);
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
  const bool ff_size_targetp = ff_size >= ulong(bytes_to_download);
  if (ff_created && ff_size_targetp)
    {
      if (verify_data_on_disk(prime_file_path, bytes_to_download))
	{
	  if (!copy_first_n_bytes(prime_file_path, sized_file_path,
				  bytes_to_download))
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

  if (verify_data_on_disk(sized_file_path, bytes_to_download))
    return sized_file_path;
  else
    {
      if (ff_size == 0)
	{
	  // Log in working directory.
	  fs::path ppath = fs::path(output_dir).parent_path();
	  ofstream& ofno = log_suspect(ppath.string());
	  ofno << ifile << endl;
	  ofno.flush();
	}
      return ret;
    }
}
