/**
 * @file        kernel/xam/xam_stats.cpp
 * @brief       Xbox Live user stats (leaderboards) on top of stats_service:
 *              XSessionWriteStats, XUserReadStats and the stats enumerators
 *              (XamUserCreateStatsEnumerator + XamEnumerate).
 *
 * Guest structures (big-endian):
 *   XUSER_STATS_SPEC            0x88  view id, column count, u16 column ids[64]
 *   XUSER_STATS_READ_RESULTS    0x08  view count, views
 *   XUSER_STATS_VIEW            0x10  view id, total rows, row count, rows
 *   XUSER_STATS_ROW             0x30  xuid, rank, i64 rating @0x10, gamertag[16] @0x18,
 *                                     column count @0x28, columns @0x2C
 *   XUSER_STATS_COLUMN          0x18  u16 column id, XUSER_DATA @8
 *   XSESSION_VIEW_PROPERTIES    0x0C  view id, property count, properties
 *   XUSER_PROPERTY              0x18  property id, XUSER_DATA @8
 *   XUSER_DATA                  0x10  type byte, value @8 (strings/binary: size, pointer)
 * PGR4 parses results in sub_828AA908 (enumerator) and sizes XUserReadStats
 * buffers with the XDK formula used in ResultsSize below.
 *
 * Players on the server are gamertags with a derived XUID, so local profile
 * and friend XUIDs are translated to that XUID on the way out and back on the
 * way in.
 */

#include <algorithm>
#include <cstring>
#include <unordered_map>

#include <fmt/format.h>

#include <rex/hook.h>
#include <rex/kernel/xam/private.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/string/utf8.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/stats_service.h>
#include <rex/system/xam/user_profile.h>
#include <rex/system/xenumerator.h>
#include <rex/system/xtypes.h>
#include <rex/types.h>

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;
using namespace rex::system::xam;

namespace {

constexpr uint32_t kSpecSize = 0x88;
constexpr uint32_t kMaxSpecColumns = 64;
constexpr uint32_t kViewSize = 0x10;
constexpr uint32_t kRowSize = 0x30;
constexpr uint32_t kColumnSize = 0x18;
constexpr uint32_t kPropertySize = 0x18;
constexpr uint32_t kMaxRows = 100;

struct Spec {
  uint32_t view_id;
  std::vector<uint16_t> columns;
};

memory::Memory* Mem() {
  return REX_KERNEL_MEMORY();
}

std::vector<Spec> ReadSpecs(uint32_t count, uint32_t specs_ptr) {
  std::vector<Spec> specs;
  if (!specs_ptr) return specs;
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* s = Mem()->TranslateVirtual<const uint8_t*>(specs_ptr + i * kSpecSize);
    Spec spec;
    spec.view_id = memory::load_and_swap<uint32_t>(s + 0);
    const uint32_t n = std::min(memory::load_and_swap<uint32_t>(s + 4), kMaxSpecColumns);
    for (uint32_t c = 0; c < n; ++c) {
      spec.columns.push_back(memory::load_and_swap<uint16_t>(s + 8 + c * 2));
    }
    specs.push_back(std::move(spec));
  }
  return specs;
}

// The XDK's own size for rows_per_view rows of every spec (0x34 per row and
// 0x1C per column leave room for alignment).
uint32_t ResultsSize(uint32_t rows_per_view, const std::vector<Spec>& specs) {
  uint32_t size = 8;
  for (const auto& s : specs) {
    size += kViewSize + rows_per_view * (0x34 + uint32_t(s.columns.size()) * 0x1C);
  }
  return size;
}

// --- Identity: guest XUIDs <-> the server's gamertag XUIDs ------------------

std::string GamertagForXuid(uint64_t xuid) {
  for (uint32_t i = 0; i < 4; ++i) {
    const UserProfile* p = REX_KERNEL_STATE()->user_profile(i);
    if (p && p->xuid() == xuid) return p->name();
  }
  return xeXFriendsGamertag(xuid);
}

struct XuidMap {
  std::unordered_map<uint64_t, uint64_t> to_guest;

  XuidMap() {
    // Local players always map back to their own profile XUIDs.
    for (uint32_t i = 0; i < 4; ++i) {
      const UserProfile* p = REX_KERNEL_STATE()->user_profile(i);
      if (p && !p->name().empty()) to_guest[stats::XuidForGamertag(p->name())] = p->xuid();
    }
  }

  uint64_t ToServer(uint64_t guest_xuid) {
    const std::string gamertag = GamertagForXuid(guest_xuid);
    if (gamertag.empty()) return guest_xuid;
    const uint64_t server = stats::XuidForGamertag(gamertag);
    to_guest[server] = guest_xuid;
    return server;
  }

  uint64_t ToGuest(uint64_t server_xuid) const {
    auto it = to_guest.find(server_xuid);
    return it == to_guest.end() ? server_xuid : it->second;
  }
};

// --- Results writer ---------------------------------------------------------

void WriteData(uint8_t* d, const stats::Value& v) {
  // Strings and binary would need extra buffer space the XDK sizing doesn't
  // reserve; PGR4 skips those types anyway, so they read as null.
  std::memset(d, 0, 16);
  switch (v.type) {
    case stats::kTypeContext:
    case stats::kTypeInt32:
      d[0] = v.type;
      memory::store_and_swap<uint32_t>(d + 8, uint32_t(int32_t(v.number)));
      break;
    case stats::kTypeInt64:
    case stats::kTypeDateTime:
      d[0] = v.type;
      memory::store_and_swap<uint64_t>(d + 8, uint64_t(v.number));
      break;
    case stats::kTypeDouble:
      d[0] = v.type;
      memory::store_and_swap<double>(d + 8, v.real);
      break;
    case stats::kTypeFloat:
      d[0] = v.type;
      memory::store_and_swap<float>(d + 8, float(v.real));
      break;
    default:
      d[0] = stats::kTypeNull;
      break;
  }
}

stats::Value ColumnValue(const stats::Row& row, uint16_t id) {
  stats::Value v;
  if (id == stats::kColumnRank) {
    v.type = stats::kTypeInt32;
    v.number = row.rank;
  } else if (id == stats::kColumnRating) {
    v.type = stats::kTypeInt64;
    v.number = row.rating;
  } else {
    for (const auto& c : row.columns) {
      if (c.id == id) return c.value;
    }
  }
  return v;
}

// Fills XUSER_STATS_READ_RESULTS at buffer_ptr, one view per spec, dropping
// trailing rows that don't fit in buffer_size.
void WriteResults(uint32_t buffer_ptr, uint32_t buffer_size, const std::vector<Spec>& specs,
                  std::vector<stats::ViewRows> views, const XuidMap& map) {
  uint8_t* base = Mem()->TranslateVirtual<uint8_t*>(buffer_ptr);
  if (buffer_size < 8) return;
  std::memset(base, 0, buffer_size);
  views.resize(specs.size());
  for (size_t i = 0; i < specs.size(); ++i) {
    views[i].view_id = specs[i].view_id;
  }

  auto needed = [&] {
    uint32_t size = 8 + uint32_t(specs.size()) * kViewSize;
    for (size_t i = 0; i < specs.size(); ++i) {
      size += uint32_t(views[i].rows.size()) *
              (kRowSize + uint32_t(specs[i].columns.size()) * kColumnSize);
    }
    return size;
  };
  while (needed() > buffer_size) {
    auto it = std::max_element(views.begin(), views.end(), [](const auto& a, const auto& b) {
      return a.rows.size() < b.rows.size();
    });
    if (it == views.end() || it->rows.empty()) break;
    it->rows.pop_back();
  }
  if (needed() > buffer_size) {
    memory::store_and_swap<uint32_t>(base + 0, 0);
    memory::store_and_swap<uint32_t>(base + 4, 0);
    return;
  }

  const uint32_t views_ptr = buffer_ptr + 8;
  uint32_t cursor = views_ptr + uint32_t(specs.size()) * kViewSize;
  memory::store_and_swap<uint32_t>(base + 0, uint32_t(specs.size()));
  memory::store_and_swap<uint32_t>(base + 4, specs.empty() ? 0 : views_ptr);
  for (size_t i = 0; i < specs.size(); ++i) {
    const auto& spec = specs[i];
    const auto& view = views[i];
    uint8_t* v = Mem()->TranslateVirtual<uint8_t*>(views_ptr + uint32_t(i) * kViewSize);
    const uint32_t rows_ptr = cursor;
    const uint32_t row_count = uint32_t(view.rows.size());
    memory::store_and_swap<uint32_t>(v + 0, spec.view_id);
    memory::store_and_swap<uint32_t>(v + 4, view.total);
    memory::store_and_swap<uint32_t>(v + 8, row_count);
    memory::store_and_swap<uint32_t>(v + 12, row_count ? rows_ptr : 0);
    cursor += row_count * kRowSize;
    for (uint32_t r = 0; r < row_count; ++r) {
      const auto& row = view.rows[r];
      uint8_t* out = Mem()->TranslateVirtual<uint8_t*>(rows_ptr + r * kRowSize);
      memory::store_and_swap<uint64_t>(out + 0x00, map.ToGuest(row.xuid));
      memory::store_and_swap<uint32_t>(out + 0x08, row.rank);
      memory::store_and_swap<uint64_t>(out + 0x10, uint64_t(row.rating));
      std::strncpy(reinterpret_cast<char*>(out + 0x18), row.gamertag.c_str(), 15);
      const uint32_t cols_ptr = cursor;
      const uint32_t col_count = uint32_t(spec.columns.size());
      memory::store_and_swap<uint32_t>(out + 0x28, col_count);
      memory::store_and_swap<uint32_t>(out + 0x2C, col_count ? cols_ptr : 0);
      cursor += col_count * kColumnSize;
      for (uint32_t c = 0; c < col_count; ++c) {
        uint8_t* col = Mem()->TranslateVirtual<uint8_t*>(cols_ptr + c * kColumnSize);
        memory::store_and_swap<uint16_t>(col + 0, spec.columns[c]);
        WriteData(col + 8, ColumnValue(row, spec.columns[c]));
      }
    }
  }
}

// --- Enumerator -------------------------------------------------------------

class XStatsEnumerator : public XEnumerator {
 public:
  XStatsEnumerator(KernelState* kernel_state, uint32_t buffer_size, uint32_t title_id, bool by_rank,
                   uint64_t pivot, uint32_t num_rows, std::vector<Spec> specs)
      : XEnumerator(kernel_state, 1, buffer_size),
        title_id_(title_id),
        by_rank_(by_rank),
        pivot_(pivot),
        num_rows_(num_rows),
        specs_(std::move(specs)) {}

  // Runs off the title's threads (XamEnumerate with an overlapped is
  // completed deferred), so the network round trip happens here.
  uint32_t WriteItems(uint32_t buffer_ptr, uint8_t* buffer_data, uint32_t* written_count) override {
    if (done_) return X_ERROR_NO_MORE_FILES;
    done_ = true;
    XuidMap map;
    const uint64_t pivot = by_rank_ ? 0 : map.ToServer(pivot_);
    std::vector<stats::ViewRows> views;
    for (const auto& spec : specs_) {
      stats::ViewRows rows;
      bool ok = by_rank_ ? stats::ReadByRank(title_id_, spec.view_id, uint32_t(pivot_), num_rows_,
                                             rows)
                         : stats::ReadAroundXuid(title_id_, spec.view_id, pivot, num_rows_, rows);
      if (!ok) rows = {};
      views.push_back(std::move(rows));
    }
    REXKRNL_INFO("stats: enumerate {} view(s) {} {:X}: {} row(s) in view {}", specs_.size(),
                 by_rank_ ? "from rank" : "around", pivot_,
                 views.empty() ? 0 : views[0].rows.size(),
                 specs_.empty() ? 0 : specs_[0].view_id);
    WriteResults(buffer_ptr, uint32_t(item_size()), specs_, std::move(views), map);
    if (written_count) *written_count = 1;
    return X_ERROR_SUCCESS;
  }

 private:
  uint32_t title_id_;
  bool by_rank_;
  uint64_t pivot_;
  uint32_t num_rows_;
  std::vector<Spec> specs_;
  bool done_ = false;
};

std::string ReadUtf16(uint32_t ptr, uint32_t bytes) {
  std::u16string s;
  for (uint32_t i = 0; ptr && i + 1 < bytes; i += 2) {
    char16_t c = memory::load_and_swap<uint16_t>(Mem()->TranslateVirtual<uint8_t*>(ptr + i));
    if (!c) break;
    s.push_back(c);
  }
  return rex::string::to_utf8(s);
}

std::string Base64(const uint8_t* data, uint32_t size) {
  static const char* k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  for (uint32_t i = 0; i < size; i += 3) {
    uint32_t n = uint32_t(data[i]) << 16;
    if (i + 1 < size) n |= uint32_t(data[i + 1]) << 8;
    if (i + 2 < size) n |= data[i + 2];
    out += k[(n >> 18) & 63];
    out += k[(n >> 12) & 63];
    out += i + 1 < size ? k[(n >> 6) & 63] : '=';
    out += i + 2 < size ? k[n & 63] : '=';
  }
  return out;
}

stats::Value ReadData(const uint8_t* d) {
  stats::Value v;
  v.type = d[0];
  switch (v.type) {
    case stats::kTypeContext:
    case stats::kTypeInt32:
      v.number = int32_t(memory::load_and_swap<uint32_t>(d + 8));
      break;
    case stats::kTypeInt64:
    case stats::kTypeDateTime:
      v.number = int64_t(memory::load_and_swap<uint64_t>(d + 8));
      break;
    case stats::kTypeDouble:
      v.real = memory::load_and_swap<double>(d + 8);
      break;
    case stats::kTypeFloat:
      v.real = memory::load_and_swap<float>(d + 8);
      break;
    case stats::kTypeUnicode:
      v.text = ReadUtf16(memory::load_and_swap<uint32_t>(d + 12),
                         std::min(memory::load_and_swap<uint32_t>(d + 8), 1024u));
      break;
    case stats::kTypeBinary: {
      const uint32_t size = std::min(memory::load_and_swap<uint32_t>(d + 8), 256u);
      const uint32_t ptr = memory::load_and_swap<uint32_t>(d + 12);
      v.text = ptr ? Base64(Mem()->TranslateVirtual<uint8_t*>(ptr), size) : "";
      break;
    }
    default:
      v.type = stats::kTypeNull;
      break;
  }
  return v;
}

}  // namespace

void xeXSessionWriteStats(uint64_t xuid, uint32_t num_views, uint32_t views_ptr) {
  if (!stats::Enabled() || !views_ptr) return;
  const std::string gamertag = GamertagForXuid(xuid);
  if (gamertag.empty()) {
    REXKRNL_INFO("stats: skipping write for {:016X} (not a local player)", xuid);
    return;
  }
  // Everyone without a gamertag set is "User": uploading would claim that
  // shared name on the server for whoever got there first.
  if (gamertag == "User") {
    REXKRNL_INFO("stats: not uploading for the default gamertag \"User\" (set one in the "
                 "launcher or Guide > Change Gamertag)");
    return;
  }
  std::vector<stats::ViewWrite> writes;
  for (uint32_t i = 0; i < std::min(num_views, 64u); ++i) {
    const uint8_t* v = Mem()->TranslateVirtual<const uint8_t*>(views_ptr + i * 12);
    stats::ViewWrite w;
    w.view_id = memory::load_and_swap<uint32_t>(v + 0);
    const uint32_t count = std::min(memory::load_and_swap<uint32_t>(v + 4), 64u);
    const uint32_t props_ptr = memory::load_and_swap<uint32_t>(v + 8);
    std::string log;
    for (uint32_t p = 0; props_ptr && p < count; ++p) {
      const uint8_t* e = Mem()->TranslateVirtual<const uint8_t*>(props_ptr + p * kPropertySize);
      stats::Property prop;
      prop.id = memory::load_and_swap<uint32_t>(e + 0);
      prop.value = ReadData(e + 8);
      if (prop.value.type == stats::kTypeNull) continue;
      log += fmt::format(" {:08X}={}", prop.id,
                         prop.value.type == stats::kTypeDouble || prop.value.type == stats::kTypeFloat
                             ? fmt::format("{}", prop.value.real)
                         : prop.value.text.empty() ? std::to_string(prop.value.number)
                                                   : prop.value.text);
      w.properties.push_back(std::move(prop));
    }
    REXKRNL_INFO("stats: write \"{}\" view {}:{}", gamertag, w.view_id, log);
    writes.push_back(std::move(w));
  }
  stats::SubmitWrite(REX_KERNEL_STATE()->title_id(), gamertag, std::move(writes));
}

X_RESULT xeXUserReadStats(uint32_t title_id, uint32_t xuid_count, uint32_t xuids_ptr,
                          uint32_t spec_count, uint32_t specs_ptr, uint32_t results_size,
                          uint32_t results_ptr) {
  if (!results_ptr || results_size < 8) return X_ERROR_SUCCESS;
  auto specs = ReadSpecs(std::min(spec_count, 64u), specs_ptr);
  XuidMap map;
  std::vector<uint64_t> xuids;
  for (uint32_t i = 0; xuids_ptr && i < std::min(xuid_count, kMaxRows); ++i) {
    xuids.push_back(map.ToServer(
        memory::load_and_swap<uint64_t>(Mem()->TranslateVirtual<uint8_t*>(xuids_ptr + i * 8))));
  }
  std::vector<uint32_t> view_ids;
  for (const auto& s : specs) view_ids.push_back(s.view_id);
  std::vector<stats::ViewRows> found;
  if (stats::Enabled() && !xuids.empty() && !view_ids.empty()) {
    stats::ReadByXuids(title_id ? title_id : REX_KERNEL_STATE()->title_id(), xuids, view_ids,
                       found);
  }
  // Like Live: one row per requested player, in request order, per spec.
  // Players without an entry get rank 0, rating 0 and null columns. PGR4
  // needs that row: it loads each local player's personal best from it
  // (sub_824C5838 -> sub_8252C568) and only uploads a time when a best is
  // loaded and beaten (sub_825477C8).
  std::vector<stats::ViewRows> views(specs.size());
  for (size_t i = 0; i < specs.size(); ++i) {
    const stats::ViewRows* f = nullptr;
    for (const auto& v : found) {
      if (v.view_id == specs[i].view_id) f = &v;
    }
    views[i].view_id = specs[i].view_id;
    views[i].total = f ? f->total : 0;
    for (uint64_t x : xuids) {
      const stats::Row* hit = nullptr;
      for (size_t r = 0; f && r < f->rows.size(); ++r) {
        if (f->rows[r].xuid == x) hit = &f->rows[r];
      }
      if (hit) {
        views[i].rows.push_back(*hit);
      } else {
        stats::Row empty;
        empty.xuid = x;
        empty.gamertag = GamertagForXuid(map.ToGuest(x));
        views[i].rows.push_back(std::move(empty));
      }
    }
  }
  size_t ranked = 0;
  for (const auto& v : views) {
    for (const auto& r : v.rows) ranked += r.rank != 0;
  }
  REXKRNL_INFO("stats: read {} xuid(s) x {} view(s): {} ranked row(s), first view {}",
               xuids.size(), specs.size(), ranked, specs.empty() ? 0 : specs[0].view_id);
  // No rows (or no server) still writes an empty result: left unwritten,
  // PGR4 (sub_828A3270) walks whatever the buffer held.
  WriteResults(results_ptr, results_size, specs, std::move(views), map);
  return X_ERROR_SUCCESS;
}

// Export arguments arrive as the low 32 bits of their registers, so a XUID
// pivot loses its top half. "Around me" pivots on a local player: match the
// low half against the signed-in profiles.
uint64_t ResolvePivot(uint64_t pivot) {
  if (pivot >> 32) return pivot;
  for (uint32_t i = 0; i < 4; ++i) {
    const UserProfile* p = REX_KERNEL_STATE()->user_profile(i);
    if (p && uint32_t(p->xuid()) == uint32_t(pivot)) return p->xuid();
  }
  return pivot;
}

// type 0 = around a XUID, 1 = from a rank (PGR4: sub_828AA6A8 / sub_828AA5A8).
u32 XamUserCreateStatsEnumerator_entry(u32 title_id, u32 type, u64 pivot, u32 num_rows,
                                       u32 num_specs, mapped_void specs_ptr,
                                       mapped_u32 buffer_size_ptr, mapped_u32 handle_ptr) {
  if (!num_rows || !num_specs || !specs_ptr || !handle_ptr || type > 1) {
    return X_ERROR_INVALID_PARAMETER;
  }
  const uint32_t rows = std::min(uint32_t(num_rows), kMaxRows);
  auto specs = ReadSpecs(std::min(uint32_t(num_specs), 64u), specs_ptr.guest_address());
  const uint32_t size = ResultsSize(rows, specs);
  if (buffer_size_ptr) *buffer_size_ptr = size;
  auto e = object_ref<XStatsEnumerator>(new XStatsEnumerator(
      REX_KERNEL_STATE(), size, title_id ? uint32_t(title_id) : REX_KERNEL_STATE()->title_id(),
      type == 1, type == 1 ? uint64_t(pivot) : ResolvePivot(pivot), rows, std::move(specs)));
  auto result = e->Initialize(0, 0xFB, 0, 0, 0);
  if (XFAILED(result)) return result;
  *handle_ptr = e->handle();
  return X_ERROR_SUCCESS;
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

REX_EXPORT(__imp__XamUserCreateStatsEnumerator,
           rex::kernel::xam::XamUserCreateStatsEnumerator_entry)
