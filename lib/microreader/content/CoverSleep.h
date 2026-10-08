// Added by acs (fork of CidVonHighwind/microreader), 2026-10-08: book cover as sleep image.
#pragma once

#include <cstddef>
#include <cstdint>

namespace microreader {

// Result of make_cover_sleep_image().
enum class CoverResult {
  Ok,       // out_path written
  NoCover,  // the book has no usable cover image (worth remembering)
  Failed,   // I/O or memory error (worth retrying next time)
};

// Extract the cover image of an EPUB and write it as an MGR2 sleep image
// (800x480 physical frame, 2 bpp, 4 grey levels, portrait cover rotated like
// convert_bmp_to_mgr2). The cover is fitted inside the screen, centred, on white.
//
// Scratch memory comes from the caller so no large heap block is needed:
//   plane0 / plane1: two buffers of at least 48000 bytes each (the two display
//   buffers). Both are overwritten.
//
// Grey levels: the existing 1-bit decoders run at twice the screen resolution
// and every 2x2 block of dithered pixels becomes one 4-level output pixel.
CoverResult make_cover_sleep_image(const char* epub_path, const char* out_path, uint8_t* plane0, uint8_t* plane1,
                                   size_t plane_size);

}  // namespace microreader
