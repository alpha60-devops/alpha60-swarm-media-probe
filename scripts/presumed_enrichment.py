#!/usr/bin/env python3
"""Create swarm media enrichment JSON from torrent filenames using GuessIt."""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, BinaryIO

try:
    from guessit import guessit
except ImportError as exc:  # pragma: no cover
    raise SystemExit("guessit is required: python3 -m pip install guessit") from exc

API_VERSION = "1.4"
MEDIA_EXTENSIONS = {
    ".3gp", ".aac", ".ac3", ".aiff", ".ape", ".asf", ".avi", ".divx",
    ".dts", ".eac3", ".flac", ".flv", ".m2ts", ".m4a", ".m4v", ".mka",
    ".mkv", ".mov", ".mp3", ".mp4", ".mpeg", ".mpg", ".mts", ".ogg",
    ".ogm", ".ogv", ".opus", ".rm", ".rmvb", ".ts", ".vob", ".wav",
    ".webm", ".wma", ".wmv",
}


class BencodeError(ValueError):
    pass


@dataclass(frozen=True)
class TorrentEntry:
    btih: str
    name: str
    file_size: int


def read_bencoded(stream: BinaryIO) -> Any:
    token = stream.read(1)
    if not token:
        raise BencodeError("unexpected end of file")
    if token == b"i":
        raw = _read_until(stream, b"e")
        try:
            return int(raw)
        except ValueError as exc:
            raise BencodeError("invalid integer") from exc
    if token == b"l":
        values = []
        while _peek(stream) != b"e":
            values.append(read_bencoded(stream))
        stream.read(1)
        return values
    if token == b"d":
        values = {}
        while _peek(stream) != b"e":
            key = read_bencoded(stream)
            if not isinstance(key, bytes):
                raise BencodeError("dictionary key is not bytes")
            values[key] = read_bencoded(stream)
        stream.read(1)
        return values
    if token.isdigit():
        length_raw = token + _read_until(stream, b":")
        try:
            length = int(length_raw)
        except ValueError as exc:
            raise BencodeError("invalid byte-string length") from exc
        value = stream.read(length)
        if len(value) != length:
            raise BencodeError("truncated byte string")
        return value
    raise BencodeError(f"unexpected token {token!r}")


def encode_bencoded(value: Any) -> bytes:
    if isinstance(value, bytes):
        return str(len(value)).encode("ascii") + b":" + value
    if isinstance(value, int):
        return b"i" + str(value).encode("ascii") + b"e"
    if isinstance(value, list):
        return b"l" + b"".join(encode_bencoded(item) for item in value) + b"e"
    if isinstance(value, dict):
        return b"d" + b"".join(
            encode_bencoded(key) + encode_bencoded(value[key])
            for key in sorted(value)
        ) + b"e"
    raise TypeError(f"cannot bencode {type(value).__name__}")


def _read_until(stream: BinaryIO, delimiter: bytes) -> bytes:
    chunks = bytearray()
    while True:
        byte = stream.read(1)
        if not byte:
            raise BencodeError("missing delimiter")
        if byte == delimiter:
            return bytes(chunks)
        chunks.extend(byte)


def _peek(stream: BinaryIO) -> bytes:
    position = stream.tell()
    value = stream.read(1)
    stream.seek(position)
    if not value:
        raise BencodeError("unexpected end of file")
    return value


def decode_text(value: Any) -> str:
    if not isinstance(value, bytes):
        return ""
    return value.decode("utf-8", errors="replace")


def torrent_entries(path: Path) -> list[TorrentEntry]:
    with path.open("rb") as stream:
        metainfo = read_bencoded(stream)
    if not isinstance(metainfo, dict) or not isinstance(metainfo.get(b"info"), dict):
        raise BencodeError("torrent has no info dictionary")

    info = metainfo[b"info"]
    btih = hashlib.sha1(encode_bencoded(info)).hexdigest()
    root_name = decode_text(info.get(b"name.utf-8") or info.get(b"name"))
    files = info.get(b"files")
    entries: list[TorrentEntry] = []

    if isinstance(files, list):
        for item in files:
            if not isinstance(item, dict):
                continue
            parts = item.get(b"path.utf-8") or item.get(b"path") or []
            relative = "/".join(decode_text(part) for part in parts)
            name = f"{root_name}/{relative}" if root_name and relative else relative or root_name
            entries.append(TorrentEntry(btih, name, int(item.get(b"length", 0))))
    else:
        entries.append(TorrentEntry(btih, root_name or path.stem, int(info.get(b"length", 0))))

    return [entry for entry in entries if Path(entry.name).suffix.lower() in MEDIA_EXTENSIONS]


def as_text(value: Any) -> str | None:
    if value is None:
        return None
    if isinstance(value, (list, tuple, set)):
        return ", ".join(str(item) for item in value) or None
    return str(value)


def as_languages(value: Any) -> list[str]:
    if value is None:
        return []
    values = value if isinstance(value, (list, tuple, set)) else [value]
    return [str(item) for item in values]


def resolution_dimensions(value: Any) -> tuple[int | None, int | None]:
    if not value:
        return None, None
    resolution = str(value).lower()
    known = {
        "2160p": (3840, 2160), "1080p": (1920, 1080), "1080i": (1920, 1080),
        "720p": (1280, 720), "576p": (720, 576), "576i": (720, 576),
        "480p": (720, 480), "480i": (720, 480),
    }
    return known.get(resolution, (None, None))


def presumed_metadata(name: str, file_size: int) -> dict[str, Any]:
    guessed = dict(guessit(Path(name).name))
    width, height = resolution_dimensions(guessed.get("screen_size"))
    media_type = as_text(guessed.get("type"))
    title = as_text(guessed.get("title"))
    season = guessed.get("season")
    episode = guessed.get("episode")
    season_text = as_text(season)
    if episode is not None:
        season_text = f"S{int(season):02d}E{int(episode):02d}" if season is not None else f"E{int(episode):02d}"

    source = as_text(guessed.get("source"))
    release_group = as_text(guessed.get("release_group"))
    year = as_text(guessed.get("year"))
    comment_bits = [bit for bit in (source, release_group, year) if bit]

    return {
        "originating_source_medium_id": title,
        "originating_source_form": media_type,
        "originating_network_name": as_text(guessed.get("streaming_service")),
        "format_commercial_if_any": as_text(guessed.get("container")) or Path(name).suffix.lstrip(".").upper() or None,
        "file_size": file_size,
        "duration": None,
        "overall_bit_rate": None,
        "domain": None,
        "collection": title,
        "season": season_text,
        "distributed_by": release_group,
        "genre": None,
        "content_type": media_type,
        "owner": None,
        "country": as_text(guessed.get("country")),
        "comment": "; ".join(comment_bits) or None,
        "video_codec_id": as_text(guessed.get("video_codec")),
        "video_codec_version": as_text(guessed.get("video_profile")),
        "video_bitrate": None,
        "video_frame_rate": as_text(guessed.get("frame_rate")),
        "video_color_primaries": as_text(guessed.get("color_depth")),
        "video_color_space": as_text(guessed.get("color_space")),
        "video_width": width,
        "video_height": height,
        "video_creation_metadata": year,
        "audio_codec": as_text(guessed.get("audio_codec")),
        "audio_codec_version": as_text(guessed.get("audio_profile")),
        "audio_bitrate": as_text(guessed.get("audio_bit_rate")),
        "audio_sampling_rate": None,
        "audio_channels": as_text(guessed.get("audio_channels")),
        "audio_bit_depth": None,
        "audio_languages": as_languages(guessed.get("language")),
        "subtitle_languages": as_languages(guessed.get("subtitle_language")),
        "subtitle_format": as_text(guessed.get("subtitle_format")),
    }


def presumed_output_path(requested: Path) -> Path:
    if requested.name.endswith(".presumed.json"):
        return requested
    if requested.suffix.lower() == ".json":
        return requested.with_name(f"{requested.stem}.presumed.json")
    return requested.with_name(f"{requested.name}.presumed.json")


def build_output(torrent_dir: Path, collection_key: str) -> dict[str, Any]:
    torrent_paths = sorted(torrent_dir.rglob("*.torrent"))
    objects = []
    total_size = 0
    failures = 0
    for torrent_path in torrent_paths:
        try:
            entries = torrent_entries(torrent_path)
        except (OSError, BencodeError, TypeError, ValueError) as exc:
            failures += 1
            print(f"warning: {torrent_path}: {exc}", file=sys.stderr)
            continue
        for entry in entries:
            total_size += entry.file_size
            objects.append({
                "btih": entry.btih,
                "name": entry.name,
                "metadata": presumed_metadata(entry.name, entry.file_size),
            })

    count = len(objects)
    return {
        "api_version": API_VERSION,
        "datestamp": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "collection_key": collection_key,
        "collection_media_cache_size_mb": 0,
        "collection_media_size_mb": total_size // (1024 * 1024),
        "pipeline_metrics": {
            "btiha_size": count,
            "media_cache_file_size_mb": 0,
            "btiha_unreachable_size": failures,
            "btiha_partial_size": count,
            "btiha_extracted_size": 0,
            "btiha_extracted_percent": 0.0,
        },
        "media_objects": objects,
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("torrent_dir", type=Path, help="directory containing .torrent files")
    parser.add_argument("output", type=Path, help="requested JSON path; '.presumed' is appended")
    parser.add_argument("--collection-key", help="defaults to the torrent directory name")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if not args.torrent_dir.is_dir():
        print(f"error: not a directory: {args.torrent_dir}", file=sys.stderr)
        return 2
    output_path = presumed_output_path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    payload = build_output(args.torrent_dir, args.collection_key or args.torrent_dir.name)
    output_path.write_text(json.dumps(payload, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(output_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
