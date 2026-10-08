// Added by acs (fork of CidVonHighwind/microreader), 2026-10-08: Wi-Fi Transfer screen.
#include "WifiTransferScreen.h"

#include <cstdio>
#include <cstring>

#include "../Application.h"
#include "../display/ui_font_header.h"
#include "../display/ui_font_large.h"
#include "../display/ui_font_small.h"

namespace microreader {

void WifiTransferScreen::start(DrawBuffer& buf, IRuntime& /*runtime*/) {
  if (!title_font_.valid())
    title_font_.init(kFontData_ui_header_mbf, kFontData_ui_header_mbf_size);
  if (!big_font_.valid())
    big_font_.init(kFontData_ui_large_mbf, kFontData_ui_large_mbf_size);
  if (!small_font_.valid())
    small_font_.init(kFontData_ui_small_mbf, kFontData_ui_small_mbf_size);

  IFileServer* server = app_ ? app_->file_server() : nullptr;
  started_ok_ = server && server->start();
  shown_ = server ? server->status() : FileServerStatus{};
  if (!server)
    std::snprintf(shown_.error, sizeof(shown_.error), "Not available on this device");
  since_poll_ms_ = 0;
  draw_(buf, shown_);
}

void WifiTransferScreen::stop() {
  IFileServer* server = app_ ? app_->file_server() : nullptr;
  if (server)
    server->stop();
  started_ok_ = false;
}

void WifiTransferScreen::update(const ButtonState& buttons, DrawBuffer& buf, IRuntime& runtime) {
  if (!app_)
    return;

  // Keep the device awake while the server is up (uploads can take a while).
  app_->keep_awake();

  Button btn;
  while (buttons.next_press(btn)) {
    if (btn == Button::Button0) {
      IFileServer* server = app_->file_server();
      const FileServerStatus final_st = server ? server->status() : FileServerStatus{};
      if (server)
        server->stop();
      started_ok_ = false;

      // New or deleted books: refresh the index so the menu shows them right away.
      if (final_st.files_received > 0 || final_st.files_deleted > 0)
        app_->rebuild_book_index(buf);

      app_->pop_screen();
      return;
    }
  }

  since_poll_ms_ += runtime.frame_time_ms();
  if (since_poll_ms_ < kPollMs)
    return;
  since_poll_ms_ = 0;

  IFileServer* server = app_->file_server();
  if (!server)
    return;
  const FileServerStatus st = server->status();
  if (differs_(st, shown_)) {
    shown_ = st;
    draw_(buf, shown_);
    buf.refresh();
  }
}

bool WifiTransferScreen::differs_(const FileServerStatus& a, const FileServerStatus& b) {
  // Redraw on any visible change; progress only in 10% steps to limit e-ink refreshes.
  auto pct = [](const FileServerStatus& s) -> uint32_t {
    return s.current_total ? (s.current_bytes / 1024) * 10 / (s.current_total / 1024 + 1) : 0;
  };
  return a.running != b.running || a.files_received != b.files_received || a.files_deleted != b.files_deleted ||
         a.clients != b.clients || a.receiving != b.receiving || std::strcmp(a.error, b.error) != 0 ||
         std::strcmp(a.current_file, b.current_file) != 0 || pct(a) != pct(b);
}

void WifiTransferScreen::draw_(DrawBuffer& buf, const FileServerStatus& st) {
  buf.fill(true);
  const int W = buf.width();
  const int margin = 24;
  int y = 30;

  auto line = [&](const BitmapFont& f, const char* text, int extra_gap = 0) {
    if (text && *text)
      buf.draw_text_proportional(margin, y + f.baseline(), text, std::strlen(text), f, false);
    y += f.y_advance() + extra_gap;
  };
  auto centered = [&](const BitmapFont& f, const char* text, int extra_gap = 0) {
    const int w = f.word_width(text, std::strlen(text), FontStyle::Regular);
    buf.draw_text_proportional((W - w) / 2, y + f.baseline(), text, std::strlen(text), f, false);
    y += f.y_advance() + extra_gap;
  };

  centered(title_font_, "Wi-Fi Transfer", 6);
  buf.fill_rect(margin, y, W - 2 * margin, 2, false);
  y += 18;

  if (!st.running) {
    line(big_font_, "Wi-Fi could not start.", 8);
    line(small_font_, st.error[0] ? st.error : "Unknown error.", 20);
  } else {
    line(small_font_, "1. On your phone or computer, join:", 6);
    line(big_font_, st.ssid, 14);
    if (st.password[0]) {
      line(small_font_, "Password:", 6);
      line(big_font_, st.password, 20);
    }
    line(small_font_, "2. Open this address in the browser:", 6);
    line(big_font_, st.url, 24);

    buf.fill_rect(margin, y, W - 2 * margin, 1, false);
    y += 16;

    char text[96];
    if (st.receiving) {
      if (st.current_total > 0) {
        const unsigned pct = static_cast<unsigned>((static_cast<uint64_t>(st.current_bytes) * 100) / st.current_total);
        std::snprintf(text, sizeof(text), "Receiving... %u%%", pct);
      } else {
        std::snprintf(text, sizeof(text), "Receiving...");
      }
      line(big_font_, text, 4);
      line(small_font_, st.current_file, 12);
    } else {
      line(big_font_, st.clients ? "Connected. Waiting for books..." : "Waiting for a connection...", 12);
    }

    std::snprintf(text, sizeof(text), "Books received: %lu", static_cast<unsigned long>(st.files_received));
    line(small_font_, text, 4);
    if (st.files_deleted) {
      std::snprintf(text, sizeof(text), "Books deleted: %lu", static_cast<unsigned long>(st.files_deleted));
      line(small_font_, text, 4);
    }
    if (st.error[0])
      line(small_font_, st.error, 4);
  }

  // Footer hint.
  const char* kHint = "Back: stop Wi-Fi and exit";
  const int hw = small_font_.word_width(kHint, std::strlen(kHint), FontStyle::Regular);
  buf.draw_text_proportional((W - hw) / 2, buf.height() - 20 + small_font_.baseline() - small_font_.y_advance() / 2,
                             kHint, std::strlen(kHint), small_font_, false);
}

}  // namespace microreader
