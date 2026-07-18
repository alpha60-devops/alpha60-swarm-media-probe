#ifndef TORRENT_DOWNLOADER_HPP
#define TORRENT_DOWNLOADER_HPP

#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <cerrno>

#include <atomic>
#include <cstdint>
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

/// Parameters used in determining how media objects in the swarm
/// probe are captured and saved to disk as fractional archival ".sized" files.
/// 1 : is the minimum file size on disk to be viable for mediainfo extraction.
/// 2 : is the maximum byte size downloaded that is necessary to produce archive
using probe_size = std::tuple<std::size_t, std::size_t>;

// Convenience.
inline uint
to_mb(double d)
{ return d / (1024 * 1024); }

/// Timeout details in seconds.
struct time_limits
{
  // Amount of time before downloading is considered futile.
  // This could be for a number of factors: no peers, private, unreachable.
  // Number of iterations/seconds to wait before zero download speed -> unreachable.
  // Most seem to start by 3-4 tries.
  uint unresponsive;

  // Minimum wait time.
  uint minimum;

  // Maximum wait time, timeout.
  uint maximum;
};

enum class probe_wait_status
{
  complete,
  stalled,
  timed_out,
  torrent_error
};

struct probe_wait_result
{
  probe_wait_status status{probe_wait_status::stalled};
  std::size_t verified_pieces{0};
  std::size_t required_pieces{0};
};

/// Default timeouts. Allow peer discovery and handshake before declaring a
/// range request stalled, while retaining a five-minute absolute limit.
constexpr time_limits dtlimits { 30, 10, 300 };


/// Validate that a cache artifact meets the minimum size and contains the
/// complete video, audio, and format metadata required by pipeline_metrics.
bool
media_cache_file_complete(const fs::path& media_file,
			  const std::size_t minimum_size,
			  std::string* reason = nullptr);


/// Downloader encapsulation.
struct media_downloader
{
  void
  is_enough(lt::session& sesh, lt::torrent_handle& handle,
	    const uint max_wait = 10);

  probe_wait_result
  just_a_bit(lt::session& sesh, lt::torrent_handle& handle,
	     const time_limits& tlimits, const lt::torrent_info& torrent,
	     const lt::file_index_t p_index,
	     const std::uint64_t prefix_bytes,
	     const std::uint64_t tail_bytes,
	     const probe_size psize);



  // With regrets to John Pawson.
  // Download only the first 'bytes_to_download' bytes of the media file
  // Returns path to the downloaded partial file, or empty if failed.
  // 10 MB default,
  std::optional<fs::path>
  almost_nothing(const std::string& torrent_path, const std::string& output_dir,
		 const probe_size psize, const bool delete_p,
		 const std::string fsuffix = ".sized");

private:

  void
  drain_alerts(lt::session& sesh);

  bool
  drain_alerts(lt::session& sesh, lt::torrent_handle& handle);
};


#endif // TORRENT_DOWNLOADER_HPP
