#!/usr/bin/env python3
"""
Python test simulating the xbmc/filesystem/bluray metadata extraction flow
when browsing a Blu-ray directory (CBlurayDirectory::GetDirectory).

The sample is a remote Blu-ray ISO fetched via HTTP range requests.
Only the UDF volume structure + BDMV/PLAYLIST + BDMV/CLIPINF files are
downloaded (a few hundred KB), not the ~73.6 GB content.

Usage:
  python3 test_bluray_metadata.py [--iso-url URL]

Default ISO URL is the test sample.

This mirrors the C++ parsing in:
  - xbmc/filesystem/BlurayDirectory.cpp
  - xbmc/filesystem/bluray/MPLSParser.cpp
  - xbmc/filesystem/bluray/StreamParser.cpp
"""

import struct
import sys
import os
import urllib.request
import urllib.error
import hashlib
from collections import OrderedDict
from typing import Optional, List, Tuple, Dict, BinaryIO

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

SECTOR_SIZE = 2048

# UDF TagIdentifiers
TAG_AVDP = 2
TAG_PVD = 1
TAG_PARTITION_DESC = 5
TAG_LOGICAL_VOLUME_DESC = 6
TAG_FSD = 256
TAG_FILE_ENTRY = 261
TAG_EXTENDED_FILE_ENTRY = 266
TAG_TERMINATING_DESC = 8

# UDF FileTypes
FILE_TYPE_DIR = 4
FILE_TYPE_FILE = 5

# Blu-ray video formats (from MPLS STN table)
BLURAY_VIDEO_FORMAT_480I = 0
BLURAY_VIDEO_FORMAT_576I = 1
BLURAY_VIDEO_FORMAT_480P = 2
BLURAY_VIDEO_FORMAT_576P = 3
BLURAY_VIDEO_FORMAT_720P = 4
BLURAY_VIDEO_FORMAT_1080I = 5
BLURAY_VIDEO_FORMAT_1080P = 6
BLURAY_VIDEO_FORMAT_2160P = 7

BLURAY_VIDEO_FORMAT_NAMES = {
    BLURAY_VIDEO_FORMAT_480I: "480i",
    BLURAY_VIDEO_FORMAT_576I: "576i",
    BLURAY_VIDEO_FORMAT_480P: "480p",
    BLURAY_VIDEO_FORMAT_576P: "576p",
    BLURAY_VIDEO_FORMAT_720P: "720p",
    BLURAY_VIDEO_FORMAT_1080I: "1080i",
    BLURAY_VIDEO_FORMAT_1080P: "1080p",
    BLURAY_VIDEO_FORMAT_2160P: "2160p",
}

# Blu-ray video rates (frame rates)
BLURAY_VIDEO_RATE_23_976 = 1
BLURAY_VIDEO_RATE_24 = 2
BLURAY_VIDEO_RATE_25 = 3
BLURAY_VIDEO_RATE_29_97 = 4
BLURAY_VIDEO_RATE_50 = 6
BLURAY_VIDEO_RATE_60 = 7

BLURAY_VIDEO_RATE_NAMES = {
    BLURAY_VIDEO_RATE_23_976: "23.976",
    BLURAY_VIDEO_RATE_24: "24",
    BLURAY_VIDEO_RATE_25: "25",
    BLURAY_VIDEO_RATE_29_97: "29.97",
    BLURAY_VIDEO_RATE_50: "50",
    BLURAY_VIDEO_RATE_60: "60",
}

# Encoding types (matching ENCODING_TYPE in DiscDirectoryHelper.h)
ENCODING_TYPE = {
    0x02: "MPEG-2 Video",
    0xea: "VC-1 Video",
    0x1b: "H.264/AVC Video",
    0x20: "H.264 MVC Video",
    0x24: "HEVC/H.265 Video",
    0x80: "LPCM Audio",
    0x81: "AC-3 Audio",
    0x82: "DTS Audio",
    0x83: "TrueHD Audio",
    0x84: "E-AC-3 Audio",
    0x85: "DTS-HD Audio",
    0x86: "DTS-HD Master Audio",
    0xa1: "E-AC-3 Secondary Audio",
    0xa2: "DTS-HD Secondary Audio",
    0x90: "Presentation Graphics (Subtitle)",
    0x91: "Interactive Graphics",
    0x92: "Text Subtitle",
}

# BLURAY_PLAYBACK_TYPE
BLURAY_PLAYBACK_TYPE_NAMES = {
    1: "Sequential",
    2: "Random",
    3: "Shuffle",
}

# BLURAY_CONNECTION
BLURAY_CONNECTION_NAMES = {
    1: "Seamless",
    5: "Non-seamless",
    6: "Branching (Out-of-mux)",
}

# BLURAY_STREAM_TYPE
BLURAY_STREAM_TYPE_NAMES = {
    1: "PlayItem",
    2: "SubPath",
    3: "SubPath (in-mux PiP)",
    4: "SubPath (Dolby Vision)",
}

# Dynamic Range
HDR_TYPE_NAMES = {
    0: "SDR",
    1: "HDR10",
    2: "HDR10+",
    3: "Dolby Vision",
}

# ASPECT_RATIO
ASPECT_RATIO_NAMES = {
    2: "4:3",
    3: "16:9",
}

# ---------------------------------------------------------------------------
# Helper: HTTP Range Fetcher with caching
# ---------------------------------------------------------------------------


class RangeFetcher:
    """Fetch byte ranges from a remote resource via HTTP Range requests."""

    def __init__(self, url: str, timeout: int = 30):
        self.url = url
        self.timeout = timeout
        self._cache: Dict[int, bytes] = {}
        self.content_length: Optional[int] = None
        self._get_content_length()

    def _get_content_length(self):
        req = urllib.request.Request(self.url, method="HEAD")
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                length = resp.headers.get("Content-Length")
                if length:
                    self.content_length = int(length)
        except urllib.error.URLError:
            pass

    def read_range(self, offset: int, length: int) -> bytes:
        """Read a contiguous byte range, caching by sector."""
        # Round to sector boundaries for caching
        start_sector = offset // SECTOR_SIZE
        end_sector = (offset + length + SECTOR_SIZE - 1) // SECTOR_SIZE
        result = b""

        for sector in range(start_sector, end_sector):
            if sector not in self._cache:
                sector_offset = sector * SECTOR_SIZE
                req = urllib.request.Request(
                    self.url,
                    headers={"Range": f"bytes={sector_offset}-{sector_offset + SECTOR_SIZE - 1}"},
                )
                try:
                    with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                        if resp.status == 206:
                            self._cache[sector] = resp.read()
                        else:
                            raise RuntimeError(
                                f"Expected 206 Partial Content, got {resp.status}"
                            )
                except urllib.error.HTTPError as e:
                    raise RuntimeError(f"HTTP {e.code} fetching sector {sector}: {e.reason}")

            data = self._cache[sector]
            chunk_start = max(0, offset - sector * SECTOR_SIZE)
            chunk_end = min(SECTOR_SIZE, offset + length - sector * SECTOR_SIZE)
            result += data[chunk_start:chunk_end]

        return result[:length]

    def read_sector(self, sector: int) -> bytes:
        return self.read_range(sector * SECTOR_SIZE, SECTOR_SIZE)

    def read_sectors(self, start_sector: int, count: int) -> bytes:
        return self.read_range(start_sector * SECTOR_SIZE, count * SECTOR_SIZE)


# ---------------------------------------------------------------------------
# UDF Filesystem Reader (minimal — enough for BDMV traversal)
# ---------------------------------------------------------------------------


def u8(data: bytes, off: int) -> int:
    return data[off]


def u16(data: bytes, off: int) -> int:
    return struct.unpack_from("<H", data, off)[0]


def u32(data: bytes, off: int) -> int:
    return struct.unpack_from("<I", data, off)[0]


def u64(data: bytes, off: int) -> int:
    return struct.unpack_from("<Q", data, off)[0]


def read_tag(data: bytes, off: int) -> dict:
    return {
        "id": u16(data, off),
        "version": u16(data, off + 2),
        "checksum": u8(data, off + 4),
        "serial": u16(data, off + 6),
        "crc": u16(data, off + 8),
        "crc_length": u16(data, off + 10),
        "location": u32(data, off + 12),
    }


def read_long_ad(data: bytes, off: int) -> Tuple[int, int, int]:
    """Return (extent_length, block_location, partition_ref) for a Long AD."""
    return u32(data, off), u32(data, off + 4), u16(data, off + 8)


def read_short_ad(data: bytes, off: int) -> Tuple[int, int]:
    """Return (extent_length, block_location) for a Short AD."""
    return u32(data, off), u32(data, off + 4)


class UDFReader:
    """
    Minimal UDF 2.50 filesystem reader.
    Can traverse directory trees and read files by path.
    """

    def __init__(self, fetcher: RangeFetcher):
        self.fetcher = fetcher
        self._root_icb: Optional[Tuple[int, int]] = None  # (sector, partition)
        self._volume_set_icb: Optional[Tuple[int, int]] = None
        self._sector_size = SECTOR_SIZE
        self._partition_offset: int = 0
        self._init()

    def _init(self):
        """Locate AVDP, FSD, and root directory ICB."""
        # Try AVDP at sector 256
        data = self.fetcher.read_sector(256)
        tag = read_tag(data, 0)
        if tag["id"] != TAG_AVDP:
            # Try sector 512
            data = self.fetcher.read_sector(512)
            tag = read_tag(data, 0)
            if tag["id"] != TAG_AVDP:
                raise RuntimeError("Cannot find Anchor Volume Descriptor Pointer")

        # Main VDS extent
        vds_len, vds_loc, _ = read_long_ad(data, 16)

        # Scan VDS for Partition Descriptor and Logical Volume Descriptor
        partition_offset = 0
        lvds_sector = None

        current = vds_loc
        while True:
            data = self.fetcher.read_sector(current)
            tag = read_tag(data, 0)
            if tag["id"] == TAG_TERMINATING_DESC:
                break
            if tag["id"] == TAG_PARTITION_DESC:
                # Partition Descriptor: volume_sequence_number(2) + reserved(2) +
                # implementation_use_flags(2) + reserved(2) + length(4) +
                # start_sector(4) + num_sectors(4) + ...
                partition_offset = u32(data, 30)
            elif tag["id"] == TAG_LOGICAL_VOLUME_DESC:
                lvds_sector = current
            current += 1

        if lvds_sector is None:
            raise RuntimeError("Cannot find Logical Volume Descriptor")

        # LVD has: tag(16) + rec_date(12) + ... + domain_id(32) +
        # logical_volume_contents_use(16=long_ad) + map_table(16) +
        # partition_map_count(4) + implementation_id(32) + ...
        # The file set descriptor location is the first long_ad after
        # logical_volume_contents_use at offset 128.
        # Actually: LVD offset 0=tag(16), 16=rec_date(12), 28=..., 
        # Actually let me compute more carefully.
        # LVD tag=6, after tag(16), rec_date(12) = 28, then...
        # Let me just look for the FSD via the Volume Descriptor sequence.
        # Actually, the LVD's logical_volume_contents_use field is a long_ad
        # pointing to the File Set Descriptor.
        data = self.fetcher.read_sector(lvds_sector)
        # LVD: tag(16) + rec_date(12) + character_set(2) + lv_id(128) +
        # lv_space_size(4) + ..., then domain_id(32), then contents_use(16)
        # Actually let me count the bytes carefully.
        # LVD starts at offset 0 with tag(16)
        # offset 16: recording date (12) = 28
        # offset 28: character_set (2) = 30
        # offset 30: logical_volume_identifier (128) = 158
        # offset 158: logical_block_size (4) = 162
        # offset 162: domain_tag (16) = 178
        # offset 178: ... actually let me just use a fixed offset.
        # In UDF spec, LVD content_use is at offset 264 (0x108):
        # tag(16) + recording_date(12) + character_set(2) + lv_identifier(128) + 
        # lv_size(4) + lv_block_size(4) + domain_id(32) + content_use(16) + ...
        # Actually let me just use the offset from the spec:
        # LVD (tag 6) structure:
        # +0  tag(16) 
        # +16 recording_date 12 
        # +28 character_set 2
        # +30 logical_volume_identifier 128
        # +158 logical_block_size 4
        # +162 domain_identifier 32
        # +194 logical_volume_contents_use 16 (long_ad)
        # +210 map_table 16 (long_ad)
        # +226 partition_maps
        fsd_len, fsd_loc, _ = read_long_ad(data, 194)

        # Read File Set Descriptor
        data = self.fetcher.read_sector(fsd_loc)
        tag = read_tag(data, 0)
        if tag["id"] != TAG_FSD:
            raise RuntimeError(f"Expected File Set Descriptor, got tag {tag['id']}")

        # FSD: tag(16) + rec_date(12) + interchange_level(2) + max_interchange(2) +
        # char_set_list(4) + max_char_set(4) + file_set_number(4) +
        # file_set_desc_number(4) + integrity_sequence(16) + extent(16) +
        # root_dir_icb(16) + domain_id(32) + next_extent(16) + stream_dir_icb(16)
        # Root directory ICB is at offset 120 (16 * 5 within FSD after tag + rec_date).
        # Actually: FSD tag bytes 0-15, then:
        # +16 rec_date 12 = 28
        # +28 interchange_level 2 = 30
        # +30 max_interchange 2 = 32
        # +32 char_set_list 4 = 36
        # +36 max_char_set 4 = 40
        # +40 file_set_number 4 = 44
        # +44 file_set_desc_number 4 = 48
        # +48 integrity_sequence 16 = 64
        # +64 content_extent 16 = 80
        # +80 root_dir_icb 16 (long_ad)
        root_len, root_loc, root_part = read_long_ad(data, 80)
        self._root_icb = (root_loc, root_part)

        # Also get the volume set ICB (for reading files)
        vol_len, vol_loc, vol_part = read_long_ad(data, 64)
        self._volume_set_icb = (vol_loc, vol_part)

        self._partition_offset = partition_offset

    def _read_file_entry(self, location: int) -> bytes:
        """Read the File Entry descriptor and return the raw bytes."""
        data = self.fetcher.read_sector(location)
        tag = read_tag(data, 0)
        if tag["id"] not in (TAG_FILE_ENTRY, TAG_EXTENDED_FILE_ENTRY):
            raise RuntimeError(f"Expected File Entry, got tag {tag['id']}")
        return data[:SECTOR_SIZE]

    def _get_file_entry_info(self, fe_data: bytes) -> dict:
        """Extract info from a File Entry (or Extended File Entry)."""
        tag = read_tag(fe_data, 0)
        is_extended = tag["id"] == TAG_EXTENDED_FILE_ENTRY

        if is_extended:
            # Extended File Entry: tag(16) + icb_tag(16) + uid(4) + gid(4) +
            # permissions(4) + link_count(2) + rec_format(1) + rec_attrs(1) +
            # rec_length(4) + info_length(8) + logical_blocks(8) + ...
            # offset 56: info_length(8)
            info_length = u64(fe_data, 56)
            # offset 64: logical_blocks_recorded(8)
            logical_blocks = u64(fe_data, 64)
            # offset 72: access_time(12) + 84 modification_time(12) + 96 attr_time(12)
            # offset 108: checkpoint(4) + extended_attr_icb(16) + impl_id(32) + 
            # unique_id(8) + ext_attr_length(4) + alloc_descriptors_length(4)
            # offset 184: ext_attributes + alloc_descriptors
            ext_attr_len = u32(fe_data, 176)
            alloc_desc_len = u32(fe_data, 180)
            alloc_desc_offset = 184 + ext_attr_len
        else:
            # File Entry: tag(16) + icb_tag(16) + uid(4) + gid(4) +
            # permissions(4) + link_count(2) + rec_format(1) + rec_attrs(1) +
            # rec_length(4) + info_length(8) + logical_blocks(8) + ...
            # offset 56: info_length(8)
            info_length = u64(fe_data, 56)
            # offset 64: logical_blocks_recorded(8)
            logical_blocks = u64(fe_data, 64)
            # offset 72: access_time(12) + 84 modification_time(12) + 96 attr_time(12)
            # offset 108: checkpoint(4) + ext_attr_icb(16) + impl_id(32) +
            # unique_id(8) + ext_attr_length(4) + alloc_desc_length(4)
            # offset 184: ext_attributes + alloc_descriptors
            ext_attr_len = u32(fe_data, 176)
            alloc_desc_len = u32(fe_data, 180)
            alloc_desc_offset = 184 + ext_attr_len

        # ICB tag has file type at byte 11
        icb_tag = u16(fe_data, 16) | (u16(fe_data, 18) << 16)
        file_type = (icb_tag >> 24) & 0xFF

        # Parse allocation descriptors
        alloc_data = fe_data[alloc_desc_offset: alloc_desc_offset + alloc_desc_len]
        extents = []
        pos = 0
        while pos < len(alloc_data):
            ext_len = u32(alloc_data, pos)
            ext_loc = u32(alloc_data, pos + 4)
            if ext_len == 0:
                break
            # Check if long_ad (14 bytes) or short_ad (8 bytes)
            # The ICB tag flags (bits 0-1) determine type.
            # For now try both — short_ad is most common.
            # Actually, the flag in ICBTag bits 0-1: 0=short, 1=long, 2=extended.
            flags = icb_tag & 0x7
            if flags == 1:  # long_ad
                extents.append((ext_len, ext_loc))
                pos += 14
            else:  # short_ad (default)
                extents.append((ext_len, ext_loc))
                pos += 8

        return {
            "file_type": file_type,
            "info_length": info_length,
            "logical_blocks": logical_blocks,
            "extents": extents,
        }

    def _read_file_data(self, fe_data: bytes) -> bytes:
        """Read the full file content from its allocation extents."""
        info = self._get_file_entry_info(fe_data)
        result = b""
        for ext_len, ext_loc in info["extents"]:
            # Read the extent from the partition
            abs_sector = ext_loc + self._partition_offset
            remaining = ext_len
            sector = abs_sector
            while remaining > 0:
                chunk = self.fetcher.read_sector(sector)
                take = min(remaining, SECTOR_SIZE)
                result += chunk[:take]
                remaining -= take
                sector += 1
        return result[: info["info_length"]]

    def _read_directory_entries(self, fe_data: bytes) -> List[Tuple[str, int, int]]:
        """
        Read directory entries from a File Entry.
        Returns list of (name, file_type, location_sector).
        """
        file_data = self._read_file_data(fe_data)
        entries = []
        pos = 0
        while pos + 4 < len(file_data):
            # Descriptor Tag
            tag = read_tag(file_data, pos)
            if tag["id"] not in (257,):  # File Identifier Descriptor
                pos += 1
                continue

            # FID: tag(16) + version(1) + characteristics(1) + id_len(1) +
            # icb(16=long_ad) + impl_use_len(2) + ext_attr_len(2) +
            # impl_use + ext_attr + file_id (padded to even)
            characteristics = u8(file_data, pos + 17)
            id_len = u8(file_data, pos + 18)
            icb_len = u32(file_data, pos + 20)
            icb_loc = u32(file_data, pos + 24)
            impl_use_len = u16(file_data, pos + 36)
            ext_attr_len = u16(file_data, pos + 38)

            file_id_offset = pos + 40 + impl_use_len + ext_attr_len
            # Pad to even
            file_id_offset = (file_id_offset + 1) & ~1

            if id_len > 0:
                name = file_data[file_id_offset: file_id_offset + id_len].decode("utf-8", errors="replace")
            else:
                name = ""

            is_dir = bool(characteristics & 0x01)
            entries.append((name, FILE_TYPE_DIR if is_dir else FILE_TYPE_FILE, icb_loc))

            # Next FID at even boundary
            fid_len = file_id_offset + id_len - pos
            fid_len = (fid_len + 1) & ~1  # pad to even
            # Minimum fid_len, but actually we need to compute the full size
            # FID length = 40 + impl_use_len + ext_attr_len + (id_len padded to even)
            entry_len = 40 + impl_use_len + ext_attr_len + id_len
            if entry_len % 2:
                entry_len += 1
            pos += entry_len

        return entries

    def find_file(self, path: str) -> Optional[bytes]:
        """
        Find and read a file by UDF path (e.g. 'BDMV/PLAYLIST/00001.mpls').
        Returns the file contents as bytes, or None if not found.
        """
        parts = path.strip("/").split("/")
        if self._root_icb is None:
            return None

        current_loc, _ = self._root_icb

        for part in parts:
            fe_data = self._read_file_entry(current_loc)
            info = self._get_file_entry_info(fe_data)

            if info["file_type"] == FILE_TYPE_DIR:
                entries = self._read_directory_entries(fe_data)
                found = False
                for name, ftype, loc in entries:
                    if name.upper() == part.upper():  # case-insensitive for UDF
                        current_loc = loc
                        found = True
                        break
                if not found:
                    return None
            elif info["file_type"] == FILE_TYPE_FILE:
                return self._read_file_data(fe_data)
            else:
                return None

        # After traversal, read the file at current_loc
        fe_data = self._read_file_entry(current_loc)
        info = self._get_file_entry_info(fe_data)
        if info["file_type"] == FILE_TYPE_FILE:
            return self._read_file_data(fe_data)

        return None

    def list_directory(self, path: str) -> Optional[List[Tuple[str, bool]]]:
        """List contents of a directory by UDF path. Returns [(name, is_dir), ...]."""
        parts = path.strip("/").split("/")
        if self._root_icb is None:
            return None

        current_loc, _ = self._root_icb

        for part in parts:
            if not part:
                continue
            fe_data = self._read_file_entry(current_loc)
            info = self._get_file_entry_info(fe_data)
            if info["file_type"] != FILE_TYPE_DIR:
                return None
            entries = self._read_directory_entries(fe_data)
            found = False
            for name, ftype, loc in entries:
                if name.upper() == part.upper():
                    current_loc = loc
                    found = True
                    break
            if not found:
                return None

        # Now read directory at current_loc
        fe_data = self._read_file_entry(current_loc)
        info = self._get_file_entry_info(fe_data)
        if info["file_type"] != FILE_TYPE_DIR:
            return None
        entries = self._read_directory_entries(fe_data)
        return [(name, ftype == FILE_TYPE_DIR) for name, ftype, _ in entries]


# ---------------------------------------------------------------------------
# MPLS Parser — mirrors xbmc/filesystem/bluray/MPLSParser.cpp
# ---------------------------------------------------------------------------


class MPLSParser:
    """
    Parse Blu-ray MPLS (playlist) files.
    Mirrors CMPLSParser in MPLSParser.cpp.
    """

    def __init__(self, data: bytes):
        self.data = data
        self.playlist = None
        self.version = ""
        self.duration = 0  # milliseconds
        self.playback_type = 0
        self.playback_count = 0
        self.play_items: List[dict] = []
        self.clips: List[dict] = []
        self.sub_play_items: List[dict] = []
        self.extension_sub_play_items: List[dict] = []
        self.playlist_marks: List[dict] = []
        self.chapters: List[dict] = []

    def parse(self) -> bool:
        """Parse the MPLS file. Returns True on success."""
        data = self.data
        if len(data) < 40:
            return False

        header = data[0:4].decode("ascii", errors="replace")
        if header != "MPLS":
            return False
        self.version = data[4:8].decode("ascii", errors="replace")

        playlist_pos = u32(data, 8)
        playlist_mark_pos = u32(data, 12)
        ext_data_pos = u32(data, 16)

        # AppInfoPlayList
        app_info_size = u32(data, 40)
        if app_info_size > 0 and len(data) >= 40 + app_info_size:
            playback_type = u8(data, 40 + 5)
            self.playback_type = playback_type
            if playback_type in (2, 3):  # RANDOM, SHUFFLE
                self.playback_count = u16(data, 40 + 6)

        # Parse Playlist
        if playlist_pos > 0:
            if not self._parse_playlist(playlist_pos, data):
                return False

        # Parse PlayListMark
        if playlist_mark_pos > 0:
            self._parse_playlist_mark(playlist_mark_pos, data)

        # Parse extension data
        if ext_data_pos > 0:
            self._parse_extension_data(ext_data_pos, data)

        # Derive chapters
        self._derive_chapters()

        return True

    def _parse_playlist(self, pos: int, data: bytes) -> bool:
        playlist_size = u32(data, pos)
        if len(data) < pos + playlist_size:
            return False

        num_play_items = u16(data, pos + 4)
        num_sub_paths = u16(data, pos + 6)
        p = pos + 10

        if num_play_items > 0:
            self.play_items = []
            for _ in range(num_play_items):
                pi = self._parse_play_item(p, data)
                if pi is None:
                    return False
                self.play_items.append(pi)
                p = pi["_next_offset"]

            # Calculate duration
            total_ms = 0
            for pi in self.play_items:
                total_ms += pi["out_time"] - pi["in_time"]
            self.duration = total_ms

        if num_sub_paths > 0:
            for _ in range(num_sub_paths):
                sub_count = self._parse_sub_path(p, data)
                if sub_count is not None:
                    p = sub_count

        return True

    def _parse_play_item(self, pos: int, data: bytes) -> Optional[dict]:
        length = u16(data, pos)
        if length < 18:
            return None
        if len(data) < pos + length:
            return None

        clip_id = data[pos + 2: pos + 7].decode("ascii", errors="replace")
        codec_id = data[pos + 7: pos + 11].decode("ascii", errors="replace")
        if codec_id not in ("M2TS", "FMTS"):
            return None

        flags = u16(data, pos + 11)
        is_multi_angle = bool((flags >> 10) & 0x01)
        connection = (flags >> 6) & 0x0F

        in_time = u32(data, pos + 14) // 45  # 45 kHz clock
        out_time = u32(data, pos + 18) // 45

        flags2 = u8(data, pos + 30)
        random_access = bool((flags2 >> 7) & 0x01)
        still_mode = u8(data, pos + 31)
        still_time = 0
        if still_mode == 1:  # BLURAY_STILL_TIME
            still_time = u16(data, pos + 32)

        angle_count = 1
        if is_multi_angle:
            angle_count = u8(data, pos + 34)
            angle_offset = pos + 36
        else:
            angle_offset = pos + 34

        # Angle clips
        angle_clips = []
        clip_num = int(clip_id)
        angle_clips.append({"clip": clip_num, "codec": codec_id})
        for j in range(1, angle_count):
            a_clip_id = data[angle_offset: angle_offset + 5].decode("ascii", errors="replace")
            a_codec = data[angle_offset + 5: angle_offset + 9].decode("ascii", errors="replace")
            angle_clips.append({"clip": int(a_clip_id), "codec": a_codec})
            angle_offset += 10

        # STN table
        stn_offset = angle_offset
        stn_length = u16(data, stn_offset)
        if len(data) < stn_offset + stn_length:
            return None

        num_video = u8(data, stn_offset + 4)
        num_audio = u8(data, stn_offset + 5)
        num_pg = u8(data, stn_offset + 6)
        num_ig = u8(data, stn_offset + 7)
        num_sec_video = u8(data, stn_offset + 8)
        num_sec_audio = u8(data, stn_offset + 9)
        num_pip_sub = u8(data, stn_offset + 10)
        num_dv = u8(data, stn_offset + 11)

        st = stn_offset + 16

        video_streams = []
        for _ in range(num_video):
            info, st = self._parse_stream(st, data, 0)
            if info:
                video_streams.append(info)

        audio_streams = []
        for _ in range(num_audio):
            info, st = self._parse_stream(st, data, 1)
            if info:
                audio_streams.append(info)

        pg_streams = []
        for _ in range(num_pg):
            info, st = self._parse_stream(st, data, 2)
            if info:
                pg_streams.append(info)

        ig_streams = []
        for _ in range(num_ig):
            info, st = self._parse_stream(st, data, 3)
            if info:
                ig_streams.append(info)

        sec_audio_streams = []
        for _ in range(num_sec_audio):
            info, st = self._parse_stream(st, data, 5)
            if info:
                sec_audio_streams.append(info)

        sec_video_streams = []
        for _ in range(num_sec_video):
            info, st = self._parse_stream(st, data, 4)
            if info:
                sec_video_streams.append(info)

        dv_streams = []
        for _ in range(num_dv):
            info, st = self._parse_stream(st, data, 7)
            if info:
                dv_streams.append(info)

        return {
            "clip_id": clip_num,
            "codec_id": codec_id,
            "connection": connection,
            "is_multi_angle": is_multi_angle,
            "in_time": in_time,
            "out_time": out_time,
            "random_access": random_access,
            "still_mode": still_mode,
            "still_time": still_time,
            "angle_clips": angle_clips,
            "video_streams": video_streams,
            "audio_streams": audio_streams,
            "pg_streams": pg_streams,
            "ig_streams": ig_streams,
            "secondary_audio_streams": sec_audio_streams,
            "secondary_video_streams": sec_video_streams,
            "dolby_vision_streams": dv_streams,
            "_next_offset": stn_offset + stn_length + 2,
        }

    def _parse_stream(self, pos: int, data: bytes, stype: int) -> Tuple[Optional[dict], int]:
        """Parse one stream entry from the STN table."""
        if pos >= len(data):
            return None, pos

        # Stream entry
        length = u8(data, pos)
        if length == 0 or pos + length >= len(data):
            return None, pos + 1

        stream_type = u8(data, pos + 1)
        info = {
            "stream_type": stream_type,  # BLURAY_STREAM_TYPE
            "packet_identifier": 0,
            "subpath_id": 0,
            "subclip_id": 0,
        }

        if stream_type == 1:  # PLAYITEM
            info["packet_identifier"] = u16(data, pos + 2)
        elif stream_type == 2:  # SUBPATH
            info["subpath_id"] = u16(data, pos + 2)
            info["subclip_id"] = u16(data, pos + 4)
            info["packet_identifier"] = u16(data, pos + 6)
        elif stream_type in (3, 4):  # SUBPATH_INMUX_PIP, DV
            info["subpath_id"] = u16(data, pos + 2)
            info["packet_identifier"] = u16(data, pos + 4)

        pos += length + 1

        # Coding entry
        if pos >= len(data):
            return None, pos
        coding_len = u8(data, pos)
        if coding_len == 0 or pos + coding_len >= len(data):
            return None, pos + 1

        coding = u8(data, pos + 1)
        info["coding"] = coding

        if coding in (0x02, 0xea, 0x1b, 0x20, 0x24):  # Video
            flag1 = u8(data, pos + 2)
            info["format"] = (flag1 >> 4) & 0x0F  # video format (resolution)
            info["rate"] = flag1 & 0x0F  # frame rate
            if coding == 0x24:  # HEVC
                if pos + 4 < len(data):
                    flag2 = u8(data, pos + 3)
                    info["dynamic_range"] = (flag2 >> 4) & 0x0F
                    info["color_space"] = flag2 & 0x0F
                    flag3 = u8(data, pos + 4)
                    info["copy_restricted"] = (flag3 >> 7) & 0x01
                    info["hdr10_plus"] = (flag3 >> 6) & 0x01
        elif coding in (0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0xa1, 0xa2):  # Audio
            flag1 = u8(data, pos + 2)
            info["format"] = (flag1 >> 4) & 0x0F
            info["rate"] = flag1 & 0x0F
            audio_lang = data[pos + 3: pos + 6].decode("ascii", errors="replace")
            info["language"] = audio_lang.strip()
        elif coding in (0x90, 0x91):  # Subtitle
            sub_lang = data[pos + 2: pos + 5].decode("ascii", errors="replace")
            info["language"] = sub_lang.strip()
        elif coding == 0x92:  # Text subtitle
            info["character_encoding"] = u8(data, pos + 2)
            text_lang = data[pos + 3: pos + 6].decode("ascii", errors="replace")
            info["language"] = text_lang.strip()

        pos += coding_len + 1

        # Secondary references (for secondary audio/video streams)
        if stype in (4, 5):  # SECONDARY_VIDEO, SECONDARY_AUDIO
            pass  # Would parse references here

        return info, pos

    def _parse_sub_path(self, pos: int, data: bytes) -> Optional[int]:
        """Parse a SubPath entry."""
        if pos + 4 >= len(data):
            return None
        sub_path_size = u32(data, pos)
        if len(data) < pos + sub_path_size:
            return None

        num_sub_play_items = u8(data, pos + 9)
        p = pos + 10

        for _ in range(num_sub_play_items):
            spi = self._parse_sub_play_item(p, data)
            if spi is None:
                return pos + sub_path_size + 4
            self.sub_play_items.append(spi)
            p = spi["_next_offset"]

        return pos + sub_path_size + 4

    def _parse_sub_play_item(self, pos: int, data: bytes) -> Optional[dict]:
        length = u16(data, pos)
        if length < 24:
            return None

        clip_id = data[pos + 2: pos + 7].decode("ascii", errors="replace")
        codec_id = data[pos + 7: pos + 11].decode("ascii", errors="replace")
        if codec_id not in ("M2TS", "FMTS"):
            return None

        flags = u32(data, pos + 11)
        is_multi_clip = bool(flags & 0x40000000)
        connection = (flags >> 14) & 0x0F

        in_time = u32(data, pos + 16) // 45
        out_time = u32(data, pos + 20) // 45
        sync_play_item_id = u16(data, pos + 24)

        num_clips = 1
        if is_multi_clip:
            num_clips = u8(data, pos + 26)
            p = pos + 27
        else:
            p = pos + 26

        clips = [{"clip": int(clip_id), "codec": codec_id}]
        for j in range(1, num_clips):
            c_clip = data[p: p + 5].decode("ascii", errors="replace")
            c_codec = data[p + 5: p + 9].decode("ascii", errors="replace")
            clips.append({"clip": int(c_clip), "codec": c_codec})
            p += 10

        return {
            "connection": connection,
            "is_multi_clip": is_multi_clip,
            "in_time": in_time,
            "out_time": out_time,
            "sync_play_item_id": sync_play_item_id,
            "clips": clips,
            "_next_offset": pos + length + 2,
        }

    def _parse_playlist_mark(self, pos: int, data: bytes):
        """Parse PlayListMark section."""
        mark_size = u32(data, pos)
        if len(data) < pos + mark_size:
            return

        num_marks = u16(data, pos + 4)
        p = pos + 6

        self.playlist_marks = []
        for _ in range(num_marks):
            if p + 14 > len(data):
                break
            mark_type = u8(data, p + 1)
            play_item_ref = u16(data, p + 2)
            mark_time = u32(data, p + 4) // 45
            packet_id = u16(data, p + 8)
            mark_duration = u32(data, p + 10)

            self.playlist_marks.append({
                "mark_type": mark_type,
                "play_item_ref": play_item_ref,
                "time": mark_time,
                "packet_identifier": packet_id,
                "duration": mark_duration,
            })
            p += 14

    def _parse_extension_data(self, pos: int, data: bytes):
        """Parse extension data (for SubPath extensions)."""
        ext_size = u32(data, pos)
        if ext_size == 0 or len(data) < pos + ext_size:
            return

        num_entries = u8(data, pos + 11)
        p = pos + 12
        for _ in range(num_entries):
            if p + 12 > len(data):
                break
            ext_type = u16(data, p)
            ext_version = u16(data, p + 2)
            if ext_type == 2 and ext_version == 2:
                ext_start = u32(data, p + 4)
                ext_data_len = u32(data, p + 8)
                if pos + ext_start + ext_data_len <= len(data):
                    sp = pos + ext_start
                    num_sub = u16(data, sp + 4)
                    sp += 6
                    for _ in range(num_sub):
                        sub_count = self._parse_sub_path(sp, data)
                        if sub_count is not None:
                            sp = sub_count
                        if sub_count is None:
                            break
            p += 12

    def _derive_chapters(self):
        """Derive chapters from playlist marks."""
        # Update mark times relative to clips
        prev_chapter = -1
        for i, mark in enumerate(self.playlist_marks):
            if mark["mark_type"] == 1:  # ENTRY mark
                if prev_chapter >= 0:
                    prev_mark = self.playlist_marks[prev_chapter]
                    if prev_mark["duration"] == 0:
                        prev_mark["duration"] = mark["time"] - prev_mark["time"]
                prev_chapter = i

        if prev_chapter >= 0 and self.playlist_marks[prev_chapter]["duration"] == 0:
            self.playlist_marks[prev_chapter]["duration"] = self.duration - self.playlist_marks[prev_chapter]["time"]

        # Build chapters
        self.chapters = []
        chapter_num = 1
        for mark in self.playlist_marks:
            if mark["mark_type"] == 1:
                self.chapters.append({
                    "chapter": chapter_num,
                    "start": mark["time"],
                    "duration": mark["duration"],
                })
                chapter_num += 1


# ---------------------------------------------------------------------------
# CLPI Parser — mirrors xbmc/filesystem/bluray/MPLSParser.cpp::ParseCLPI
# ---------------------------------------------------------------------------


class CLPIParser:
    """
    Parse Blu-ray CLPI (clip information) files.
    """

    CLPI_HEADER_SIZE = 28

    def __init__(self, data: bytes, clip: int = 0):
        self.data = data
        self.clip = clip
        self.version = ""
        self.programs: List[dict] = []

    def parse(self) -> bool:
        data = self.data
        if len(data) < self.CLPI_HEADER_SIZE:
            return False

        header = data[0:4].decode("ascii", errors="replace")
        if header != "HDMV":
            return False
        self.version = data[4:8].decode("ascii", errors="replace")

        prog_info_start = u32(data, 12)
        offset = prog_info_start

        length = u32(data, offset)
        if len(data) < offset + length:
            return False

        num_programs = u8(data, offset + 5)
        offset += 6

        self.programs = []
        for _ in range(num_programs):
            prog = self._parse_program(data, offset)
            if prog is None:
                return False
            self.programs.append(prog)
            offset = prog["_next_offset"]

        return True

    def _parse_program(self, data: bytes, offset: int) -> Optional[dict]:
        spn = u32(data, offset)
        prog_id = u16(data, offset + 4)
        num_streams = u8(data, offset + 6)
        num_groups = u8(data, offset + 7)
        offset += 8

        streams = []
        for _ in range(num_streams):
            sid = u16(data, offset)
            offset += 2
            s_len = u8(data, offset)
            coding = u8(data, offset + 1)

            stream_info = {
                "packet_identifier": sid,
                "coding": coding,
                "format": 0,
                "rate": 0,
                "aspect": 0,
                "language": "",
                "hdr_type": 0,
                "color_space": 0,
                "hdr10_plus": False,
            }

            if coding in (0x02, 0xea, 0x1b, 0x20, 0x24):  # Video
                if s_len >= 4:
                    flag = u32(data, offset + 2)
                    stream_info["format"] = (flag >> 28) & 0x0F
                    stream_info["rate"] = (flag >> 24) & 0x0F
                    stream_info["aspect"] = (flag >> 20) & 0x0F
                    stream_info["out_of_mux"] = (flag >> 14) & 0x01
                    if coding == 0x24:  # HEVC
                        stream_info["copy_restricted"] = (flag >> 13) & 0x01
                        stream_info["hdr_type"] = (flag >> 12) & 0x0F
                        stream_info["color_space"] = (flag >> 8) & 0x0F
                        stream_info["hdr10_plus"] = (flag >> 4) & 0x01
            elif coding in (0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0xa1, 0xa2):  # Audio
                if s_len >= 2:
                    flag = u8(data, offset + 2)
                    stream_info["format"] = (flag >> 4) & 0x0F
                    stream_info["rate"] = flag & 0x0F
                    if s_len >= 3:
                        stream_info["language"] = data[offset + 3: offset + 6].decode("ascii", errors="replace").strip()
            elif coding in (0x90, 0x91):  # Subtitle
                if s_len >= 2:
                    stream_info["language"] = data[offset + 2: offset + 5].decode("ascii", errors="replace").strip()
            elif coding == 0x92:  # Text subtitle
                if s_len >= 3:
                    stream_info["character_encoding"] = u8(data, offset + 2)
                    stream_info["language"] = data[offset + 3: offset + 6].decode("ascii", errors="replace").strip()

            streams.append(stream_info)
            offset += s_len + 1

        return {
            "spn": spn,
            "program_id": prog_id,
            "num_groups": num_groups,
            "streams": streams,
            "_next_offset": offset,
        }


# ---------------------------------------------------------------------------
# Metadata Extraction — mirrors CBlurayDirectory + CStreamParser
# ---------------------------------------------------------------------------


def format_duration(ms: int) -> str:
    """Format milliseconds to HH:MM:SS."""
    total_seconds = ms // 1000
    hours = total_seconds // 3600
    minutes = (total_seconds % 3600) // 60
    seconds = total_seconds % 60
    return f"{hours:02d}:{minutes:02d}:{seconds:02d}"


def describe_video_stream(stream: dict) -> str:
    """Describe a video stream from MPLS + CLPI data."""
    coding = stream.get("coding", 0)
    codec_name = ENCODING_TYPE.get(coding, f"Unknown(0x{coding:02x})")
    fmt = stream.get("format", 0)
    resolution = BLURAY_VIDEO_FORMAT_NAMES.get(fmt, f"Format({fmt})")
    rate = stream.get("rate", 0)
    framerate = BLURAY_VIDEO_RATE_NAMES.get(rate, f"Rate({rate})")
    hdr = stream.get("hdr_type", 0)
    hdr_name = HDR_TYPE_NAMES.get(hdr, f"HDR({hdr})")
    hdr10p = " HDR10+" if stream.get("hdr10_plus") else ""

    return f"{codec_name} {resolution}@{framerate}fps {hdr_name}{hdr10p}"


def describe_audio_stream(stream: dict) -> str:
    """Describe an audio stream."""
    coding = stream.get("coding", 0)
    codec_name = ENCODING_TYPE.get(coding, f"Unknown(0x{coding:02x})")
    lang = stream.get("language", "??")
    return f"{codec_name} [{lang}]"


def describe_subtitle_stream(stream: dict) -> str:
    """Describe a subtitle stream."""
    coding = stream.get("coding", 0)
    lang = stream.get("language", "??")
    return f"Presentation Graphics [{lang}]"


# ---------------------------------------------------------------------------
# Main Test
# ---------------------------------------------------------------------------


DEFAULT_ISO_URL = (
    "http://127.0.0.4:5244/d/115/g-box/"
    "%E6%9A%B4%E5%8A%9B%E5%8F%B2%20.2005%20%5B%E7%89%B9%E6%95%88%E5%AD%97%E5%B9%95%5D"
    "%5B73.72GB%5D%5BUHD%E5%8E%9F%E7%9B%98%5D%20BluRay%202160P%20DoVi.HEVC%20DTS-HD%20MA%205.1"
    "-BHYS%40OurBits.iso"
)


def main():
    import argparse

    parser = argparse.ArgumentParser(
        description="Simulate Blu-ray metadata extraction from a remote ISO"
    )
    parser.add_argument("--iso-url", default=DEFAULT_ISO_URL, help="URL of the Blu-ray ISO")
    parser.add_argument("--playlist", type=int, default=None, help="Specific playlist number to parse")
    parser.add_argument("--verbose", "-v", action="store_true", help="Verbose output")
    args = parser.parse_args()

    iso_url = args.iso_url
    print(f"=== Blu-ray Metadata Extraction Test ===")
    print(f"ISO URL: {iso_url}")
    print()

    # Step 1: Initialize HTTP range fetcher
    print("[1] Connecting to ISO...")
    fetcher = RangeFetcher(iso_url)
    print(f"    Content-Length: {fetcher.content_length} bytes ({fetcher.content_length / (1024**3):.1f} GB)")
    print()

    # Step 2: Parse UDF filesystem
    print("[2] Parsing UDF filesystem...")
    try:
        udf = UDFReader(fetcher)
    except RuntimeError as e:
        print(f"    ERROR: {e}")
        sys.exit(1)
    print("    UDF filesystem parsed successfully")
    print()

    # Step 3: List BDMV directory structure
    print("[3] BDMV directory structure:")
    bdmv_dirs = udf.list_directory("BDMV")
    if bdmv_dirs is None:
        print("    ERROR: Cannot find BDMV directory")
        sys.exit(1)

    for name, is_dir in sorted(bdmv_dirs):
        if name and not name.startswith("."):
            suffix = "/" if is_dir else ""
            print(f"    {name}{suffix}")

    # Step 4: List PLAYLIST directory
    print()
    print("[4] Playlist files:")
    playlist_files = udf.list_directory("BDMV/PLAYLIST")
    if playlist_files is None:
        print("    ERROR: Cannot find BDMV/PLAYLIST directory")
        sys.exit(1)

    mpls_files = sorted([n for n, is_dir in playlist_files if n.endswith(".mpls")])
    for f in mpls_files:
        print(f"    {f}")

    if not mpls_files:
        print("    ERROR: No MPLS files found")
        sys.exit(1)

    # Step 5: List CLIPINF directory
    print()
    print("[5] Clip info files:")
    clipinf_files = udf.list_directory("BDMV/CLIPINF")
    if clipinf_files is None:
        print("    ERROR: Cannot find BDMV/CLIPINF directory")
        sys.exit(1)

    clpi_files = sorted([n for n, is_dir in clipinf_files if n.endswith(".clpi")])
    for f in clpi_files:
        print(f"    {f}")

    # Step 6: Parse MPLS files
    print()
    print("[6] Parsing playlists:")
    all_playlists = []
    target_playlist = args.playlist

    for mpls_name in mpls_files:
        playlist_num = int(mpls_name.replace(".mpls", ""))
        if target_playlist is not None and playlist_num != target_playlist:
            continue

        try:
            mpls_data = udf.find_file(f"BDMV/PLAYLIST/{mpls_name}")
        except Exception as e:
            print(f"    {mpls_name}: FETCH ERROR - {e}")
            continue

        if mpls_data is None:
            print(f"    {mpls_name}: FILE NOT FOUND")
            continue

        mpls = MPLSParser(mpls_data)
        if not mpls.parse():
            print(f"    {mpls_name}: PARSE ERROR")
            continue

        all_playlists.append(mpls)
        duration_str = format_duration(mpls.duration)
        playback_type = BLURAY_PLAYBACK_TYPE_NAMES.get(mpls.playback_type, f"Unknown({mpls.playback_type})")
        num_play_items = len(mpls.play_items)
        num_marks = len(mpls.playlist_marks)
        num_chapters = len(mpls.chapters)

        print(f"\n    --- {mpls_name} (Playlist {playlist_num}) ---")
        print(f"        Version:      {mpls.version}")
        print(f"        Duration:     {duration_str}")
        print(f"        Playback:     {playback_type}")
        print(f"        PlayItems:    {num_play_items}")
        print(f"        Marks:        {num_marks}")
        print(f"        Chapters:     {num_chapters}")

        # Show first play item's streams
        if mpls.play_items:
            pi = mpls.play_items[0]
            if pi["video_streams"]:
                print(f"        Video streams:")
                for vs in pi["video_streams"]:
                    print(f"          - {describe_video_stream(vs)}")
            if pi["audio_streams"]:
                print(f"        Audio streams:")
                for a in pi["audio_streams"]:
                    print(f"          - {describe_audio_stream(a)}")
            if pi["pg_streams"]:
                print(f"        Subtitle streams:")
                for s in pi["pg_streams"]:
                    print(f"          - {describe_subtitle_stream(s)}")
            if pi["dolby_vision_streams"]:
                print(f"        Dolby Vision streams:")
                for dv in pi["dolby_vision_streams"]:
                    print(f"          - PID 0x{dv.get('packet_identifier', 0):04x}, coding 0x{dv.get('coding', 0):02x}")

    # Step 7: Parse CLPI files for the first playlist's clips
    print()
    print("[7] Parsing clip info (CLPI) for first playlist:")
    if all_playlists:
        first_pl = all_playlists[0]
        if first_pl.play_items:
            for pi_item in first_pl.play_items:
                clip_id = pi_item["clip_id"]
                clpi_name = f"{clip_id:05d}.clpi"
                if clpi_name in clpi_files:
                    try:
                        clpi_data = udf.find_file(f"BDMV/CLIPINF/{clpi_name}")
                    except Exception as e:
                        print(f"    {clpi_name}: FETCH ERROR - {e}")
                        continue

                    if clpi_data is None:
                        print(f"    {clpi_name}: FILE NOT FOUND")
                        continue

                    clpi = CLPIParser(clpi_data, clip_id)
                    if not clpi.parse():
                        print(f"    {clpi_name}: PARSE ERROR")
                        continue

                    print(f"\n    --- {clpi_name} (Clip {clip_id}) ---")
                    print(f"        Version: {clpi.version}")
                    for prog_idx, prog in enumerate(clpi.programs):
                        print(f"        Program {prog_idx}: {len(prog['streams'])} streams")
                        for s in prog["streams"]:
                            coding = s.get("coding", 0)
                            if coding in (0x02, 0xea, 0x1b, 0x20, 0x24):
                                print(f"          Video: {describe_video_stream(s)}")
                            elif coding in (0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0xa1, 0xa2):
                                print(f"          Audio: {describe_audio_stream(s)}")
                            elif coding in (0x90, 0x91, 0x92):
                                print(f"          Sub:   {describe_subtitle_stream(s)}")
                            else:
                                print(f"          Stream: coding=0x{coding:02x}")

    # Step 8: Summary
    print()
    print("=== Summary ===")
    print(f"Total playlists: {len(all_playlists)}")

    # Find the longest playlist (main movie candidate)
    if all_playlists:
        longest = max(all_playlists, key=lambda p: p.duration)
        longest_idx = all_playlists.index(longest)
        print(f"Longest playlist: {longest.playlist if longest.playlist else 'N/A'}"
              f" ({format_duration(longest.duration)}) at index {longest_idx}")

        # Show main movie metadata
        print()
        print("Main Movie Metadata:")
        main_pl = longest
        print(f"  Playlist ID:  {main_pl.playlist}")
        print(f"  Duration:     {format_duration(main_pl.duration)}")
        print(f"  Chapters:     {len(main_pl.chapters)}")

        if main_pl.play_items:
            pi = main_pl.play_items[0]
            if pi["video_streams"]:
                print(f"  Video:        {describe_video_stream(pi['video_streams'][0])}")
            if pi["audio_streams"]:
                print(f"  Audio:        {', '.join(describe_audio_stream(a) for a in pi['audio_streams'])}")
            if pi["pg_streams"]:
                print(f"  Subtitles:    {', '.join(describe_subtitle_stream(s) for s in pi['pg_streams'])}")
            if pi["dolby_vision_streams"]:
                print(f"  Dolby Vision: {len(pi['dolby_vision_streams'])} stream(s)")

    # Step 9: Sanity check — verify metadata against filename expectations
    print()
    print("=== Verification ===")
    filename_checks = [
        ("2160P", "4K resolution (2160p)"),
        ("DoVi", "Dolby Vision HDR"),
        ("HEVC", "HEVC/H.265 video codec"),
        ("DTS-HD", "DTS-HD audio"),
    ]
    for keyword, desc in filename_checks:
        if keyword in iso_url:
            print(f"  [EXPECTED] {desc} — checking...")
            found = False
            if all_playlists:
                longest = max(all_playlists, key=lambda p: p.duration)
                if longest.play_items:
                    pi = longest.play_items[0]
                    if keyword == "2160P":
                        for vs in pi["video_streams"]:
                            if vs.get("format") == 7:
                                found = True
                                break
                    elif keyword == "DoVi":
                        if pi["dolby_vision_streams"]:
                            found = True
                        for vs in pi["video_streams"]:
                            if vs.get("hdr_type") == 3:
                                found = True
                    elif keyword == "HEVC":
                        for vs in pi["video_streams"]:
                            if vs.get("coding") == 0x24:
                                found = True
                                break
                    elif keyword == "DTS-HD":
                        for a in pi["audio_streams"]:
                            if a.get("coding") in (0x85, 0x86):
                                found = True
                                break
            if found:
                print(f"    ✓ VERIFIED")
            else:
                print(f"    ✗ NOT FOUND (may need deeper analysis)")

    print()
    print("=== Test Complete ===")


if __name__ == "__main__":
    main()