// Added by acs (fork of CidVonHighwind/microreader), 2026-10-08: book cover as sleep image.
#include "CoverSleep.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

#include "../HeapLog.h"
#include "Book.h"
#include "ImageDecoder.h"
#include "ZipReader.h"

namespace microreader {

namespace {

// MGR2 physical frame: 800x480, 2 bits per pixel (0=white, 1=light, 2=dark, 3=black).
constexpr int kPhysW = 800;
constexpr int kPhysH = 480;
constexpr int kStride = kPhysW / 4;            // 200 bytes per row
constexpr int kRowsPerPlane = kPhysH / 2;      // 240 rows in each scratch plane
constexpr size_t kPlaneBytes = static_cast<size_t>(kStride) * kRowsPerPlane;  // 48000

// Logical (portrait) screen the cover is fitted into.
constexpr int kScreenW = 480;
constexpr int kScreenH = 800;

bool is_image_name(std::string_view name) {
  return guess_format(std::string(name).c_str()) != ImageFormat::Unknown;
}

bool contains_cover(std::string_view name) {
  static constexpr char kNeedle[] = "cover";
  const size_t n = sizeof(kNeedle) - 1;
  for (size_t i = 0; i + n <= name.size(); ++i) {
    size_t k = 0;
    while (k < n && std::tolower(static_cast<unsigned char>(name[i + k])) == kNeedle[k])
      ++k;
    if (k == n)
      return true;
  }
  return false;
}

// Cover declared in the OPF if it is an image, otherwise the largest image
// whose path contains "cover" (covers declared as an XHTML page, or missing).
int find_cover_entry(const Epub& epub) {
  const ZipReader& zip = epub.zip();
  const int declared = epub.cover_index();
  if (declared >= 0 && static_cast<size_t>(declared) < zip.entry_count() && is_image_name(zip.entry(declared).name))
    return declared;
  int best = -1;
  uint32_t best_size = 0;
  for (size_t i = 0; i < zip.entry_count(); ++i) {
    const ZipEntry& e = zip.entry(i);
    if (contains_cover(e.name) && is_image_name(e.name) && e.uncompressed_size > best_size) {
      best = static_cast<int>(i);
      best_size = e.uncompressed_size;
    }
  }
  return best;
}

// Collects the 2x-resolution dithered rows into the 2-bpp physical frame.
struct Canvas {
  uint8_t* plane[2];
  int off_x = 0, off_y = 0;  // top-left of the cover on the portrait screen
  int out_w = 0, out_h = 0;  // cover size in screen pixels
  int hi_w = 0;              // width of the decoded (2x) rows
  uint8_t count[kScreenW] = {};
  int pending_row = -1;      // output row accumulated so far (-1 = none)
  int pending_subrows = 0;   // how many 2x rows went into it (1 or 2)

  // Portrait pixel (x, y) -> physical frame: rotated like convert_bmp_to_mgr2.
  void set(int x, int y, uint8_t level) {
    const int px = y;
    const int py = (kScreenW - 1) - x;
    if (px < 0 || px >= kPhysW || py < 0 || py >= kPhysH)
      return;
    uint8_t* row = plane[py / kRowsPerPlane] + static_cast<size_t>(py % kRowsPerPlane) * kStride;
    const int shift = 6 - (px % 4) * 2;
    row[px / 4] = static_cast<uint8_t>((row[px / 4] & ~(3 << shift)) | (level << shift));
  }

  void flush() {
    if (pending_row < 0)
      return;
    const int y = pending_row;
    for (int x = 0; x < out_w; ++x) {
      const int cols = (2 * x + 1 < hi_w) ? 2 : 1;
      const int samples = cols * pending_subrows;
      // White sub-pixels scaled to 0..4.
      const int white = (count[x] * 4 + samples / 2) / samples;
      uint8_t level;
      switch (white) {
        case 4: level = 0; break;                                  // white
        case 3: level = 1; break;                                  // light grey
        case 2: level = ((off_x + x + off_y + y) & 1) ? 1 : 2; break;  // mid grey
        case 1: level = 2; break;                                  // dark grey
        default: level = 3; break;                                 // black
      }
      set(off_x + x, off_y + y, level);
    }
    pending_row = -1;
    pending_subrows = 0;
  }

  void add_row(uint16_t hy, const uint8_t* data, uint16_t width) {
    const int y = hy / 2;
    if (y != pending_row) {
      flush();
      if (y >= out_h)
        return;
      std::memset(count, 0, sizeof(count));
      pending_row = y;
    }
    const int w = width < 2 * out_w ? width : 2 * out_w;
    for (int hx = 0; hx < w; ++hx)
      if ((data[hx >> 3] >> (7 - (hx & 7))) & 1)
        ++count[hx >> 1];
    ++pending_subrows;
    if (pending_subrows == 2)
      flush();
  }
};

}  // namespace

CoverResult make_cover_sleep_image(const char* epub_path, const char* out_path, uint8_t* plane0, uint8_t* plane1,
                                   size_t plane_size) {
  if (!epub_path || !out_path || !plane0 || !plane1 || plane_size < kPlaneBytes)
    return CoverResult::Failed;

  // 1. Find the cover and its size. The display planes serve as the parser's
  //    work buffers; the book is closed again before decoding to free its memory.
  std::string entry_name;
  uint32_t entry_offset = 0;
  uint16_t src_w = 0, src_h = 0;
  {
    Book book;
    if (book.open(epub_path, plane0, plane1, /*parse_css_ncx=*/false) != EpubError::Ok) {
      MR_LOGI("cover", "cannot open '%s'", epub_path);
      return CoverResult::Failed;
    }
    const int idx = find_cover_entry(book.epub());
    if (idx < 0) {
      MR_LOGI("cover", "no cover in '%s'", epub_path);
      return CoverResult::NoCover;
    }
    const ZipEntry& e = book.epub().zip().entry(static_cast<size_t>(idx));
    entry_name.assign(e.name.data(), e.name.size());
    entry_offset = e.local_header_offset;
    if (!book.read_image_size(static_cast<uint16_t>(idx), src_w, src_h, plane0, plane_size) || !src_w || !src_h) {
      MR_LOGI("cover", "unreadable cover '%s'", entry_name.c_str());
      return CoverResult::NoCover;
    }
  }

  // 2. Fit the cover into the screen; decode it at twice that size.
  int hi_w, hi_h;
  if (static_cast<uint32_t>(src_w) * (2 * kScreenH) > static_cast<uint32_t>(src_h) * (2 * kScreenW)) {
    hi_w = 2 * kScreenW;
    hi_h = static_cast<int>(static_cast<uint32_t>(src_h) * hi_w / src_w);
  } else {
    hi_h = 2 * kScreenH;
    hi_w = static_cast<int>(static_cast<uint32_t>(src_w) * hi_h / src_h);
  }
  if (hi_w < 2)
    hi_w = 2;
  if (hi_h < 2)
    hi_h = 2;

  Canvas canvas;
  canvas.plane[0] = plane0;
  canvas.plane[1] = plane1;
  canvas.hi_w = hi_w;
  canvas.out_w = (hi_w + 1) / 2;
  canvas.out_h = (hi_h + 1) / 2;
  canvas.off_x = (kScreenW - canvas.out_w) / 2;
  canvas.off_y = (kScreenH - canvas.out_h) / 2;
  std::memset(plane0, 0, kPlaneBytes);  // white background
  std::memset(plane1, 0, kPlaneBytes);

  StdioZipFile file;
  if (!file.open(epub_path))
    return CoverResult::Failed;
  ZipEntry entry;
  if (ZipReader::read_local_entry(file, entry_offset, entry) != ZipError::Ok)
    return CoverResult::Failed;
  entry.name = entry_name;  // lets the decoder pick the format from the extension

  ImageRowSink sink;
  sink.ctx = &canvas;
  sink.emit_row = [](void* c, uint16_t y, const uint8_t* data, uint16_t width) {
    static_cast<Canvas*>(c)->add_row(y, data, width);
  };
  // Adam7 (interlaced) PNGs deliver single pixels: keep one per 2x2 block.
  ImagePixelSink pixel_sink;
  pixel_sink.ctx = &canvas;
  pixel_sink.set_pixel = [](void* c, uint16_t x, uint16_t y, bool white) {
    auto* cv = static_cast<Canvas*>(c);
    if ((x | y) & 1)
      return;
    if (x / 2 < cv->out_w && y / 2 < cv->out_h)
      cv->set(cv->off_x + x / 2, cv->off_y + y / 2, white ? 0 : 3);
  };

  // Covers are shown even when book images are switched off in the reader.
  const bool images_were_enabled = images_enabled;
  images_enabled = true;
  DecodedImage unused;
  const ImageError err = decode_image_from_entry(file, entry, static_cast<uint16_t>(hi_w), static_cast<uint16_t>(hi_h),
                                                 unused, nullptr, 0, /*scale_to_fill=*/true, &sink, &pixel_sink);
  images_enabled = images_were_enabled;
  canvas.flush();
  file.close();
  if (err != ImageError::Ok) {
    MR_LOGI("cover", "decode failed (%d) for '%s'", static_cast<int>(err), entry_name.c_str());
    return err == ImageError::ReadError ? CoverResult::Failed : CoverResult::NoCover;
  }

  // 3. Write the MGR2 file (via a temporary name so a cut-off write is never used).
  const std::string tmp_path = std::string(out_path) + ".tmp";
  FILE* out = std::fopen(tmp_path.c_str(), "wb");
  if (!out)
    return CoverResult::Failed;
  const uint16_t w = kPhysW, h = kPhysH;
  bool ok = std::fwrite("MGR2", 1, 4, out) == 4 && std::fwrite(&w, 2, 1, out) == 1 && std::fwrite(&h, 2, 1, out) == 1 &&
            std::fwrite(plane0, 1, kPlaneBytes, out) == kPlaneBytes &&
            std::fwrite(plane1, 1, kPlaneBytes, out) == kPlaneBytes;
  ok = (std::fclose(out) == 0) && ok;
  if (ok) {
    std::remove(out_path);
    ok = std::rename(tmp_path.c_str(), out_path) == 0;
  }
  if (!ok) {
    std::remove(tmp_path.c_str());
    return CoverResult::Failed;
  }
  MR_LOGI("cover", "cover %ux%u -> %dx%d written to %s", src_w, src_h, canvas.out_w, canvas.out_h, out_path);
  return CoverResult::Ok;
}

}  // namespace microreader
