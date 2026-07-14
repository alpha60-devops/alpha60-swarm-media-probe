#ifndef TORRENT_PARSER_HPP
#define TORRENT_PARSER_HPP

#include <string>
#include <vector>
#include <optional>
#include <filesystem>
#include <openssl/sha.h>
#include <fstream>
#include <iomanip>
#include <sstream>

#include <libtorrent/torrent_info.hpp>
#include <libtorrent/info_hash.hpp>
#include <libtorrent/bdecode.hpp>
#include <libtorrent/bencode.hpp>

namespace fs = std::filesystem;

struct torrent_file {
    std::string btih;                     // BitTorrent Info Hash (40-character hex)
    std::string name;                     // Torrent name
    std::vector<std::string> file_paths;  // Paths of files within torrent
    std::vector<std::int64_t> file_sizes; // Sizes of files in bytes
    std::int64_t total_size;              // Total size of all files in bytes
    fs::path torrent_path;                // Filesystem path to .torrent file
};

class torrent_parser {
public:
    // Constructor takes directory containing .torrent files
    explicit torrent_parser(const fs::path& idir);
    
    // Parse all .torrent files in the input directory
    std::vector<torrent_file> parse_all_torrents();
    
    // Parse a single .torrent file
    std::optional<torrent_file> parse_single_torrent(const fs::path& torrent_file);
    
private:
    fs::path input_directory;
    
    // Compute BTIH (info hash) from torrent file data
    std::string compute_info_hash(const std::vector<char>& torrent_data);
    
    // Read a .torrent file into a byte buffer
    std::vector<char> read_torrent_file(const fs::path& path);
};

#endif // TORRENT_PARSER_HPP
