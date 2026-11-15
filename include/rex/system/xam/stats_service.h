/**
 * @file        rex/system/xam/stats_service.h
 * @brief       Online leaderboards: Xbox Live user stats over an HTTP/JSON
 *              backend (PGR4-Leaderboard: Cloudflare Worker + D1, or its
 *              local dev server).
 *
 * XSessionWriteStats uploads the properties the title would have sent to
 * Live; the backend maps them onto the stats views of the title's SPA with
 * the SPA's aggregation and ranks rows by the rating column, highest first.
 * XUserReadStats and the stats enumerators read rows back.
 *
 * Players are gamertags, nothing else: the backend gives each one a stable
 * Live-style XUID (XuidForGamertag) and the first upload under a gamertag
 * claims it with a random key kept in stats_key.txt beside the executable.
 *
 * Off when stats_server is empty. Reads block the calling thread (callers run
 * them deferred, off the title's threads); writes go out on a background
 * thread. Windows only (WinHTTP); elsewhere Enabled() stays false.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rex::system::xam::stats {

// XUSER_DATA_TYPE values.
constexpr uint8_t kTypeContext = 0;
constexpr uint8_t kTypeInt32 = 1;
constexpr uint8_t kTypeInt64 = 2;
constexpr uint8_t kTypeDouble = 3;
constexpr uint8_t kTypeUnicode = 4;
constexpr uint8_t kTypeFloat = 5;
constexpr uint8_t kTypeBinary = 6;
constexpr uint8_t kTypeDateTime = 7;
constexpr uint8_t kTypeNull = 0xFF;

// Special stats column ids (SPA attribute ids).
constexpr uint16_t kColumnRank = 0xFFFF;
constexpr uint16_t kColumnRating = 0xFFFE;
constexpr uint16_t kColumnGamertag = 0xFFFD;

// One XUSER_DATA value: integers, floats (as double) and FILETIMEs in `number`
// / `real`, strings as UTF-8, binary as base64.
struct Value {
  uint8_t type = kTypeNull;
  int64_t number = 0;
  double real = 0;
  std::string text;
};

struct Property {
  uint32_t id = 0;
  Value value;
};

struct ViewWrite {
  uint32_t view_id = 0;
  std::vector<Property> properties;
};

struct Column {
  uint16_t id = 0;
  Value value;
};

struct Row {
  uint64_t xuid = 0;
  uint32_t rank = 0;
  int64_t rating = 0;
  std::string gamertag;
  std::vector<Column> columns;
};

// A message from the maintainer (GET /v1/:title/announcements).
struct Announcement {
  int64_t id = 0;
  std::string title;
  std::string text;
  int64_t created_ms = 0;
};

// A message from another player (POST /v1/:title/inbox).
struct PlayerMessage {
  int64_t id = 0;
  std::string from;
  std::string text;
  int64_t created_ms = 0;
};

struct ViewRows {
  uint32_t view_id = 0;
  uint32_t total = 0;
  std::vector<Row> rows;
};

// stats_server is set (and this is a Windows build).
bool Enabled();

// The XUID the backend uses for a gamertag (case-insensitive FNV-1a, 0x0009...).
uint64_t XuidForGamertag(const std::string& gamertag);

// Queues an upload for this gamertag; returns at once.
void SubmitWrite(uint32_t title_id, const std::string& gamertag, std::vector<ViewWrite> views);

// Blocking reads (a few seconds at most). False when the server can't be reached.
bool ReadByRank(uint32_t title_id, uint32_t view_id, uint32_t start_rank, uint32_t count,
                ViewRows& out);
bool ReadAroundXuid(uint32_t title_id, uint32_t view_id, uint64_t xuid, uint32_t count,
                    ViewRows& out);
bool ReadByXuids(uint32_t title_id, const std::vector<uint64_t>& xuids,
                 const std::vector<uint32_t>& view_ids, std::vector<ViewRows>& out);

// Announcements newer than `after_id`, newest first. False when the server is
// off or unreachable. Blocking: call it off the game's threads.
bool GetAnnouncements(uint32_t title_id, int64_t after_id, std::vector<Announcement>& out);

// Player messages, signed with this PC's gamertag claim key (stats_key.txt).
// Blocking: call them off the game's threads. `error` gets a short reason.
bool SendPlayerMessage(uint32_t title_id, const std::string& from, const std::string& to,
                       const std::string& text, std::string* error);
bool GetInbox(uint32_t title_id, const std::string& player, int64_t after_id,
              std::vector<PlayerMessage>& out);

}  // namespace rex::system::xam::stats
