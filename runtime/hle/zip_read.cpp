// SPDX-License-Identifier: MIT
// Zip reading (see zip_read.h). Moved out of save_port.cpp on 2026-11-15 so mod packs can be zips.
#include "zip_read.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <istream>
#include <ostream>

namespace hle {
namespace zipr {

namespace fs = std::filesystem;

static std::string S(const fs::path& p) { auto u = p.u8string(); return std::string(u.begin(), u.end()); }

uint32_t crc32_update(uint32_t crc, const uint8_t* p, size_t n) {
  static uint32_t tab[256];
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; ++i) { uint32_t c = i; for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1; tab[i] = c; }
    init = true;
  }
  crc = ~crc;
  for (size_t i = 0; i < n; ++i) crc = tab[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

namespace {

struct BitIn {
  std::ifstream& f;
  uint64_t remaining;   // compressed bytes left
  uint8_t buf[1 << 16];
  size_t pos = 0, len = 0;
  uint32_t bitbuf = 0;
  int bitcnt = 0;
  bool err = false;
  BitIn(std::ifstream& s, uint64_t n) : f(s), remaining(n) {}
  int byte() {
    if (pos == len) {
      size_t want = (size_t)std::min<uint64_t>(remaining, sizeof buf);
      if (!want) { err = true; return 0; }
      f.read((char*)buf, (std::streamsize)want);
      len = (size_t)f.gcount();
      remaining -= len;
      pos = 0;
      if (!len) { err = true; return 0; }
    }
    return buf[pos++];
  }
  uint32_t bits(int n) {
    while (bitcnt < n) { bitbuf |= (uint32_t)byte() << bitcnt; bitcnt += 8; }
    uint32_t v = bitbuf & ((1u << n) - 1);
    bitbuf >>= n;
    bitcnt -= n;
    return v;
  }
};

struct OutWin {
  std::ofstream& f;
  uint8_t ring[1 << 16];
  uint64_t total = 0, flushed = 0;
  uint32_t crc = 0;
  bool err = false;
  uint64_t limit = ~0ull;
  explicit OutWin(std::ofstream& s) : f(s) {}
  void put(uint8_t b) {
    if (err) return;
    if (total >= limit) { err = true; return; }
    ring[total & 0xFFFF] = b;
    ++total;
    if (total - flushed == 0x8000) drain();
  }
  void drain() {
    size_t n = (size_t)(total - flushed);
    if (!n) return;
    const uint8_t* p = ring + (flushed & 0xFFFF);
    f.write((const char*)p, (std::streamsize)n);
    if (!f) err = true;
    crc = crc32_update(crc, p, n);
    flushed = total;
  }
};

struct Huff { uint16_t count[16]; uint16_t symbol[320]; };

int huff_build(Huff& h, const uint8_t* len, int n) {
  for (int i = 0; i < 16; ++i) h.count[i] = 0;
  for (int s = 0; s < n; ++s) h.count[len[s]]++;
  if (h.count[0] == n) return 0;
  int left = 1;
  for (int l = 1; l <= 15; ++l) { left <<= 1; left -= h.count[l]; if (left < 0) return left; }
  uint16_t offs[16];
  offs[1] = 0;
  for (int l = 1; l < 15; ++l) offs[l + 1] = (uint16_t)(offs[l] + h.count[l]);
  for (int s = 0; s < n; ++s) if (len[s]) h.symbol[offs[len[s]]++] = (uint16_t)s;
  return left;
}

int huff_decode(BitIn& in, const Huff& h) {
  int code = 0, first = 0, index = 0;
  for (int l = 1; l <= 15; ++l) {
    code |= (int)in.bits(1);
    int count = h.count[l];
    if (code - count < first) return h.symbol[index + (code - first)];
    index += count;
    first += count;
    first <<= 1;
    code <<= 1;
    if (in.err) return -1;
  }
  return -1;
}

const uint16_t kLBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint16_t kLExt[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const uint16_t kDExt[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool inflate_codes(BitIn& in, OutWin& out, const Huff& lc, const Huff& dc) {
  for (;;) {
    int sym = huff_decode(in, lc);
    if (sym < 0 || in.err) return false;
    if (sym < 256) { out.put((uint8_t)sym); if (out.err) return false; continue; }
    if (sym == 256) return true;
    sym -= 257;
    if (sym >= 29) return false;
    int len = kLBase[sym] + (int)in.bits(kLExt[sym]);
    int ds = huff_decode(in, dc);
    if (ds < 0 || ds >= 30) return false;
    uint32_t dist = kDBase[ds] + in.bits(kDExt[ds]);
    if (dist > out.total || dist > 32768) return false;
    for (int i = 0; i < len; ++i) out.put(out.ring[(out.total - dist) & 0xFFFF]);
    if (out.err || in.err) return false;
  }
}

bool inflate_stream(BitIn& in, OutWin& out) {
  int last;
  do {
    last = (int)in.bits(1);
    int type = (int)in.bits(2);
    if (type == 0) {
      in.bitbuf = 0; in.bitcnt = 0;
      uint32_t len = (uint32_t)in.byte(); len |= (uint32_t)in.byte() << 8;
      uint32_t nlen = (uint32_t)in.byte(); nlen |= (uint32_t)in.byte() << 8;
      if (in.err || (len ^ 0xFFFF) != nlen) return false;
      for (uint32_t i = 0; i < len; ++i) { out.put((uint8_t)in.byte()); if (in.err || out.err) return false; }
    } else if (type == 1) {
      static Huff lc, dc;
      static bool init = false;
      if (!init) {
        uint8_t l[288];
        int s = 0;
        for (; s < 144; ++s) l[s] = 8;
        for (; s < 256; ++s) l[s] = 9;
        for (; s < 280; ++s) l[s] = 7;
        for (; s < 288; ++s) l[s] = 8;
        huff_build(lc, l, 288);
        uint8_t d[30];
        for (s = 0; s < 30; ++s) d[s] = 5;
        huff_build(dc, d, 30);
        init = true;
      }
      if (!inflate_codes(in, out, lc, dc)) return false;
    } else if (type == 2) {
      static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
      int nlen = (int)in.bits(5) + 257, ndist = (int)in.bits(5) + 1, ncode = (int)in.bits(4) + 4;
      if (nlen > 286 || ndist > 30) return false;
      uint8_t lengths[320] = {};
      for (int i = 0; i < ncode; ++i) lengths[order[i]] = (uint8_t)in.bits(3);
      Huff cl;
      if (huff_build(cl, lengths, 19) != 0) return false;
      int idx = 0;
      uint8_t ll[320] = {};
      while (idx < nlen + ndist) {
        int sym = huff_decode(in, cl);
        if (sym < 0 || in.err) return false;
        if (sym < 16) ll[idx++] = (uint8_t)sym;
        else {
          int rep, val = 0;
          if (sym == 16) { if (idx == 0) return false; val = ll[idx - 1]; rep = 3 + (int)in.bits(2); }
          else if (sym == 17) rep = 3 + (int)in.bits(3);
          else rep = 11 + (int)in.bits(7);
          if (idx + rep > nlen + ndist) return false;
          while (rep--) ll[idx++] = (uint8_t)val;
        }
      }
      if (ll[256] == 0) return false;
      Huff lc, dc;
      int e = huff_build(lc, ll, nlen);
      if (e < 0 || (e > 0 && nlen - lc.count[0] != 1)) return false;
      e = huff_build(dc, ll + nlen, ndist);
      if (e < 0 || (e > 0 && ndist - dc.count[0] != 1)) return false;
      if (!inflate_codes(in, out, lc, dc)) return false;
    } else {
      return false;
    }
  } while (!last);
  out.drain();
  return !in.err && !out.err;
}

uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
uint64_t rd64(const uint8_t* p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }

}  // namespace

bool list(const fs::path& zp, std::vector<Entry>& out, std::string* err) {
  std::ifstream f(zp, std::ios::binary);
  if (!f) { *err = "The zip file could not be opened: " + S(zp); return false; }
  f.seekg(0, std::ios::end);
  const uint64_t fsize = (uint64_t)f.tellg();
  if (fsize < 22) { *err = "That file is not a zip archive (too small)."; return false; }
  const uint64_t tail = std::min<uint64_t>(fsize, 65557);
  std::vector<uint8_t> t((size_t)tail);
  f.seekg((std::streamoff)(fsize - tail));
  f.read((char*)t.data(), (std::streamsize)tail);
  int64_t e = -1;
  for (int64_t i = (int64_t)tail - 22; i >= 0; --i) if (rd32(&t[(size_t)i]) == 0x06054b50u) { e = i; break; }
  if (e < 0) { *err = "That file is not a zip archive (no end-of-archive record)."; return false; }
  uint64_t cd_off = rd32(&t[(size_t)e + 16]), cd_size = rd32(&t[(size_t)e + 12]), count = rd16(&t[(size_t)e + 10]);
  if (cd_off == 0xFFFFFFFFu || cd_size == 0xFFFFFFFFu || count == 0xFFFF) {
    if (e < 20 || rd32(&t[(size_t)e - 20]) != 0x07064b50u) { *err = "That zip file is damaged (zip64 record missing)."; return false; }
    uint64_t o64 = rd64(&t[(size_t)e - 20 + 8]);
    uint8_t z[56];
    f.seekg((std::streamoff)o64);
    f.read((char*)z, 56);
    if (f.gcount() != 56 || rd32(z) != 0x06064b50u) { *err = "That zip file is damaged (zip64 record unreadable)."; return false; }
    count = rd64(z + 32); cd_size = rd64(z + 40); cd_off = rd64(z + 48);
  }
  if (cd_off + cd_size > fsize || cd_size > (1ull << 30)) { *err = "That zip file is damaged (directory out of range)."; return false; }
  std::vector<uint8_t> cd((size_t)cd_size);
  f.seekg((std::streamoff)cd_off);
  f.read((char*)cd.data(), (std::streamsize)cd_size);
  if ((uint64_t)f.gcount() != cd_size) { *err = "That zip file is damaged (directory unreadable)."; return false; }
  constexpr uint64_t kMaxFiles = 200000;
  uint64_t total_us = 0, files = 0;
  size_t p = 0;
  for (uint64_t k = 0; k < count; ++k) {
    if (p + 46 > cd.size() || rd32(&cd[p]) != 0x02014b50u) { *err = "That zip file is damaged (bad directory entry)."; return false; }
    uint16_t flags = rd16(&cd[p + 8]), method = rd16(&cd[p + 10]);
    uint32_t crc = rd32(&cd[p + 16]);
    uint64_t cs = rd32(&cd[p + 20]), us = rd32(&cd[p + 24]), lho = rd32(&cd[p + 42]);
    size_t nl = rd16(&cd[p + 28]), el = rd16(&cd[p + 30]), cl = rd16(&cd[p + 32]);
    if (p + 46 + nl + el + cl > cd.size()) { *err = "That zip file is damaged (entry overruns the directory)."; return false; }
    std::string name((const char*)&cd[p + 46], nl);
    for (char& c : name) if (c == '\\') c = '/';
    const uint8_t* ex = &cd[p + 46 + nl];
    for (size_t q = 0; q + 4 <= el;) {
      uint16_t id = rd16(ex + q), sz = rd16(ex + q + 2);
      if (q + 4 + sz > el) break;
      if (id == 1) {
        size_t o = q + 4;
        if (us == 0xFFFFFFFFu && o + 8 <= q + 4 + sz) { us = rd64(ex + o); o += 8; }
        if (cs == 0xFFFFFFFFu && o + 8 <= q + 4 + sz) { cs = rd64(ex + o); o += 8; }
        if (lho == 0xFFFFFFFFu && o + 8 <= q + 4 + sz) { lho = rd64(ex + o); o += 8; }
      }
      q += 4 + sz;
    }
    p += 46 + nl + el + cl;
    if (name.empty() || name.back() == '/') continue;
    if (flags & 1) { *err = "That zip file is password protected."; return false; }
    if (++files > kMaxFiles) { *err = "That zip file has too many files (more than " + std::to_string(kMaxFiles) + ")."; return false; }
    total_us += us;
    if (us > (4ull << 30) || total_us > (4ull << 30)) { *err = "That zip file expands to more than 4 GB, so it was refused."; return false; }
    Entry se;
    se.rel = name; se.size = us; se.csize = cs; se.lho = lho; se.method = method; se.crc = crc;
    out.push_back(se);
  }
  return true;
}

bool extract(const fs::path& zip, const Entry& e, const fs::path& dst, std::string* err) {
  std::error_code ec;
  std::ifstream in(zip, std::ios::binary);
  uint8_t lh[30];
  in.seekg((std::streamoff)e.lho);
  in.read((char*)lh, 30);
  if (in.gcount() != 30 || rd32(lh) != 0x04034b50u) { *err = "The zip file is damaged (entry " + e.rel + " not found)."; return false; }
  in.seekg((std::streamoff)(e.lho + 30 + rd16(lh + 26) + rd16(lh + 28)));
  std::ofstream out(dst, std::ios::binary | std::ios::trunc);
  if (!out) { *err = "Could not write " + S(dst); return false; }
  uint32_t crc = 0;
  uint64_t written = 0;
  bool ok = false;
  if (e.method == 0) {
    std::vector<uint8_t> b(1 << 20);
    uint64_t left = e.csize;
    ok = true;
    while (left && ok) {
      size_t n = (size_t)std::min<uint64_t>(left, b.size());
      in.read((char*)b.data(), (std::streamsize)n);
      if ((size_t)in.gcount() != n) { ok = false; break; }
      out.write((const char*)b.data(), (std::streamsize)n);
      crc = crc32_update(crc, b.data(), n);
      left -= n; written += n;
    }
  } else if (e.method == 8) {
    BitIn bi(in, e.csize);
    OutWin ow(out);
    ow.limit = e.size;
    ok = inflate_stream(bi, ow);
    crc = ow.crc; written = ow.total;
  } else {
    *err = "The zip file uses an unsupported compression method for " + e.rel + ".";
    return false;
  }
  out.flush();
  if (!ok || !out) { *err = "The zip file is damaged or the disk is full (while reading " + e.rel + ")."; return false; }
  if (written != e.size || crc != e.crc) { *err = "The zip file is damaged: " + e.rel + " does not match its checksum."; return false; }
  return true;
}

}  // namespace zipr
}  // namespace hle
