// SPDX-License-Identifier: MIT
// Loose-file DRP entry mods (project code, ours): pack discovery, the (archive, entry) index, path mapping and the
// replacement-chunk builder. Host-only, no dependency on the lifted game; tests/mods/mods_tests.cpp covers it.
// Design and user guide: docs/mods.md.
#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mods {

struct Pack {
  std::string id;            // folder name under the mods root, or the .zip's name without ".zip"
  std::string dir;           // absolute path (for a zip: its unpacked copy under <root>/.unpacked/<id>)
  std::string name, version, author, description;
  bool enabled = true;
  bool zip = false;          // the pack is <root>/<id>.zip
};

struct Override {
  std::string file;          // absolute path of the replacement
  int pack = 0;              // index into Index::packs
  std::string label;         // "archive/entry" for logs
  std::string rel;           // the file's path inside its pack
};

// Two enabled packs change the same file: `path` is the file inside the pack ("romfs/..." for whole files); the
// winner is the pack higher in mods.json. Indexes into Index::packs.
struct Conflict {
  std::string path;
  int winner = 0, loser = 0;
};

uint32_t crc32(const void* data, size_t n);
// DRP entry extension id: crc32 of the lowercase extension (verified: "nutexb" -> 0x5c156dbc). An 8-digit hex extension
// (the unpacker's name for ids it does not know) is taken as the id itself.
uint32_t ext_crc(const std::string& ext);
// "Common/UI\\x.drp" or "rom:/common/UI/x.drp" -> "common/ui/x.drp"
std::string norm_archive(const std::string& a);
// A path inside a pack ("common/UI/master/Foo.drp/icon.nutexb", forward slashes) -> archive, entry stem and ext id. The
// first path component that ends in ".drp" ends the archive path; exactly one file name must follow it. A trailing
// ".<ext>" is dropped from the stem when it repeats the extension (the game stores some names with their extension).
bool parse_pack_path(const std::string& rel, std::string* archive, std::string* stem, uint32_t* extcrc);
std::string make_key(const std::string& archive_norm, const std::string& stem_lower, uint32_t extcrc);

class Index {
 public:
  std::vector<Pack> packs;
  std::unordered_map<std::string, Override> map;
  std::vector<Conflict> conflicts;     // every file two enabled packs change, in the order found
  std::vector<std::pair<std::string, int>> rom_layers;   // (<pack>/romfs folder, pack index) of enabled packs, priority order:
                                                          // whole files that shadow rom:/ (e.g. an archive with added entries)
  bool empty() const { return map.empty(); }
  void add(int pack, const std::string& archive, const std::string& stem, uint32_t extcrc, const std::string& file, const std::string& rel = "");
  // `name` is the entry name as stored in the chunk; the plain name and the name minus a ".<ext>" suffix whose crc is
  // the chunk's ext id are both tried.
  const Override* find(const std::string& archive_norm, const std::string& name, uint32_t extcrc) const;
};

// Mods root: SNAP_MODS_DIR, else %APPDATA%\SnapRecomp\mods.
std::string default_root();
// Reads <root>/mods.json (priority order, "enabled" flags; without the file nothing is enabled except the packs the game needs (native-options),
// in name order), each pack.json, and indexes the files of enabled packs. Problems go to `warnings`.
// unpack = false (the launcher's conflict list): enabled zips are read from their file list and not unpacked, so the
// index has the same keys and conflicts but no servable files or romfs layers for them.
void load(const std::string& root, Index& out, std::vector<std::string>& warnings, bool unpack = true);

// ---- Zipped packs. A pack may be a folder or <root>/<id>.zip with the same layout inside (at the top of the zip, or in
// one folder at its top). The zip is never changed: an enabled zip is unpacked once into <root>/.unpacked/<id>/ and read
// from there; a changed zip (size or time) is unpacked again. Deleting .unpacked is always safe.
// Every pack on disk (folders with a pack.json, and .zip files), by id, in name order. ".unpacked" is skipped.
std::vector<std::string> packs_on_disk(const std::string& root);
// Fills p (id, zip, name, version, author, description) for the pack `id` without unpacking a zip (pack.json is read
// from the zip). p.dir is the folder, or for a zip its unpacked copy, which may not exist yet. False if there is no such pack.
bool pack_info(const std::string& root, const std::string& id, Pack& p, std::vector<std::string>& warnings);
// Unpacks <root>/<id>.zip into <root>/.unpacked/<id>/ unless that copy is current; returns the copy's folder, or "" (with
// a warning) when the zip is damaged or holds a path that would leave the folder.
std::string unpack_zip(const std::string& root, const std::string& id, std::vector<std::string>& warnings);
// True when `id` is a plain name for one folder or zip directly under the mods root (no separators, no trailing dot or
// space, no DOS device name). Every id from mods.json or the command line goes through it before touching the disk.
bool safe_pack_id(const std::string& id);

// Replacement chunk for the DRP entry worker: the original chunk's header (words, name and padding, `hdr_len` bytes)
// followed by the raw data, stored the way the game stores uncompressed entries (csize == dsize) with the data's CRC32.
// The size fields are rewritten, so the data may be any length.
std::vector<uint8_t> build_chunk(const uint8_t* orig, size_t hdr_len, const uint8_t* data, size_t n);
// Header length of a chunk with a NUL-terminated name of `name_len` bytes at +0x18: 0x18 + ((len + 4) & ~3)
size_t chunk_header_len(size_t name_len);

// ---- Texture capture for the builder's AI texture upscaling (SNAP_TEXDUMP; docs/hd-textures-ai.md). Each nutexb the
// game loads is saved once, decompressed, at <root>/<archive>/<entry name>: the same layout as a pack folder.
// Root: SNAP_TEXDUMP_DIR, else %LOCALAPPDATA%\SnapRecomp\texcache.
std::string texdump_root();
// On when SNAP_TEXDUMP is set to anything but "0"; when it is unset, on while <root>\capture.on exists (the builder
// creates that file so normal play keeps filling the cache).
bool texdump_enabled();
// Relative cache path of one entry: "<archive_norm>/<name>", with ".nutexb" added when the name has no extension.
// Empty when the name could leave the folder (separators, "..", ':', control characters) or is empty.
std::string texdump_rel(const std::string& archive_norm, const std::string& name);

}  // namespace mods
