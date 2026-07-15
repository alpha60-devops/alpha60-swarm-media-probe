#include "json_enricher.hpp"

enrichment::enrichment() {}

pipeline_metrics
enrichment::compute_pipeline_metrics(const process_result& presult,
				     const size_t mini_size, const size_t csize)
{
  const std::vector<media_info_data>& media_data = presult.media_data_list;
  const std::vector<fs::path>& downloaded_files = presult.downloaded_files;

  pipeline_metrics metrics;
  metrics.media_cache_file_size = mini_size;
  metrics.btiha_size = downloaded_files.size();
  metrics.btiha_cached_size = csize;

  for (size_t i = 0; i < downloaded_files.size(); ++i)
    {
      const auto& path = downloaded_files[i];
      const auto& md = (i < media_data.size()) ? media_data[i] : media_info_data();

      // Check if file exists and has size
      bool file_exists = !path.empty() && fs::exists(path);
      uintmax_t file_size = file_exists ? fs::file_size(path) : 0;
      if (file_size == 0)
	{
	  metrics.btiha_unreachable_size++;
	  continue;
	}

      // Check if extraction produced usable metadata.
      // If so, call this media object "extracted"

      // Video metadata
      bool has_vmetadata = false;
      const bool has_vsize = md.video.width > 0 && md.video.height > 0;
      if (has_vsize || !md.video.frame_rate.empty())
	has_vmetadata = true;

      // Audio metadata
      bool has_ametadata = false;
      if (md.audio.bitrate > 0 || md.audio.sampling_rate > 0)
	has_ametadata = true;

      // Format metadata
      bool has_fmetadata = false;
      if (md.duration > 0 || md.file_size > 0)
	has_fmetadata = true;

      // Use these individual flags to assess extraction.
      if (has_vmetadata && has_ametadata && has_fmetadata)
	metrics.btiha_extracted_size++;
      else
	metrics.btiha_partial_size++;
    }

  // Calculate percentage
  if (metrics.btiha_size > 0)
    {
      double br = double(metrics.btiha_extracted_size) / metrics.btiha_size;
      metrics.btiha_extracted_percent = br  * 100.0;
    }

  return metrics;
}

std::string
enrichment::build_output(const std::vector<torrent_file>& torrents,
			 const process_result& presult,
			 const std::string& collection_key,
			 const uint mini_size,
			 uintmax_t cache_dir_size_mb,
			 uintmax_t torrent_total_size_mb,
			 const uint csize)
{
  // Compute pipeline metrics
  pipeline_metrics metrics = compute_pipeline_metrics(presult, mini_size, csize);

  // Get current UTC time
  auto now = std::time(nullptr);
  std::stringstream time_ss;
  time_ss << std::put_time(std::gmtime(&now), "%Y-%m-%dT%H:%M:%SZ");

  std::stringstream ss;
  ss << "{\n";
  ss << "  \"api_version\": \"1.4\",\n";
  ss << "  \"datestamp\": \"" << time_ss.str() << "\",\n";
  ss << "  \"collection_key\": \"" << escape_json_string(collection_key) << "\",\n";
  ss << "  \"collection_media_cache_size_mb\": " << cache_dir_size_mb << ",\n";
  ss << "  \"collection_media_size_mb\": " << torrent_total_size_mb << ",\n";
  ss << "  \"pipeline_metrics\": {\n";
  ss << "    \"btiha_size\": " << metrics.btiha_size << ",\n";
  ss << "    \"min_cache_file_size_mb\": " << metrics.media_cache_file_size / (1024*1024) << ",\n";
  ss << "    \"btiha_unreachable_size\": " << metrics.btiha_unreachable_size << ",\n";
  ss << "    \"btiha_cached_size\": " << metrics.btiha_cached_size << ",\n";
  ss << "    \"btiha_partial_size\": " << metrics.btiha_partial_size << ",\n";
  ss << "    \"btiha_extracted_size\": " << metrics.btiha_extracted_size << ",\n";
  ss << "    \"btiha_extracted_percent\": " << std::fixed << std::setprecision(2) << metrics.btiha_extracted_percent << "\n";
  ss << "  },\n";
  ss << "  \"media_objects\": [\n";

  const std::vector<media_info_data>& media_data = presult.media_data_list;
  for (size_t i = 0; i < torrents.size(); ++i)
    {
      const auto& tf = torrents[i];
      const auto& md = (i < media_data.size()) ? media_data[i] : media_info_data();

    ss << "    {\n";
    ss << "      \"btih\": \"" << escape_json_string(tf.btih) << "\",\n";
    ss << "      \"name\": \"" << escape_json_string(tf.name) << "\",\n";
    ss << "      \"metadata\": {\n";

    // Originating source fields
    ss << "        \"originating_source_medium_id\": "
       << json_string(md.originating_source_medium_id) << ",\n";
    ss << "        \"originating_source_form\": "
       << json_string(md.originating_source_form) << ",\n";
    ss << "        \"originating_network_name\": "
       << json_string(md.originating_network_name) << ",\n";

    // Container fields
    ss << "        \"format_commercial_if_any\": " << json_string(md.format_commercial_if_any) << ",\n";
    ss << "        \"file_size\": " << md.file_size << ",\n";
    ss << "        \"duration\": " << (md.duration > 0 ? std::to_string(md.duration) : "null") << ",\n";
    ss << "        \"overall_bit_rate\": " << (md.overall_bit_rate > 0 ? std::to_string(md.overall_bit_rate) : "null") << ",\n";
    ss << "        \"domain\": " << json_string(md.domain) << ",\n";
    ss << "        \"collection\": " << json_string(md.collection) << ",\n";
    ss << "        \"season\": " << json_string(md.season) << ",\n";
    ss << "        \"distributed_by\": " << json_string(md.distributed_by) << ",\n";
    ss << "        \"genre\": " << json_string(md.genre) << ",\n";
    ss << "        \"content_type\": " << json_string(md.content_type) << ",\n";
    ss << "        \"owner\": " << json_string(md.owner) << ",\n";
    ss << "        \"country\": " << json_string(md.country) << ",\n";
    ss << "        \"comment\": " << json_string(md.comment) << ",\n";

    // Video fields
    ss << "        \"video_codec_id\": " << json_string(md.video.codec_id) << ",\n";
    ss << "        \"video_codec_version\": " << json_string(md.video.codec_version) << ",\n";
    ss << "        \"video_bitrate\": " << (md.video.bitrate > 0 ? std::to_string(md.video.bitrate) : "null") << ",\n";
    ss << "        \"video_frame_rate\": " << json_string(md.video.frame_rate) << ",\n";
    ss << "        \"video_color_primaries\": " << json_string(md.video.color_primaries) << ",\n";
    ss << "        \"video_color_space\": " << json_string(md.video.color_space) << ",\n";
    ss << "        \"video_width\": " << (md.video.width > 0 ? std::to_string(md.video.width) : "null") << ",\n";
    ss << "        \"video_height\": " << (md.video.height > 0 ? std::to_string(md.video.height) : "null") << ",\n";
    ss << "        \"video_creation_metadata\": " << json_string(md.video_creation_metadata) << ",\n";

    // Audio fields
    ss << "        \"audio_codec\": " << json_string(md.audio.codec) << ",\n";
    ss << "        \"audio_codec_version\": " << json_string(md.audio.codec_version) << ",\n";
    ss << "        \"audio_bitrate\": " << (md.audio.bitrate > 0 ? std::to_string(md.audio.bitrate) : "null") << ",\n";
    ss << "        \"audio_sampling_rate\": " << (md.audio.sampling_rate > 0 ? std::to_string(md.audio.sampling_rate) : "null") << ",\n";
    ss << "        \"audio_channels\": " << json_string(md.audio.channels) << ",\n";
    ss << "        \"audio_bit_depth\": " << (md.audio.bit_depth > 0 ? std::to_string(md.audio.bit_depth) : "null") << ",\n";

    // Audio languages array
    ss << "        \"audio_languages\": [";
    for (size_t j = 0; j < md.audio.languages.size(); ++j) {
      if (j > 0) ss << ", ";
      ss << "\"" << escape_json_string(md.audio.languages[j]) << "\"";
    }
    ss << "],\n";

    // Subtitle fields
    ss << "        \"subtitle_languages\": [";
    for (size_t j = 0; j < md.subtitle.languages.size(); ++j) {
      if (j > 0) ss << ", ";
      ss << "\"" << escape_json_string(md.subtitle.languages[j]) << "\"";
    }
    ss << "],\n";
    ss << "        \"subtitle_format\": " << json_string(md.subtitle.format) << "\n";

    ss << "      }\n";
    ss << "    }";
    if (i < torrents.size() - 1) ss << ",";
    ss << "\n";
  }

  ss << "  ]\n";
  ss << "}\n";

  return ss.str();
}

std::string
enrichment::json_string(const std::string& str)
{
  if (str.empty())
    return "null";
  return "\"" + escape_json_string(str) + "\"";
}

bool
enrichment::write_output(const std::string& output_path,
			 const std::string& json_content)
{
  bool ret(false);
  std::ofstream file(output_path);
  if (file.is_open())
    {
      file << json_content;
      ret = true;
    }
  return ret;
}

std::string
enrichment::escape_json_string(const std::string& str)
{
  std::stringstream ss;
  for (char c : str)
    {
      switch (c)
	{
	case '"': ss << "\\\""; break;
	case '\\': ss << "\\\\"; break;
	case '\b': ss << "\\b"; break;
	case '\f': ss << "\\f"; break;
	case '\n': ss << "\\n"; break;
	case '\r': ss << "\\r"; break;
	case '\t': ss << "\\t"; break;
	default: ss << c; break;
	}
    }
  return ss.str();
}
