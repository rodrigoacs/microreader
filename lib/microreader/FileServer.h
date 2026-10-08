// Added by acs (fork of CidVonHighwind/microreader), 2026-10-08: Wi-Fi file transfer interface.
#pragma once

#include <cstdint>

namespace microreader {

// Snapshot of the file server state, polled by the UI. Plain data so it can be
// copied across tasks (the server runs in its own task on ESP32).
struct FileServerStatus {
  bool running = false;
  char ssid[33] = {};      // network name to join
  char password[17] = {};  // WPA2 password (empty = open network)
  char url[40] = {};       // address to open in the browser
  char error[48] = {};     // last error, empty if none

  uint32_t files_received = 0;  // completed uploads since start()
  uint32_t files_deleted = 0;   // deletions since start()
  uint32_t clients = 0;         // stations currently connected

  bool receiving = false;         // an upload is in progress
  char current_file[64] = {};     // name of the file being received
  uint32_t current_bytes = 0;     // bytes received so far
  uint32_t current_total = 0;     // expected size (0 = unknown)
};

// Platform-specific file server (Wi-Fi access point + HTTP upload page).
// Only alive while the Wi-Fi Transfer screen is open: start() brings the radio
// up, stop() tears everything down and frees the memory again.
class IFileServer {
 public:
  virtual ~IFileServer() = default;

  // Returns false (and fills status().error) if the server could not start.
  virtual bool start() = 0;
  virtual void stop() = 0;
  virtual FileServerStatus status() const = 0;
};

}  // namespace microreader
