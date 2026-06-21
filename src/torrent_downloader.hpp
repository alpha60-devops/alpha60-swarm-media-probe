#ifndef TORRENT_DOWNLOADER_HPP
#define TORRENT_DOWNLOADER_HPP

#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <cerrno>

#include <atomic>
#include <string>
#include <optional>
#include <thread>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>

#include <libtorrent/session.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/write_resume_data.hpp>
#include <libtorrent/create_torrent.hpp> // for write_resume_data
#include <libtorrent/hex.hpp>            // for hex::encode
#include <libtorrent/bencode.hpp>        // for bencode

namespace fs = std::filesystem;
namespace lt = libtorrent;

/// Downloader encapsulation.
struct media_downloader
{
  // With regrets to John Pawson.
  // Download only the first 'bytes_to_download' bytes of the media file
  // Returns path to the downloaded partial file, or empty if failed.
  // 10 MB default,
  std::optional<fs::path>
  almost_nothing(const std::string& torrent_path,
		 const std::string& output_dir,
		 const std::int64_t bytes_to_download,
		 const int timeout_seconds = 300,
		 const std::string fsuffix = ".sized");

private:

  void
  drain_alerts(lt::session& sesh);

  bool
  drain_alerts(lt::session& sesh, lt::torrent_handle& handle);
};

// Convenience.
inline uint
to_mb(double d)
{ return d / (1024 * 1024); }

#endif // TORRENT_DOWNLOADER_HPP
