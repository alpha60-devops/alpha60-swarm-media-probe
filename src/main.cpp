#include "torrent_parser.hpp"
#include "torrent_downloader.hpp"
#include "mediainfo_extractor.hpp"
#include "json_enricher.hpp"
#include <iostream>
#include <vector>
#include <filesystem>
#include <atomic>
#include <csignal>
#include <iomanip>

namespace fs = std::filesystem;
using namespace std;

// Global flag for interrupt handling
atomic<bool> g_interrupted{false};

static void
signal_handler(int /*sig*/)
{
  cout << "\n! Interrupted by user. Cleaning up..." << endl;
  g_interrupted = true;
}

static void
print_usage(const char* prog_name)
{
  cout << "Usage: " << prog_name << " <input_directory> [output_file] [cache_dir]" << endl;
  cout << endl;
  cout << "  input_directory: Directory containing .torrent files" << endl;
  cout << "  output_file: Optional output JSON path (default: output/media_objects_content_analysis.json)" << endl;
  cout << "  cache_dir: Optional directory for temporary downloads (default: ./download.cache)" << endl;
  cout << endl;
  cout << "Example:" << endl;
  cout << "  " << prog_name << " /path/to/collection-torrents-dir ./enriched.json ./cache" << endl;
}


/// Helper to get collection_key.
/// NB this is the filesystem key id used for all Alpha60 workflows.
/// Usually this would be computed per the member a60::collection.collection_key
/// @param input_dir is the input directory containing torrent files
/// Use an operational trick, where the input data director is collection_key.
std::string
get_collection_key(const fs::path& input_dir)
{ return input_dir.filename().string(); }


/// Find size of cache directory contents.
double
get_directory_size_mb(const fs::path& path)
{
  if (!fs::exists(path))
    throw std::runtime_error("Path does not exist: " + path.string());

  // Resolve top-level symlink if present
  fs::path target = fs::is_symlink(path) ? fs::canonical(path) : path;
  if (!fs::is_directory(target))
    throw std::runtime_error("Path is not a directory: " + target.string());

  // Recursively iterate, following directory symlinks
  std::uintmax_t total_bytes = 0;
  std::unordered_set<fs::path> unique_files;  // stores canonical paths
  auto fsopts = fs::directory_options::follow_directory_symlink;
  for (const auto& entry : fs::recursive_directory_iterator(target, fsopts))
    {
      // Check actual file type, not symlink
      if (!fs::is_regular_file(entry.status()))
	continue;

      fs::path canonical_path;
      try
	{
	  // Resolve any symlink to the real file
	  canonical_path = fs::canonical(entry.path());
	}
      catch (const fs::filesystem_error&)
	{
	  // If canonical fails (e.g., broken symlink), skip this entry
	  continue;
	}

      // Only count the file once, even if multiple links point to it
      if (unique_files.insert(canonical_path).second)
	{
	  total_bytes += fs::file_size(canonical_path);
	}
    }

  return to_mb(total_bytes);
}


/// Helper to calculate total size of all torrents' complete media files
uintmax_t
get_collection_size_mb(const std::vector<torrent_file>& torrents)
{
  uintmax_t total_bytes = 0;
  for (const auto& tf : torrents)
    total_bytes += tf.total_size;
  return to_mb(total_bytes);
}


/// Parse torrents from input directory
vector<torrent_file>
parse_torrents(const fs::path& input_dir)
{
  cout << "\n[1/3] Parsing torrent files..." << endl;
  torrent_parser parser(input_dir);
  auto torrents = parser.parse_all_torrents();

  if (torrents.empty()) {
    cerr << "Error: No .torrent files found in " << input_dir << endl;
  } else {
    cout << "Found " << torrents.size() << " torrent file(s)" << endl;
  }

  return torrents;
}


/// Download media sample for a single torrent
struct download_result
{
  fs::path	media_path;
  bool		success;
  string	error_msg;
};


fs::path
find_cache_file(const fs::path& tdir)
{
  // Check if we already have a cached download file.  Cached media
  // file name is the name of the alpha file in the torrent +
  // "*.sized" Recursively iterate over all entries
  fs::create_directories(tdir);

  fs::path ret = { };
  for (const auto& entry : fs::recursive_directory_iterator(tdir))
    {
      try
	{
	  // Skip non‑regular files (directories, symlinks, etc.)
	  if (!fs::is_regular_file(entry.path()))
	    continue;

	  // Check if the file name ends with ".sized"
	  if (entry.path().extension() == ".sized")
	    {
	      ret = entry.path();
	      break;
	    }
	}
      catch (const fs::filesystem_error& e)
	{
	  // Skip entries we can't read (e.g., permission denied)
	  continue;
	}
    }
  return ret;
}

/// Toplevel download function.
download_result
download_torrent_media(const torrent_file& tf, const fs::path& cache_dir,
		       probe_size psize)
{
  // Create unique subdirectory for this torrent using its BTIH, if it
  // doesn't exist already.
  download_result ret = { };
  fs::path torrent_cache_dir = cache_dir / tf.btih;
  ret.media_path = find_cache_file(torrent_cache_dir);

  // Use cache if it meets the minimum file size specified.
  auto [ min_fsize, max_dlsize ] = psize;
  if (fs::exists(ret.media_path) && fs::file_size(ret.media_path) >= min_fsize)
    {
      cout << "    Using cached download: " << ret.media_path << endl;
      ret.success = true;
    }
  else
    {
      // Download minimal media file
      cout << "    Downloading first " << to_mb(max_dlsize) << "MB..." << endl;
      media_downloader downloader;
      auto media_path = downloader.almost_nothing(tf.torrent_path.string(),
						  torrent_cache_dir.string(),
						  psize);
      if (media_path.has_value())
	{
	  ret.media_path = media_path.value();
	  ret.success = true;
	}
    }

  return ret;
}


/// Extract media info from downloaded file
struct extract_result
{
  media_info_data data;
  bool success;
  string error_msg;
};


/// Toplevel extract function.
extract_result
extract_media_info(const fs::path& media_path)
{
  media_info_extractor extractor(media_path);
  auto media_data = extractor.extract();

  extract_result result = { };
  if (media_data.has_value())
    {
      result.data = media_data.value();
      result.success = true;

      // Print brief summary
      const auto& md = result.data;
      cout << "    ✓ Codec: " << (md.video.codec_id.empty() ? "unknown" : md.video.codec_id);
      if (md.video.width > 0 && md.video.height > 0) {
	cout << ", Resolution: " << md.video.width << "x" << md.video.height;
      }
      if (!md.video.frame_rate.empty()) {
	cout << ", FPS: " << md.video.frame_rate;
      }
      cout << endl;
    }
  return result;
}


/// Loop per element of btiha.
process_result
process_all_torrents(const vector<torrent_file>& torrents,
		     const fs::path& cache_dir,
		     const probe_size psize, const bool download_p)
{
  cout << "\n[2/3] " << (download_p ? "Downloading" : "Using cached")
       << " media ..." << endl;

  auto [ min_fsize, max_dlsize ] = psize;
  process_result result = { };
  for (size_t i = 0; i < torrents.size() && !g_interrupted; ++i)
    {
      const auto& tf = torrents[i];
      cout << "[" << (i+1) << "/" << torrents.size() << "] " << tf.name << endl;

      fs::path torrent_cache_dir = cache_dir / tf.btih;
      fs::path cached_file = find_cache_file(torrent_cache_dir);

      bool cache_existsp = fs::exists(cached_file) && fs::file_size(cached_file) >= min_fsize;
      if (cache_existsp)
	{
	  cout << "    Using cached download: " << cached_file << endl;
	  result.downloaded_files.push_back(cached_file);
	}
      else
	{
	  // No cache found, no download == done.
	  if (!download_p)
	    {
	      cerr << "    ✗ No cache found and download disabled. Skipping." << endl;
	      result.media_data_list.push_back(media_info_data());
	      result.downloaded_files.push_back("");
	      result.get_fail++;
	      continue;
	    }

	  // Download
	  download_result dlr = download_torrent_media(tf, cache_dir, psize);
	  if (!dlr.success)
	    {
	      cerr << "    ✗ " << dlr.error_msg << endl;
	      result.media_data_list.push_back(media_info_data());
	      result.downloaded_files.push_back("");
	      result.get_fail++;
	      continue;
	    }
	  cached_file = dlr.media_path;
	  result.downloaded_files.push_back(cached_file);
	  cout << "    ✓ Downloaded to: " << cached_file << endl;
	}


      cout << "    Extracting metadata..." << endl;
      auto extract_result = extract_media_info(cached_file);
      cout << endl;

      if (extract_result.success)
	{
	  result.media_data_list.push_back(extract_result.data);
	  result.success_count++;
	}
      else
	{
	  cerr << "failed to extract cached metadata file, removing: "
	       << extract_result.error_msg << endl;

	  // Clean up.
	  // Remove the file/directory (recursively for directories).
	  // Returns the number of items removed, but we ignore the count.
	  std::error_code ec;
	  fs::remove_all(cached_file, ec);
	  if (ec)
	    {
	      // Something went wrong – permission denied, invalid path, etc.
	      cerr << "failed to remove cache file: " << ec.value() << endl;
	    }

	  result.media_data_list.push_back(media_info_data());
	  result.extract_fail++;
	}
      //      cout << endl;
    }
  cout << endl;

  return result;
}


/// Write enriched JSON output
bool
write_enriched_output(const fs::path& output_file,
		      const vector<torrent_file>& torrents,
		      const process_result& presult,
		      const std::string& collection_key,
		      size_t min_fsize,
		      uintmax_t cache_dir_size_mb,
		      uintmax_t torrent_total_size_mb)
{
  cout << "\n[3/3] Building enriched JSON..." << endl;

  enrichment nrichr;
  string jdata = nrichr.build_output(torrents, presult, collection_key, min_fsize,
				     cache_dir_size_mb, torrent_total_size_mb);
  if (!nrichr.write_output(output_file.string(), jdata))
    {
      cerr << "✗ Error: Failed to write output file" << endl;
      return false;
    }

  cout << "Wrote enriched JSON to: " << output_file << endl;

  if (fs::exists(output_file))
    {
      auto size = fs::file_size(output_file);
      cout << "  Output size: " << fixed << setprecision(2)
	   << to_mb(size) << " MB" << endl;
    }

  return true;
}


/// Summarize.
void
print_summary(size_t total_torrents, const process_result& process_result)
{
  cout << "\n========================================" << endl;
  cout << "  Pipeline Summary" << endl;
  cout << "========================================" << endl;
  cout << "  Total torrents:         " << total_torrents << endl;
  cout << "  Successfully processed: " << process_result.success_count << endl;
  cout << "  Get Failed:             " << process_result.get_fail << endl;
  cout << "  Extract Failed:         " << process_result.extract_fail << endl;
  cout << "========================================" << endl;

  if (g_interrupted) {
    cout << "  Note: Interrupted by user" << endl;
  }
}


int main(int argc, char* argv[])
{
  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);

  if (argc < 2) {
    print_usage(argv[0]);
    return 1;
  }

  // Parse command line arguments
  fs::path input_dir = argv[1];
  fs::path output_file = (argc >= 3) ? argv[2] : "media_objects_medium_info.json";
  fs::path cache_dir = (argc >= 4) ? argv[3] : "download.cache";

  // Validate input directory
  if (!fs::exists(input_dir) || !fs::is_directory(input_dir)) {
    cerr << "Error: Input directory does not exist: " << input_dir << endl;
    return 1;
  }

  // Create output and cache directories
  if (output_file.has_parent_path()) {
    fs::create_directories(output_file.parent_path());
  }
  fs::create_directories(cache_dir);

  // Print banner
  cout << "========================================" << endl;
  cout << "  Media Enrichment Pipeline v1.0" << endl;
  cout << "========================================" << endl;
  cout << "Input directory:  " << input_dir << endl;
  cout << "Output file:      " << output_file << endl;
  cout << "Cache directory:  " << cache_dir << endl;
  cout << "========================================" << endl;

  // Step 1: Parse torrents
  auto torrents = parse_torrents(input_dir);
  if (torrents.empty()) {
    return 1;
  }

  // Get collection key from first JSON file in input directory
  std::string collection_key = get_collection_key(input_dir);
  cout << "Collection key:    " << collection_key << endl;

  // Get sizes for metrics
  double cache_dir_size_mb = get_directory_size_mb(cache_dir);
  double torrent_total_size_mb = get_collection_size_mb(torrents);

  const size_t max_fsize = 512 * 1024 * 1024;  // 128 MB
  //const size_t max_fsize = 128 * 1024 * 1024;  // 128 MB
  //const size_t max_fsize = 64 * 1024 * 1024;  // 64 MB
  //const size_t max_fsize = 32 * 1024 * 1024;  // 32 MB


  const size_t min_fsize = 16 * 1024 * 1024;  // 16 MB
  //const size_t min_fsize = 10 * 1024 * 1024;  // 10 MB
  //const size_t min_fsize = 0; // only useful for cached only


  // Step 2: Process all torrents (download + extract)
  bool download_p = true;
  probe_size psize = { min_fsize, max_fsize };
  auto process_result = process_all_torrents(torrents, cache_dir, psize,
					     download_p);

  // Check for interrupt
  if (g_interrupted)
    {
      cout << "\n! Interrupted. Cleaning up..." << endl;
      return 130;
    }

  // Step 3: Write output
  if (write_enriched_output(output_file, torrents, process_result, collection_key,
			    min_fsize, cache_dir_size_mb, torrent_total_size_mb))
    {
      print_summary(torrents.size(), process_result);
      return 0;
    }
  else
    return 1;
}
