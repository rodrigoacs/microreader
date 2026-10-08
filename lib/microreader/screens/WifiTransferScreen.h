// Added by acs (fork of CidVonHighwind/microreader), 2026-10-08: Wi-Fi Transfer screen.
#pragma once

#include <cstdint>

#include "../FileServer.h"
#include "../Input.h"
#include "../display/DrawBuffer.h"
#include "IScreen.h"

namespace microreader {

// Shows how to connect (network, password, address) while the file server runs.
// Back stops the server; if books were added or removed, the book index is
// rebuilt before returning to the menu.
class WifiTransferScreen final : public IScreen {
 public:
  WifiTransferScreen() = default;

  const char* name() const override {
    return "Wi-Fi Transfer";
  }

  void start(DrawBuffer& buf, IRuntime& runtime) override;
  void stop() override;
  void update(const ButtonState& buttons, DrawBuffer& buf, IRuntime& runtime) override;

 private:
  static constexpr uint32_t kPollMs = 500;

  BitmapFont title_font_;
  BitmapFont big_font_;
  BitmapFont small_font_;

  FileServerStatus shown_{};  // what is currently on screen
  bool started_ok_ = false;
  uint32_t since_poll_ms_ = 0;

  void draw_(DrawBuffer& buf, const FileServerStatus& st);
  static bool differs_(const FileServerStatus& a, const FileServerStatus& b);
};

}  // namespace microreader
