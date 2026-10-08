// Added by acs (fork of CidVonHighwind/microreader), 2026-10-08: Wi-Fi access point + HTTP upload server.
#pragma once

#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "microreader/FileServer.h"
#include "nvs_flash.h"

// Wi-Fi Transfer: the device becomes an access point ("Microreader-XXXX", WPA2
// with a random password shown on screen) and serves a small upload page at
// http://192.168.4.1. Everything is created in start() and destroyed in stop(),
// so the radio and its buffers only use RAM while the transfer screen is open.
//
// SD card and e-paper share SPI2_HOST, and the display driver toggles CS by
// hand, so SD access from the HTTP task must not overlap a display transfer.
// Every SD access here takes an IoLock: it asks the main loop to pause at a
// safe point (top of the loop, no SPI in flight) and waits for the grant.
class WifiFileServer final : public microreader::IFileServer {
 public:
  static constexpr const char* kSdRoot = "/sdcard";
  static constexpr const char* kUploadDir = "/sdcard/books";
  static constexpr const char* kApIp = "192.168.4.1";
  static constexpr uint32_t kMaxUpload = 200u * 1024u * 1024u;  // 200 MB sanity limit

  // ---- Main-loop handshake (call at the very top of every loop iteration) ----
  bool io_requested() const {
    return io_request_.load();
  }
  void grant_io() {
    io_granted_.store(true);
  }

  // ---- microreader::IFileServer ----
  bool start() override {
    if (running_)
      return true;
    {
      std::lock_guard<std::mutex> lk(mtx_);
      st_ = microreader::FileServerStatus{};
    }
    log_heap_("wifi: before start");

    // Receive buffer only exists while the server runs.
    chunk_ = static_cast<uint8_t*>(heap_caps_malloc(kChunkSize, MALLOC_CAP_8BIT));
    if (!chunk_) {
      set_error_("Not enough memory");
      return false;
    }

    if (!init_once_()) {
      set_error_("System init failed");
      teardown_();
      return false;
    }

    netif_ = esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
      ESP_LOGE(kTag, "esp_wifi_init: %s", esp_err_to_name(err));
      set_error_("Not enough memory for Wi-Fi");
      teardown_();
      return false;
    }
    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    char ssid[33];
    char pass[17];
    make_credentials_(ssid, sizeof(ssid), pass, sizeof(pass));

    wifi_config_t ap = {};
    // Both strings are short (<= 16 chars) and the fields are zero-initialised.
    const size_t ssid_n = std::strlen(ssid) < sizeof(ap.ap.ssid) ? std::strlen(ssid) : sizeof(ap.ap.ssid) - 1;
    std::memcpy(ap.ap.ssid, ssid, ssid_n);
    ap.ap.ssid_len = static_cast<uint8_t>(ssid_n);
    const size_t pass_n = std::strlen(pass) < sizeof(ap.ap.password) ? std::strlen(pass) : sizeof(ap.ap.password) - 1;
    std::memcpy(ap.ap.password, pass, pass_n);
    ap.ap.channel = 6;
    ap.ap.max_connection = 2;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;

    if ((err = esp_wifi_set_mode(WIFI_MODE_AP)) != ESP_OK || (err = esp_wifi_set_config(WIFI_IF_AP, &ap)) != ESP_OK ||
        (err = esp_wifi_start()) != ESP_OK) {
      ESP_LOGE(kTag, "wifi start: %s", esp_err_to_name(err));
      set_error_("Could not start access point");
      teardown_();
      return false;
    }
    wifi_started_ = true;

    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.stack_size = 6144;
    hc.max_open_sockets = 4;
    hc.max_uri_handlers = 6;
    hc.lru_purge_enable = true;
    hc.recv_wait_timeout = 20;
    hc.send_wait_timeout = 20;
    if ((err = httpd_start(&httpd_, &hc)) != ESP_OK) {
      ESP_LOGE(kTag, "httpd_start: %s", esp_err_to_name(err));
      httpd_ = nullptr;
      set_error_("Could not start web server");
      teardown_();
      return false;
    }
    register_(HTTP_GET, "/", &WifiFileServer::handle_index_);
    register_(HTTP_GET, "/api/books", &WifiFileServer::handle_list_);
    register_(HTTP_POST, "/api/upload", &WifiFileServer::handle_upload_);
    register_(HTTP_POST, "/api/delete", &WifiFileServer::handle_delete_);

    {
      std::lock_guard<std::mutex> lk(mtx_);
      st_.running = true;
      std::snprintf(st_.ssid, sizeof(st_.ssid), "%s", ssid);
      std::snprintf(st_.password, sizeof(st_.password), "%s", pass);
      std::snprintf(st_.url, sizeof(st_.url), "http://%s", kApIp);
    }
    running_ = true;
    ESP_LOGI(kTag, "access point '%s' up, http://%s", ssid, kApIp);
    log_heap_("wifi: after start");
    return true;
  }

  void stop() override {
    if (!running_ && !netif_)
      return;
    teardown_();
    running_ = false;
    {
      std::lock_guard<std::mutex> lk(mtx_);
      st_.running = false;
      st_.receiving = false;
      st_.clients = 0;
    }
    log_heap_("wifi: after stop");
  }

  microreader::FileServerStatus status() const override {
    std::lock_guard<std::mutex> lk(mtx_);
    return st_;
  }

 private:
  static constexpr const char* kTag = "wifi";

  // ---- SD access lock (see class comment) ----
  class IoLock {
   public:
    explicit IoLock(WifiFileServer& s) : s_(s) {
      s_.io_granted_.store(false);
      s_.io_request_.store(true);
      const int64_t deadline = esp_timer_get_time() + 5000000;  // 5 s
      // Block for at least one tick (10 ms at CONFIG_FREERTOS_HZ=100): pdMS_TO_TICKS(2)
      // is 0 there, which never yields to the lower-priority main loop that grants access.
      while (!s_.io_granted_.load() && esp_timer_get_time() < deadline)
        vTaskDelay(1);
      ok_ = s_.io_granted_.load();
    }
    ~IoLock() {
      // Clear the grant first so a stale grant can never satisfy the next request.
      s_.io_granted_.store(false);
      s_.io_request_.store(false);
    }
    bool ok() const {
      return ok_;
    }

   private:
    WifiFileServer& s_;
    bool ok_ = false;
  };

  // ---- one-time system init (netif, event loop, NVS for Wi-Fi calibration data) ----
  bool init_once_() {
    static bool done = false;
    if (done)
      return true;
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      nvs_flash_erase();
      err = nvs_flash_init();
    }
    if (err != ESP_OK)
      ESP_LOGW(kTag, "nvs_flash_init: %s (continuing)", esp_err_to_name(err));
    if ((err = esp_netif_init()) != ESP_OK) {
      ESP_LOGE(kTag, "esp_netif_init: %s", esp_err_to_name(err));
      return false;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
      ESP_LOGE(kTag, "event loop: %s", esp_err_to_name(err));
      return false;
    }
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &WifiFileServer::on_wifi_event_, this);
    done = true;
    return true;
  }

  static void on_wifi_event_(void* arg, esp_event_base_t /*base*/, int32_t id, void* /*data*/) {
    auto* self = static_cast<WifiFileServer*>(arg);
    std::lock_guard<std::mutex> lk(self->mtx_);
    if (id == WIFI_EVENT_AP_STACONNECTED)
      self->st_.clients++;
    else if (id == WIFI_EVENT_AP_STADISCONNECTED && self->st_.clients > 0)
      self->st_.clients--;
  }

  void teardown_() {
    if (httpd_) {
      httpd_stop(httpd_);
      httpd_ = nullptr;
    }
    if (wifi_started_) {
      esp_wifi_stop();
      wifi_started_ = false;
    }
    esp_wifi_deinit();  // harmless if init failed
    if (netif_) {
      esp_netif_destroy_default_wifi(netif_);
      netif_ = nullptr;
    }
    io_granted_.store(false);
    io_request_.store(false);
    heap_caps_free(chunk_);
    chunk_ = nullptr;
  }

  void make_credentials_(char* ssid, size_t ssid_len, char* pass, size_t pass_len) {
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    std::snprintf(ssid, ssid_len, "Microreader-%02X%02X", mac[4], mac[5]);
    // No look-alike characters (0/O, 1/l/I) so the password is easy to type.
    static constexpr char kAlphabet[] = "abcdefghjkmnpqrstuvwxyz23456789";
    const size_t n = 8;
    for (size_t i = 0; i < n && i + 1 < pass_len; ++i)
      pass[i] = kAlphabet[esp_random() % (sizeof(kAlphabet) - 1)];
    pass[n < pass_len ? n : pass_len - 1] = '\0';
  }

  void set_error_(const char* msg) {
    std::lock_guard<std::mutex> lk(mtx_);
    std::snprintf(st_.error, sizeof(st_.error), "%s", msg);
  }

  static void log_heap_(const char* where) {
    ESP_LOGI("mem", "%s: free=%lu largest=%lu", where, (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  }

  using Handler = esp_err_t (WifiFileServer::*)(httpd_req_t*);
  struct Route {
    WifiFileServer* self;
    Handler fn;
  };
  Route routes_[4] = {};
  int n_routes_ = 0;

  void register_(httpd_method_t method, const char* uri, Handler fn) {
    Route* r = &routes_[n_routes_++];
    r->self = this;
    r->fn = fn;
    httpd_uri_t u = {};
    u.uri = uri;
    u.method = method;
    u.handler = [](httpd_req_t* req) -> esp_err_t {
      auto* route = static_cast<Route*>(req->user_ctx);
      return (route->self->*(route->fn))(req);
    };
    u.user_ctx = r;
    httpd_register_uri_handler(httpd_, &u);
  }

  // ---- helpers ----
  static void url_decode_(char* s) {
    char* out = s;
    for (const char* in = s; *in; ++in) {
      if (*in == '%' && std::isxdigit(static_cast<unsigned char>(in[1])) &&
          std::isxdigit(static_cast<unsigned char>(in[2]))) {
        const char hex[3] = {in[1], in[2], 0};
        *out++ = static_cast<char>(std::strtol(hex, nullptr, 16));
        in += 2;
      } else if (*in == '+') {
        *out++ = ' ';
      } else {
        *out++ = *in;
      }
    }
    *out = '\0';
  }

  // Accepts a plain "name.epub" (no directories, no control or reserved characters).
  static bool valid_book_name_(const char* name) {
    const size_t len = std::strlen(name);
    if (len < 6 || len > 120 || name[0] == '.' || name[0] == ' ')
      return false;
    for (size_t i = 0; i < len; ++i) {
      const unsigned char c = static_cast<unsigned char>(name[i]);
      if (c < 0x20 || c == 0x7F || std::strchr("/\\:*?\"<>|", c))
        return false;
    }
    if (std::strstr(name, ".."))
      return false;
    const char* ext = name + len - 5;
    return std::tolower(static_cast<unsigned char>(ext[0])) == '.' &&
           std::tolower(static_cast<unsigned char>(ext[1])) == 'e' &&
           std::tolower(static_cast<unsigned char>(ext[2])) == 'p' &&
           std::tolower(static_cast<unsigned char>(ext[3])) == 'u' &&
           std::tolower(static_cast<unsigned char>(ext[4])) == 'b';
  }

  static bool query_name_(httpd_req_t* req, char* out, size_t out_len) {
    char query[384];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK)
      return false;
    if (httpd_query_key_value(query, "name", out, out_len) != ESP_OK)
      return false;
    url_decode_(out);
    return valid_book_name_(out);
  }

  static void json_escape_(std::string& dst, const char* s) {
    for (; *s; ++s) {
      const unsigned char c = static_cast<unsigned char>(*s);
      if (c == '"' || c == '\\') {
        dst += '\\';
        dst += static_cast<char>(c);
      } else if (c < 0x20) {
        char tmp[8];
        std::snprintf(tmp, sizeof(tmp), "\\u%04x", c);
        dst += tmp;
      } else {
        dst += static_cast<char>(c);
      }
    }
  }

  static esp_err_t send_json_(httpd_req_t* req, const char* status, const char* body) {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
  }

  // ---- handlers ----
  esp_err_t handle_index_(httpd_req_t* req);  // defined after the page below

  // GET /api/books -> [{"name":..,"path":..,"size":..,"deletable":bool}, ...]
  esp_err_t handle_list_(httpd_req_t* req) {
    std::string json = "[";
    {
      IoLock lock(*this);
      if (!lock.ok())
        return send_json_(req, "503 Service Unavailable", "{\"error\":\"busy\"}");
      bool first = true;
      list_dir_(kSdRoot, 0, json, first);
    }
    json += "]";
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json.data(), static_cast<ssize_t>(json.size()));
  }

  void list_dir_(const char* dir, int depth, std::string& json, bool& first) {
    DIR* d = opendir(dir);
    if (!d)
      return;
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr && json.size() < 24 * 1024) {
      if (ent->d_name[0] == '.')
        continue;
      std::string path = std::string(dir) + "/" + ent->d_name;
      if (ent->d_type == DT_DIR) {
        if (depth < 3)
          list_dir_(path.c_str(), depth + 1, json, first);
        continue;
      }
      const size_t len = std::strlen(ent->d_name);
      if (len < 5 || strcasecmp(ent->d_name + len - 5, ".epub") != 0)
        continue;
      struct stat stt = {};
      stat(path.c_str(), &stt);
      if (!first)
        json += ",";
      first = false;
      json += "{\"name\":\"";
      json_escape_(json, ent->d_name);
      json += "\",\"path\":\"";
      json_escape_(json, path.c_str() + std::strlen(kSdRoot));
      char tail[64];
      std::snprintf(tail, sizeof(tail), "\",\"size\":%lu,\"deletable\":%s}", (unsigned long)stt.st_size,
                    std::strcmp(dir, kUploadDir) == 0 ? "true" : "false");
      json += tail;
    }
    closedir(d);
  }

  // POST /api/upload?name=<file.epub>   body = raw file bytes
  esp_err_t handle_upload_(httpd_req_t* req) {
    char name[128];
    if (!query_name_(req, name, sizeof(name)))
      return send_json_(req, "400 Bad Request", "{\"error\":\"Invalid file name (must be a .epub)\"}");
    const size_t total = req->content_len;
    if (total == 0 || total > kMaxUpload)
      return send_json_(req, "413 Payload Too Large", "{\"error\":\"Empty or too large\"}");

    const std::string final_path = std::string(kUploadDir) + "/" + name;
    const std::string tmp_path = final_path + ".part";

    FILE* f = nullptr;
    {
      IoLock lock(*this);
      if (!lock.ok())
        return send_json_(req, "503 Service Unavailable", "{\"error\":\"busy\"}");
      mkdir(kUploadDir, 0775);
      uint64_t total_bytes = 0, free_bytes = 0;
      if (esp_vfs_fat_info(kSdRoot, &total_bytes, &free_bytes) == ESP_OK && free_bytes < total + 64 * 1024)
        return send_json_(req, "507 Insufficient Storage", "{\"error\":\"SD card is full\"}");
      f = std::fopen(tmp_path.c_str(), "wb");
    }
    if (!f)
      return send_json_(req, "500 Internal Server Error", "{\"error\":\"Cannot create file\"}");

    {
      std::lock_guard<std::mutex> lk(mtx_);
      st_.receiving = true;
      std::snprintf(st_.current_file, sizeof(st_.current_file), "%s", name);
      st_.current_bytes = 0;
      st_.current_total = static_cast<uint32_t>(total);
      st_.error[0] = '\0';
    }

    // Receive in slices: hold the SD lock for up to ~1 s of writing, then let
    // the main loop run once (so the screen can show progress), then continue.
    size_t received = 0;
    bool ok = true;
    int timeouts = 0;  // consecutive receive timeouts; give up after a few
    while (received < total && ok) {
      IoLock lock(*this);
      if (!lock.ok()) {
        ok = false;
        break;
      }
      const int64_t slice_end = esp_timer_get_time() + 1000000;
      while (received < total && esp_timer_get_time() < slice_end) {
        const size_t want = (total - received) < kChunkSize ? (total - received) : kChunkSize;
        const int r = httpd_req_recv(req, reinterpret_cast<char*>(chunk_), want);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
          if (++timeouts >= 3) {
            ok = false;
            break;
          }
          continue;
        }
        timeouts = 0;
        if (r <= 0 || std::fwrite(chunk_, 1, static_cast<size_t>(r), f) != static_cast<size_t>(r)) {
          ok = false;
          break;
        }
        received += static_cast<size_t>(r);
      }
      std::lock_guard<std::mutex> lk(mtx_);
      st_.current_bytes = static_cast<uint32_t>(received);
    }

    {
      IoLock lock(*this);
      std::fclose(f);
      if (ok) {
        std::remove(final_path.c_str());  // replace an existing book with the same name
        ok = std::rename(tmp_path.c_str(), final_path.c_str()) == 0;
      }
      if (!ok)
        std::remove(tmp_path.c_str());
    }

    {
      std::lock_guard<std::mutex> lk(mtx_);
      st_.receiving = false;
      if (ok) {
        st_.files_received++;
      } else {
        std::snprintf(st_.error, sizeof(st_.error), "Upload failed: %.30s", name);
      }
    }
    ESP_LOGI(kTag, "upload '%s' %s (%u bytes)", name, ok ? "ok" : "FAILED", (unsigned)received);
    return ok ? send_json_(req, "200 OK", "{\"ok\":true}")
              : send_json_(req, "500 Internal Server Error", "{\"error\":\"Upload failed\"}");
  }

  // POST /api/delete?name=<file.epub>   (only books inside /books can be deleted)
  esp_err_t handle_delete_(httpd_req_t* req) {
    char name[128];
    if (!query_name_(req, name, sizeof(name)))
      return send_json_(req, "400 Bad Request", "{\"error\":\"Invalid file name\"}");
    const std::string path = std::string(kUploadDir) + "/" + name;
    bool ok;
    {
      IoLock lock(*this);
      if (!lock.ok())
        return send_json_(req, "503 Service Unavailable", "{\"error\":\"busy\"}");
      ok = std::remove(path.c_str()) == 0;
    }
    if (ok) {
      std::lock_guard<std::mutex> lk(mtx_);
      st_.files_deleted++;
    }
    return ok ? send_json_(req, "200 OK", "{\"ok\":true}") : send_json_(req, "404 Not Found", "{\"error\":\"Not found\"}");
  }

  mutable std::mutex mtx_;
  microreader::FileServerStatus st_;
  std::atomic<bool> io_request_{false};
  std::atomic<bool> io_granted_{false};
  bool running_ = false;
  bool wifi_started_ = false;
  esp_netif_t* netif_ = nullptr;
  httpd_handle_t httpd_ = nullptr;
  static constexpr size_t kChunkSize = 4096;
  uint8_t* chunk_ = nullptr;  // allocated in start(), freed in stop()
};

// ---- Upload page (served at "/"; kept small, stored in flash) ----
static const char kWifiIndexHtml[] = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Microreader</title>
<style>
:root{--bg:#f6f3ee;--fg:#1d1c1a;--mut:#6b675f;--line:#d9d3c7;--acc:#2f5d50}
@media(prefers-color-scheme:dark){:root{--bg:#1b1a18;--fg:#ece7de;--mut:#a29c90;--line:#3a3833;--acc:#8cc4b2}}
*{box-sizing:border-box}body{margin:0;font:16px/1.5 system-ui,sans-serif;background:var(--bg);color:var(--fg)}
main{max-width:640px;margin:0 auto;padding:24px 16px}h1{font-size:1.4rem;margin:0 0 4px}p.s{color:var(--mut);margin:0 0 20px}
#drop{display:block;border:2px dashed var(--line);border-radius:10px;padding:28px 16px;text-align:center;cursor:pointer}
#drop.on{border-color:var(--acc)}button{font:inherit;cursor:pointer}
.bar{height:6px;background:var(--line);border-radius:3px;overflow:hidden;margin-top:6px}.bar i{display:block;height:100%;width:0;background:var(--acc)}
ul{list-style:none;padding:0;margin:8px 0 0}li{display:flex;gap:8px;align-items:center;padding:8px 0;border-bottom:1px solid var(--line)}
li span{flex:1;overflow-wrap:anywhere}li small{color:var(--mut)}li button{border:1px solid var(--line);background:none;color:var(--fg);border-radius:6px;padding:2px 10px}
h2{font-size:1rem;margin:28px 0 0}#q>div{margin-top:12px}.err{color:#c0392b}
</style></head><body><main>
<h1>Microreader</h1><p class="s">Send EPUB books to your reader.</p>
<label id="drop">Tap to choose <b>.epub</b> files, or drop them here
<input id="f" type="file" accept=".epub,application/epub+zip" multiple hidden></label>
<div id="q"></div>
<h2>Books on the device</h2><ul id="l"><li><small>Loading…</small></li></ul>
</main><script>
const $=s=>document.querySelector(s),q=[];let busy=false;
const kb=n=>n>1048576?(n/1048576).toFixed(1)+' MB':Math.ceil(n/1024)+' KB';
function esc(s){return s.replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]))}
async function list(){try{const r=await fetch('/api/books');const b=await r.json();
$('#l').innerHTML=b.length?b.sort((x,y)=>x.name.localeCompare(y.name)).map(x=>`<li><span>${esc(x.name)}<br><small>${kb(x.size)} · ${esc(x.path)}</small></span>${x.deletable?`<button data-n="${esc(x.name)}">Delete</button>`:''}</li>`).join(''):'<li><small>No books yet.</small></li>'}
catch(e){$('#l').innerHTML='<li class="err">Could not load the list.</li>'}}
$('#l').onclick=async e=>{const n=e.target.dataset.n;if(!n||!confirm('Delete "'+n+'"?'))return;
await fetch('/api/delete?name='+encodeURIComponent(n),{method:'POST'});list()};
function add(files){for(const f of files){if(!/\.epub$/i.test(f.name))continue;const d=document.createElement('div');
d.innerHTML=`<div>${esc(f.name)} <small>(${kb(f.size)})</small> <small class="st">waiting</small></div><div class="bar"><i></i></div>`;$('#q').appendChild(d);q.push({f,d})}next()}
function next(){if(busy||!q.length)return;busy=true;const {f,d}=q.shift(),x=new XMLHttpRequest();
x.open('POST','/api/upload?name='+encodeURIComponent(f.name));
x.upload.onprogress=e=>{if(e.lengthComputable)d.querySelector('i').style.width=(100*e.loaded/e.total)+'%'};
x.onloadend=()=>{const ok=x.status==200;let m='done';if(!ok){try{m=JSON.parse(x.responseText).error}catch(_){m='failed'}}
d.querySelector('.st').textContent=m;d.querySelector('.st').className=ok?'st':'st err';if(ok)d.querySelector('i').style.width='100%';busy=false;list();next()};
x.send(f)}
$('#f').onchange=e=>{add(e.target.files);e.target.value=''};
const dr=$('#drop');['dragover','dragenter'].forEach(t=>dr.addEventListener(t,e=>{e.preventDefault();dr.classList.add('on')}));
['dragleave','drop'].forEach(t=>dr.addEventListener(t,e=>{e.preventDefault();dr.classList.remove('on')}));
dr.addEventListener('drop',e=>add(e.dataTransfer.files));list();
</script></body></html>)HTML";

inline esp_err_t WifiFileServer::handle_index_(httpd_req_t* req) {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, kWifiIndexHtml, sizeof(kWifiIndexHtml) - 1);
}