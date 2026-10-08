// Modified by acs (fork of CidVonHighwind/microreader), 2026-10-08: Wi-Fi Transfer UI preview (MICROREADER_FAKE_WIFI=1).
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <vector>

#include "desktop_config.h"
#include "display.h"
#include "input.h"
#include "microreader/Application.h"
#include "microreader/FileServer.h"
#include "microreader/FontManager.h"
#include "microreader/Loop.h"
#include "microreader/content/BitmapFont.h"
#include "microreader/display/DrawBuffer.h"
#include "runtime.h"

// Load a file into a byte vector. Returns empty on failure.
static std::vector<uint8_t> load_file(const char* path) {
  std::vector<uint8_t> data;
  FILE* f = fopen(path, "rb");
  if (!f)
    return data;
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  if (sz <= 0) {
    fclose(f);
    return data;
  }
  fseek(f, 0, SEEK_SET);
  data.resize(static_cast<size_t>(sz));
  fread(data.data(), 1, data.size(), f);
  fclose(f);
  return data;
}

class DesktopFontManager : public microreader::FontManager {
 public:
  explicit DesktopFontManager(microreader::Application& app) : app_(app) {}

  void ensure_ready(microreader::DrawBuffer&) override {
    const std::string& custom_font = app_.custom_font_path();

    if (custom_font == currently_loaded_path_ && font_set_.valid())
      return;  // already loaded

    static const std::string fonts_dir = (std::filesystem::path(MICROREADER_SD_DIR) / "fonts").string();

    std::string path = custom_font;
    if (path.empty()) {
      path = fonts_dir + "/bookerly.mfb";
    } else {
      // Map built-in names from ESP32 to local .mfb files
      if (path == "Bookerly")
        path = "bookerly.mfb";
      else if (path == "Alegreya")
        path = "alegreya.mfb";

      // Allow relative paths inside sd/fonts/ for testing,
      // or absolute paths if provided.
      if (!std::filesystem::exists(path) && path.find('/') == std::string::npos) {
        path = fonts_dir + "/" + path;
      }
    }

    std::vector<uint8_t> new_bundle = load_file(path.c_str());
    if (!new_bundle.empty()) {
      bundle_data_ = std::move(new_bundle);

      // Clear fonts first
      for (int i = 0; i < microreader::kMaxFontSizes; i++) {
        prop_fonts_[i] = microreader::BitmapFont();
      }
      font_set_ = microreader::BitmapFontSet();
      num_fonts_ = 0;

      if (load_bundle(bundle_data_.data(), bundle_data_.size())) {
        printf("[font] Loaded bundle: %s (%zu bytes)\n", path.c_str(), bundle_data_.size());
        app_.set_installed_font_path(custom_font);
        currently_loaded_path_ = custom_font;
        app_.set_reader_font(font_set());
      } else {
        printf("[font] Invalid bundle file: %s\n", path.c_str());
      }
    } else {
      printf("[font] Could not load bundle file: %s\n", path.c_str());
    }
  }

 private:
  microreader::Application& app_;
  std::vector<uint8_t> bundle_data_;
  std::string currently_loaded_path_;
};

// Fake file server for previewing the Wi-Fi Transfer screen on desktop.
class PreviewFileServer final : public microreader::IFileServer {
 public:
  bool start() override {
    t0_ = std::chrono::steady_clock::now();
    running_ = true;
    return true;
  }
  void stop() override {
    running_ = false;
  }
  microreader::FileServerStatus status() const override {
    microreader::FileServerStatus st;
    st.running = running_;
    std::snprintf(st.ssid, sizeof(st.ssid), "Microreader-1A2B");
    std::snprintf(st.password, sizeof(st.password), "k7m3pqr2");
    std::snprintf(st.url, sizeof(st.url), "http://192.168.4.1");
    // Timeline: 0-3 s waiting, 3-9 s receiving one book, then 1 book received.
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0_).count();
    st.clients = s > 2 ? 1 : 0;
    if (s > 3 && s < 9) {
      st.receiving = true;
      std::snprintf(st.current_file, sizeof(st.current_file), "Dom Casmurro.epub");
      st.current_total = 2'000'000;
      st.current_bytes = static_cast<uint32_t>((s - 3) / 6 * st.current_total);
    }
    st.files_received = s >= 9 ? 1 : 0;
    return st;
  }

 private:
  bool running_ = false;
  std::chrono::steady_clock::time_point t0_{};
};

int main() {
  try {
    DesktopRuntime runtime(16);
    DesktopInputSource input(runtime);
    DesktopEmulatorDisplay display(runtime);
    microreader::Application app;
    microreader::DrawBuffer buf(display);

    // Mount the repo-root sd/ folder as the virtual SD card.
    // MICROREADER_SD_DIR is set by CMake via desktop_config.h.
    static std::string books_path = std::filesystem::absolute(MICROREADER_SD_DIR).string();
    std::filesystem::create_directories(books_path);
    std::filesystem::create_directories(books_path + "/fonts");
    app.set_books_dir(books_path.c_str());

    // Data directory for converted books, settings, reading state.
    static std::string data_path = (std::filesystem::absolute(MICROREADER_SD_DIR) / ".microreader").string();
    std::filesystem::create_directories(data_path + "/cache");
    std::filesystem::create_directories(data_path + "/data");
    app.set_data_dir(data_path.c_str());

    DesktopFontManager font_mgr(app);
    app.set_font_manager(&font_mgr);

    // UI preview of Settings > Wi-Fi Transfer. There is no real network server on
    // desktop: this fake reports a simulated upload so the screen layout can be checked.
    static PreviewFileServer preview_fs;
    if (std::getenv("MICROREADER_FAKE_WIFI"))
      app.set_file_server(&preview_fs);

    // Provide an initial font so that Application::start() passes the auto-open check.
    // The correct custom font will be loaded when ReaderScreen::start() is entered.
    font_mgr.ensure_ready(buf);

    app.start(buf, runtime);
    microreader::run_loop(app, buf, input, runtime);

    // sleep for 3 second so we see the sleep screen
    SDL_Delay(3000);
  } catch (const std::exception& e) {
    std::cerr << "Fatal: " << e.what() << std::endl;
    return 1;
  }
  return 0;
}
