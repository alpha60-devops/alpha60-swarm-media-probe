# Media Objects Content Analysis API - Version 1.0

## Overview

This API defines the output format for enriched media content analysis derived from torrent metadata and mediainfo extraction.

Output Filename: media_objects_content_analysis.json

## Top-Level Structure

```
{
  "api_version": "1.0",
  "generated_at": "2026-06-03T20:42:56Z",
  "total_objects": 2,
  "media_objects": [ ... ]
}
```

Field             | Type    | Description
------------------|---------|--------------------------------------------------------
api_version       | string  | Version of this API specification (fixed to "1.0")
generated_at      | string  | ISO 8601 UTC timestamp of file generation
total_objects     | integer | Number of objects in media_objects array
media_objects     | array   | Array of MediaObject objects

## MediaObject Schema

### Top-Level Fields

Field               | Type   | Required | Description
--------------------|--------|----------|------------------------------------------------------------
btih                | string | Yes      | BitTorrent Info Hash (40-character hex string)
name                | string | Yes      | Human-readable name from torrent metadata
technical_metadata  | object | Yes      | Container for all extracted technical fields

technical_metadata Fields

### Originating Source Metadata

Field                         | Type   | Description
------------------------------|--------|--------------------------------------------------------------
originating_source_medium_id | string | Source medium identifier (e.g., "WEB", "HDTV", "Blu-ray")
originating_source_form       | string | Original source format description
originating_network_name      | string | Original broadcast network or streaming service

### Container / General Metadata

Field                     | Type    | Description
--------------------------|---------|--------------------------------------------------------------
format_commercial_if_any  | string  | Commercial format name (e.g., "WEB-DL", "Ultra HD Blu-ray")
file_size                 | integer | File size in bytes
duration                  | float   | Duration in seconds
overall_bit_rate          | integer | Overall bitrate in bps
domain                    | string  | Source domain classification
collection                | string  | Collection or series name
season                    | string  | Season number (as string to handle "N/A")
distributed_by            | string  | Distribution company or network
genre                     | string  | Content genre classification
content_type              | string  | Type of content (e.g., "Live Coverage", "Documentary")
owner                     | string  | Content owner or rights holder
country                   | string  | Country of origin
comment                   | string  | User or encoding comments
language                  | string  | Primary language of content (ISO 639-2 three-letter code)

### Video Track Metadata

Field                   | Type    | Description
------------------------|---------|--------------------------------------------------------------
video_codec             | string  | Video codec format (e.g., "AVC", "HEVC", "MPEG-4")
video_codec_version     | string  | Version identifier for the video codec
video_bitrate           | integer | Video bitrate in bps
video_width             | integer | Display width in pixels
video_height            | integer | Display height in pixels
video_width_sampled     | integer | Sampled/encoded width in pixels
video_height_sampled    | integer | Sampled/encoded height in pixels
video_creation_metadata | string  | UTC timestamp of encoding/tagging
video_frame_rate        | string  | Video frame rate in fps (as string to handle fractions)

### Audio Track Metadata

Field                 | Type    | Description
----------------------|---------|--------------------------------------------------------------
audio_codec           | string  | Audio codec format (e.g., "AAC", "AC-3", "MP3")
audio_codec_version   | string  | Version identifier for the audio codec
audio_bitrate         | integer | Audio bitrate in bps
audio_sampling_rate   | integer | Sampling rate in Hz
audio_channels        | string  | Channel count (e.g., "2", "6", "8")
audio_bit_depth       | integer | Bit depth for lossless/high-res audio
audio_languages       | array   | Array of ISO 639-2 language codes present in audio tracks

### Subtitle Track Metadata

Field                 | Type   | Description
----------------------|--------|--------------------------------------------------------------
subtitle_languages    | array  | Array of ISO 639-2 language codes present in subtitle tracks
subtitle_format       | string | Subtitle format (e.g., "UTF-8", "ASS", "SRT") or null if none

## Example Output

```
{
  "btih": "1d0262a0f4880be8bc8352422836596bc457d03b",
  "name": "Spider-Noir.S01E03.Double.Cross.1080p.HEVC.x265-MeGusta.mkv",
  "technical_metadata": {
	"originating_source_medium_id": null,
	"originating_source_form": null,
	"originating_network_name": null,
	"format_commercial_if_any": null,
	"file_size": 684931254,
	"duration": 2843.132000,
	"overall_bit_rate": 1927258,
	"domain": null,
	"collection": null,
	"season": null,
	"distributed_by": null,
	"genre": null,
	"content_type": null,
	"owner": null,
	"country": null,
	"comment": null,
	"language": null,
	"video_codec": "HEVC",
	"video_codec_version": null,
	"video_bitrate": 1312906,
	"video_width": 1994,
	"video_height": 1080,
	"video_width_sampled": null,
	"video_height_sampled": null,
	"video_creation_metadata": null,
	"video_frame_rate": "23.976",
	"audio_codec": "E-AC-3",
	"audio_codec_version": null,
	"audio_bitrate": 576000,
	"audio_sampling_rate": 48000,
	"audio_channels": "6",
	"audio_bit_depth": 32,
	"audio_languages": [],
	"subtitle_languages": [],
	"subtitle_format": "UTF-8"
  }
}
```

## Processing Rules

Multiple Tracks:
- For audio_languages and subtitle_languages, collect all unique values from all tracks.
- For single-valued fields, use the first track of that type.

Missing Fields:
- Set missing scalar fields to null.
- Set missing array fields to [].
- Do not omit fields from output.

Data Type Conversions:
- FileSize, BitRate, OverallBitRate -> integer.
- Duration -> float (seconds).
- FrameRate -> string (preserve fractions like "23.976").
- Language -> three-letter ISO 639-2 codes (lowercase).
