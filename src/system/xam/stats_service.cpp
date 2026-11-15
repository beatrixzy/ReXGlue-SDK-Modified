/**
 * @file        system/xam/stats_service.cpp
 * @brief       Online leaderboards over the PGR4-Leaderboard HTTP/JSON API.
 *
 * Routes (title = hex title id):
 *   POST /v1/:title/stats        {gamertag, key, views:[{id, props:[{id, t, v}]}]}
 *   GET  /v1/:title/leaderboard  ?view=&start=&count=   or   ?view=&pivot=<xuid>&count=
 *   POST /v1/:title/read         {xuids:[...], views:[...]}
 * Values travel as {t: XUSER_DATA type, v: string}; rows come back with
 * columns as {"<column id>": [type, "value"]}.
 */

#include <rex/system/xam/stats_service.h>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_STRING(stats_server, "", "Network",
                      "Leaderboard server base URL (empty or off = leaderboards off), e.g. "
                      "https://pgr4-leaderboard.example.workers.dev");

#if defined(_WIN32)

#include <windows.h>
#include <winhttp.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <thread>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

namespace rex::system::xam::stats {
namespace {

using json = nlohmann::json;

constexpr DWORD kTimeoutMs = 5000;

std::filesystem::path ExeDir() {
  wchar_t buf[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, buf, MAX_PATH);
  return std::filesystem::path(buf).parent_path();
}

std::wstring Widen(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
  return w;
}

std::string BaseUrl() {
  std::string url = REXCVAR_GET(stats_server);
  while (!url.empty() && (url.back() == '/' || url.back() == ' ')) url.pop_back();
  return url == "off" ? std::string() : url;
}

// Random key that claims our gamertags on the server, created on first use.
const std::string& ClaimKey() {
  static std::string key = [] {
    const auto path = ExeDir() / "stats_key.txt";
    std::string k;
    if (std::ifstream in(path); in) {
      std::getline(in, k);
    }
    k.erase(k.find_last_not_of(" \r\n\t") + 1);
    if (k.size() < 32) {
      std::random_device rd;  // rand_s on MSVC
      k.clear();
      for (int i = 0; i < 8; ++i) k += fmt::format("{:08x}", uint32_t(rd()));
      std::ofstream(path) << k << "\n";
      REXSYS_INFO("stats: created {}", path.string());
    }
    return k;
  }();
  return key;
}

// One blocking request. Returns the HTTP status (0 = no connection).
int Http(const char* method, const std::string& path, const std::string& body, std::string& out) {
  out.clear();
  const std::wstring url = Widen(BaseUrl() + path);
  URL_COMPONENTS parts = {};
  parts.dwStructSize = sizeof(parts);
  wchar_t host[256] = {}, url_path[2048] = {};
  parts.lpszHostName = host;
  parts.dwHostNameLength = DWORD(std::size(host));
  parts.lpszUrlPath = url_path;
  parts.dwUrlPathLength = DWORD(std::size(url_path));
  wchar_t extra[2048] = {};
  parts.lpszExtraInfo = extra;
  parts.dwExtraInfoLength = DWORD(std::size(extra));
  if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) {
    return 0;
  }
  static HINTERNET session = WinHttpOpen(L"rexglue-stats/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                         WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) return 0;
  WinHttpSetTimeouts(session, kTimeoutMs, kTimeoutMs, kTimeoutMs, kTimeoutMs);
  int status = 0;
  HINTERNET connect = WinHttpConnect(session, host, parts.nPort, 0);
  HINTERNET request = nullptr;
  if (connect) {
    std::wstring object = std::wstring(url_path) + extra;
    request = WinHttpOpenRequest(connect, Widen(method).c_str(), object.c_str(), nullptr,
                                 WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
  }
  if (request) {
    const wchar_t* headers = L"Content-Type: application/json\r\n";
    if (WinHttpSendRequest(request, headers, DWORD(-1),
                           body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(),
                           DWORD(body.size()), DWORD(body.size()), 0) &&
        WinHttpReceiveResponse(request, nullptr)) {
      DWORD code = 0, size = sizeof(code);
      WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                          WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX);
      status = int(code);
      DWORD avail = 0;
      while (WinHttpQueryDataAvailable(request, &avail) && avail) {
        std::string chunk(avail, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(request, chunk.data(), avail, &read) || !read) break;
        out.append(chunk.data(), read);
      }
    }
    WinHttpCloseHandle(request);
  }
  if (connect) WinHttpCloseHandle(connect);
  return status;
}

std::string TitlePath(uint32_t title_id, const char* route) {
  return fmt::format("/v1/{:08X}/{}", title_id, route);
}

json ValueToJson(const Value& v) {
  std::string s;
  switch (v.type) {
    case kTypeContext:
    case kTypeInt32:
    case kTypeInt64:
    case kTypeDateTime:
      s = std::to_string(v.number);
      break;
    case kTypeDouble:
    case kTypeFloat:
      s = fmt::format("{}", v.real);
      break;
    default:
      s = v.text;
      break;
  }
  return json{{"t", v.type}, {"v", s}};
}

Value ValueFromJson(const json& j) {
  Value v;
  if (!j.is_array() || j.size() != 2 || !j[1].is_string()) return v;
  v.type = uint8_t(j[0].get<int>());
  const std::string s = j[1].get<std::string>();
  try {
    switch (v.type) {
      case kTypeContext:
      case kTypeInt32:
      case kTypeInt64:
      case kTypeDateTime:
        v.number = std::stoll(s);
        break;
      case kTypeDouble:
      case kTypeFloat:
        v.real = std::stod(s);
        break;
      case kTypeUnicode:
      case kTypeBinary:
        v.text = s;
        break;
      default:
        v.type = kTypeNull;
        break;
    }
  } catch (...) {
    v.type = kTypeNull;
  }
  return v;
}

uint64_t ParseXuid(const std::string& hex) {
  try {
    return std::stoull(hex, nullptr, 16);
  } catch (...) {
    return 0;
  }
}

bool ParseView(const json& j, ViewRows& out) {
  if (!j.is_object()) return false;
  out.view_id = j.value("view", 0u);
  out.total = j.value("total", 0u);
  out.rows.clear();
  for (const auto& r : j.value("rows", json::array())) {
    Row row;
    row.xuid = ParseXuid(r.value("xuid", ""));
    row.rank = r.value("rank", 0u);
    try {
      row.rating = std::stoll(r.value("rating", "0"));
    } catch (...) {
    }
    row.gamertag = r.value("gamertag", "");
    for (const auto& [id, value] : r.value("columns", json::object()).items()) {
      Column c;
      c.id = uint16_t(std::stoul(id));
      c.value = ValueFromJson(value);
      row.columns.push_back(std::move(c));
    }
    out.rows.push_back(std::move(row));
  }
  return true;
}

bool GetJson(const std::string& path, json& out) {
  std::string body;
  int status = Http("GET", path, {}, body);
  if (status != 200) {
    REXSYS_WARN("stats: GET {} -> {}", path, status);
    return false;
  }
  out = json::parse(body, nullptr, false);
  return !out.is_discarded();
}

// Background uploads.
struct PendingWrite {
  uint32_t title_id;
  std::string body;
  std::string gamertag;
};
std::mutex g_mutex;
std::condition_variable g_cv;
std::deque<PendingWrite> g_writes;
std::once_flag g_thread_once;

void WriterLoop() {
  for (;;) {
    PendingWrite w;
    {
      std::unique_lock<std::mutex> lock(g_mutex);
      g_cv.wait(lock, [] { return !g_writes.empty(); });
      w = std::move(g_writes.front());
      g_writes.pop_front();
    }
    std::string reply;
    int status = 0;
    for (int attempt = 0; attempt < 4; ++attempt) {
      status = Http("POST", TitlePath(w.title_id, "stats"), w.body, reply);
      if (status != 0 && status < 500) break;
      std::this_thread::sleep_for(std::chrono::seconds(2 << attempt));
    }
    if (status == 200) {
      REXSYS_INFO("stats: uploaded for \"{}\": {}", w.gamertag, reply);
    } else if (status == 403) {
      REXSYS_WARN("stats: gamertag \"{}\" belongs to another player on the leaderboard server",
                  w.gamertag);
    } else {
      REXSYS_WARN("stats: upload for \"{}\" failed ({}) {}", w.gamertag, status, reply);
    }
  }
}

}  // namespace

bool Enabled() {
  return !BaseUrl().empty();
}

uint64_t XuidForGamertag(const std::string& gamertag) {
  uint64_t h = 0xCBF29CE484222325ull;
  for (char c : gamertag) {
    h = (h ^ uint8_t(std::tolower(uint8_t(c)))) * 0x100000001B3ull;
  }
  return 0x0009000000000000ull | (h & 0x0000FFFFFFFFFFFFull);
}

void SubmitWrite(uint32_t title_id, const std::string& gamertag, std::vector<ViewWrite> views) {
  if (!Enabled() || gamertag.empty() || views.empty()) return;
  json j_views = json::array();
  for (const auto& v : views) {
    json props = json::array();
    for (const auto& p : v.properties) {
      json jp = ValueToJson(p.value);
      jp["id"] = p.id;
      props.push_back(std::move(jp));
    }
    j_views.push_back({{"id", v.view_id}, {"props", std::move(props)}});
  }
  json body = {{"gamertag", gamertag}, {"key", ClaimKey()}, {"views", std::move(j_views)}};
  std::call_once(g_thread_once, [] { std::thread(WriterLoop).detach(); });
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_writes.push_back({title_id, body.dump(), gamertag});
  }
  g_cv.notify_one();
}

bool ReadByRank(uint32_t title_id, uint32_t view_id, uint32_t start_rank, uint32_t count,
                ViewRows& out) {
  json j;
  return Enabled() &&
         GetJson(TitlePath(title_id, "leaderboard") +
                     fmt::format("?view={}&start={}&count={}", view_id, start_rank, count),
                 j) &&
         ParseView(j, out);
}

bool ReadAroundXuid(uint32_t title_id, uint32_t view_id, uint64_t xuid, uint32_t count,
                    ViewRows& out) {
  json j;
  return Enabled() &&
         GetJson(TitlePath(title_id, "leaderboard") +
                     fmt::format("?view={}&pivot={:016X}&count={}", view_id, xuid, count),
                 j) &&
         ParseView(j, out);
}

bool ReadByXuids(uint32_t title_id, const std::vector<uint64_t>& xuids,
                 const std::vector<uint32_t>& view_ids, std::vector<ViewRows>& out) {
  out.clear();
  if (!Enabled()) return false;
  json body = {{"xuids", json::array()}, {"views", view_ids}};
  for (uint64_t x : xuids) body["xuids"].push_back(fmt::format("{:016X}", x));
  std::string reply;
  int status = Http("POST", TitlePath(title_id, "read"), body.dump(), reply);
  if (status != 200) {
    REXSYS_WARN("stats: read -> {}", status);
    return false;
  }
  json j = json::parse(reply, nullptr, false);
  if (j.is_discarded()) return false;
  for (const auto& v : j.value("views", json::array())) {
    ViewRows rows;
    if (ParseView(v, rows)) out.push_back(std::move(rows));
  }
  return true;
}

bool GetAnnouncements(uint32_t title_id, int64_t after_id, std::vector<Announcement>& out) {
  out.clear();
  if (!Enabled()) return false;
  // Polled every minute: quiet on failure (an older server has no route).
  std::string body;
  if (Http("GET", TitlePath(title_id, "announcements") + "?after=" + std::to_string(after_id), {}, body) != 200) {
    return false;
  }
  const json j = json::parse(body, nullptr, false);
  if (!j.is_object()) return false;
  const auto& list = j.value("announcements", json::array());
  if (!list.is_array()) return false;
  for (const auto& a : list) {
    if (!a.is_object() || !a.contains("id")) continue;
    Announcement item;
    item.id = a.value("id", int64_t(0));
    item.title = a.value("title", std::string());
    item.text = a.value("text", std::string());
    item.created_ms = a.value("created", int64_t(0));
    if (item.id > 0 && !item.text.empty()) out.push_back(std::move(item));
  }
  return true;
}

bool SendPlayerMessage(uint32_t title_id, const std::string& from, const std::string& to,
                       const std::string& text, std::string* error) {
  if (!Enabled()) {
    if (error) *error = "Messages need the online server (stats_server).";
    return false;
  }
  const json body = {{"from", from}, {"key", ClaimKey()}, {"to", to}, {"text", text}};
  std::string reply;
  const int status = Http("POST", TitlePath(title_id, "messages"), body.dump(), reply);
  if (status == 200) return true;
  if (error) {
    const json j = json::parse(reply, nullptr, false);
    *error = status == 0 ? "Couldn't reach the server."
             : j.is_object() && j.contains("error") ? j.value("error", std::string("failed"))
                                                     : "Sending failed (" + std::to_string(status) + ").";
  }
  return false;
}

bool GetInbox(uint32_t title_id, const std::string& player, int64_t after_id,
              std::vector<PlayerMessage>& out) {
  out.clear();
  if (!Enabled() || player.empty()) return false;
  const json body = {{"player", player}, {"key", ClaimKey()}, {"after", after_id}};
  std::string reply;
  if (Http("POST", TitlePath(title_id, "inbox"), body.dump(), reply) != 200) return false;
  const json j = json::parse(reply, nullptr, false);
  if (!j.is_object()) return false;
  const auto& list = j.value("messages", json::array());
  if (!list.is_array()) return false;
  for (const auto& m : list) {
    if (!m.is_object()) continue;
    PlayerMessage item;
    item.id = m.value("id", int64_t(0));
    item.from = m.value("from", std::string());
    item.text = m.value("text", std::string());
    item.created_ms = m.value("created", int64_t(0));
    if (item.id > 0 && !item.text.empty()) out.push_back(std::move(item));
  }
  return true;
}

}  // namespace rex::system::xam::stats

#else  // !_WIN32

namespace rex::system::xam::stats {
bool Enabled() {
  return false;
}
uint64_t XuidForGamertag(const std::string&) {
  return 0;
}
void SubmitWrite(uint32_t, const std::string&, std::vector<ViewWrite>) {}
bool ReadByRank(uint32_t, uint32_t, uint32_t, uint32_t, ViewRows&) {
  return false;
}
bool ReadAroundXuid(uint32_t, uint32_t, uint64_t, uint32_t, ViewRows&) {
  return false;
}
bool ReadByXuids(uint32_t, const std::vector<uint64_t>&, const std::vector<uint32_t>&,
                 std::vector<ViewRows>&) {
  return false;
}
bool GetAnnouncements(uint32_t, int64_t, std::vector<Announcement>& out) {
  out.clear();
  return false;
}
bool SendPlayerMessage(uint32_t, const std::string&, const std::string&, const std::string&,
                       std::string* error) {
  if (error) *error = "Messages are not available on this platform.";
  return false;
}
bool GetInbox(uint32_t, const std::string&, int64_t, std::vector<PlayerMessage>& out) {
  out.clear();
  return false;
}
}  // namespace rex::system::xam::stats

#endif
