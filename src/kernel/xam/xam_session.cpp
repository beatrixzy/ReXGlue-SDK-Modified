/**
 * @file        kernel/xam/xam_session.cpp
 * @brief       Xbox Live sessions over the EOS room (matchmaking + QoS).
 *
 * With net_transport=eos, everyone in an EOS room acts as one Live
 * matchmaking pool:
 *   - a host's searchable session (created with public slots) is announced
 *     to the other room members over EOS P2P (eos::SetAnnouncement): its
 *     XSESSION_INFO, slots, the host's user contexts/properties and its
 *     XNetQosListen data;
 *   - XSessionSearch / SearchEx / SearchByID answer from those announcements
 *     in the console's XSESSION_SEARCHRESULT layout, filtered on the contexts
 *     the search names;
 *   - XNetQosLookup on a host found that way reports it contacted, with its
 *     QoS data, so titles list it.
 * Game traffic then goes to the host's XNADDR (its virtual IP) over the EOS
 * virtual LAN like System Link.
 *
 * Session handles: XamSessionCreateHandle hands out a distinct handle per
 * session and XamSessionRefObjByHandle a guest object for it, which the XGI
 * session messages then name.
 */

#include <cstring>
#include <map>
#include <mutex>
#include <vector>

#include <rex/kernel/xam/private.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/eos_service.h>
#include <rex/system/xevent.h>

namespace rex::kernel::xam {
using namespace rex::system;
namespace eos = rex::system::xam::eos;

namespace {

constexpr uint32_t kSessionInfoSize = 60;  // XNKID 8 + XNADDR 36 + XNKEY 16
constexpr uint32_t kSearchResultSize = 92;
constexpr uint32_t kUserContextSize = 8;
constexpr uint32_t kUserPropertySize = 24;
constexpr uint32_t kQosInfoSize = 24;

// XSESSION_CREATE_* flags.
constexpr uint32_t kCreateHost = 0x1;

constexpr uint8_t kAnnounceMagic[3] = {'X', 'S', 1};

struct Session {
  uint32_t handle = 0;
  uint32_t flags = 0;
  uint32_t max_public = 0, max_private = 0;
  uint32_t filled_public = 0, filled_private = 0;
  uint32_t user_index = 0;
  uint8_t info[kSessionInfoSize] = {};
  bool created = false;
};

struct Property {
  uint32_t id;
  uint64_t value;  // numeric types only (type = id >> 28)
};

struct Announced {
  uint32_t vip = 0;
  uint8_t info[kSessionInfoSize] = {};
  uint32_t flags = 0, max_public = 0, max_private = 0, filled_public = 0, filled_private = 0;
  std::vector<std::pair<uint32_t, uint32_t>> contexts;
  std::vector<Property> properties;
  std::vector<uint8_t> qos;
};

std::recursive_mutex g_mutex;
uint32_t g_next_handle = 0xCAFE0001;
std::map<uint32_t, uint32_t> g_object_by_handle;
std::map<uint32_t, Session> g_sessions;  // by guest object
// Per user: contexts and numeric properties set with XUserSetContext/Property.
std::map<uint32_t, uint32_t> g_contexts[4];
std::map<uint32_t, uint64_t> g_properties[4];
std::vector<uint8_t> g_qos_data;  // XNetQosListen data of our hosted session
uint32_t g_advertised = 0;        // guest object of the announced session

memory::Memory* Mem() {
  return REX_KERNEL_MEMORY();
}

template <typename T>
void Put(std::vector<uint8_t>& out, T v) {
  const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
  out.insert(out.end(), p, p + sizeof(T));
}

template <typename T>
bool Get(const std::vector<uint8_t>& in, size_t& pos, T& v) {
  if (pos + sizeof(T) > in.size()) {
    return false;
  }
  std::memcpy(&v, in.data() + pos, sizeof(T));
  pos += sizeof(T);
  return true;
}

// Picks the session to advertise (the latest created host session with
// public slots) and hands its announcement to EOS.
void UpdateAnnouncement() {
  const Session* best = nullptr;
  uint32_t best_obj = 0;
  for (const auto& [obj, s] : g_sessions) {
    if (s.created && (s.flags & kCreateHost) && s.max_public > 0) {
      best = &s;
      best_obj = obj;
    }
  }
  g_advertised = best_obj;
  if (!best) {
    eos::SetAnnouncement({});
    return;
  }
  const auto& contexts = g_contexts[best->user_index & 3];
  const auto& properties = g_properties[best->user_index & 3];
  std::vector<uint8_t> blob(kAnnounceMagic, kAnnounceMagic + 3);
  blob.insert(blob.end(), best->info, best->info + kSessionInfoSize);
  Put(blob, best->flags);
  Put(blob, best->max_public);
  Put(blob, best->max_private);
  Put(blob, best->filled_public);
  Put(blob, best->filled_private);
  Put(blob, static_cast<uint16_t>(contexts.size()));
  Put(blob, static_cast<uint16_t>(properties.size()));
  // Keep the whole announcement in one P2P packet.
  size_t fixed = blob.size() + 2 + contexts.size() * 8 + properties.size() * 12;
  size_t qos_len = g_qos_data.size();
  if (fixed + qos_len > eos::kMaxAnnouncementBytes) {
    REXKRNL_WARN("XSession: QoS data ({} bytes) too large to announce, trimmed", qos_len);
    qos_len = fixed < eos::kMaxAnnouncementBytes ? eos::kMaxAnnouncementBytes - fixed : 0;
  }
  Put(blob, static_cast<uint16_t>(qos_len));
  for (const auto& [id, value] : contexts) {
    Put(blob, id);
    Put(blob, value);
  }
  for (const auto& [id, value] : properties) {
    Put(blob, id);
    Put(blob, value);
  }
  blob.insert(blob.end(), g_qos_data.begin(), g_qos_data.begin() + qos_len);
  eos::SetAnnouncement(std::move(blob));
}

bool Parse(const eos::Announcement& a, Announced& out) {
  const auto& b = a.blob;
  if (b.size() < 3 + kSessionInfoSize || std::memcmp(b.data(), kAnnounceMagic, 3) != 0) {
    return false;
  }
  out.vip = a.vip;
  std::memcpy(out.info, b.data() + 3, kSessionInfoSize);
  size_t pos = 3 + kSessionInfoSize;
  uint16_t nctx = 0, nprop = 0, nqos = 0;
  if (!Get(b, pos, out.flags) || !Get(b, pos, out.max_public) || !Get(b, pos, out.max_private) ||
      !Get(b, pos, out.filled_public) || !Get(b, pos, out.filled_private) ||
      !Get(b, pos, nctx) || !Get(b, pos, nprop) || !Get(b, pos, nqos)) {
    return false;
  }
  for (uint16_t i = 0; i < nctx; ++i) {
    uint32_t id, value;
    if (!Get(b, pos, id) || !Get(b, pos, value)) return false;
    out.contexts.emplace_back(id, value);
  }
  for (uint16_t i = 0; i < nprop; ++i) {
    Property p;
    if (!Get(b, pos, p.id) || !Get(b, pos, p.value)) return false;
    out.properties.push_back(p);
  }
  if (pos + nqos > b.size()) {
    return false;
  }
  out.qos.assign(b.begin() + pos, b.begin() + pos + nqos);
  return true;
}

std::vector<Announced> CurrentAnnouncements() {
  std::vector<Announced> out;
  for (const auto& a : eos::Announcements()) {
    Announced parsed;
    if (Parse(a, parsed)) {
      out.push_back(std::move(parsed));
    }
  }
  return out;
}

// Writes an XSESSION_SEARCHRESULT_HEADER plus results (and their contexts and
// properties) into the guest buffer, trimming to what fits.
uint32_t WriteResults(const std::vector<const Announced*>& found, uint32_t max_results,
                      uint32_t buffer_size, uint32_t results_ptr) {
  if (!results_ptr) {
    return 0;
  }
  uint8_t* header = Mem()->TranslateVirtual<uint8_t*>(results_ptr);
  memory::store_and_swap<uint32_t>(header + 0, 0);
  memory::store_and_swap<uint32_t>(header + 4, 0);

  // Count how many fit: header, result array, then contexts/properties.
  uint32_t count = 0;
  uint32_t extra = 0;
  for (const auto* a : found) {
    if (count >= max_results) break;
    uint32_t need_extra = uint32_t(a->contexts.size()) * kUserContextSize +
                          uint32_t(a->properties.size()) * kUserPropertySize;
    if (8 + (count + 1) * kSearchResultSize + extra + need_extra > buffer_size) break;
    extra += need_extra;
    ++count;
  }
  const uint32_t array_ptr = results_ptr + 8;
  uint32_t extra_ptr = array_ptr + count * kSearchResultSize;
  for (uint32_t i = 0; i < count; ++i) {
    const Announced& a = *found[i];
    uint8_t* r = Mem()->TranslateVirtual<uint8_t*>(array_ptr + i * kSearchResultSize);
    std::memset(r, 0, kSearchResultSize);
    std::memcpy(r, a.info, kSessionInfoSize);
    auto open = [](uint32_t max, uint32_t filled) { return max > filled ? max - filled : 0; };
    memory::store_and_swap<uint32_t>(r + 60, open(a.max_public, a.filled_public));
    memory::store_and_swap<uint32_t>(r + 64, open(a.max_private, a.filled_private));
    memory::store_and_swap<uint32_t>(r + 68, a.filled_public);
    memory::store_and_swap<uint32_t>(r + 72, a.filled_private);
    memory::store_and_swap<uint32_t>(r + 76, uint32_t(a.properties.size()));
    memory::store_and_swap<uint32_t>(r + 80, uint32_t(a.contexts.size()));
    // XUSER_PROPERTY: id, pad, XUSER_DATA {type byte, pad, 8-byte value}.
    memory::store_and_swap<uint32_t>(r + 84, a.properties.empty() ? 0 : extra_ptr);
    for (const auto& p : a.properties) {
      uint8_t* e = Mem()->TranslateVirtual<uint8_t*>(extra_ptr);
      std::memset(e, 0, kUserPropertySize);
      memory::store_and_swap<uint32_t>(e + 0, p.id);
      const uint8_t type = uint8_t(p.id >> 28);
      e[8] = type;
      if (type == 2 || type == 3 || type == 7) {  // int64, double, datetime
        memory::store_and_swap<uint64_t>(e + 16, p.value);
      } else {  // int32, float
        memory::store_and_swap<uint32_t>(e + 16, uint32_t(p.value));
      }
      extra_ptr += kUserPropertySize;
    }
    memory::store_and_swap<uint32_t>(r + 88, a.contexts.empty() ? 0 : extra_ptr);
    for (const auto& [id, value] : a.contexts) {
      uint8_t* e = Mem()->TranslateVirtual<uint8_t*>(extra_ptr);
      memory::store_and_swap<uint32_t>(e + 0, id);
      memory::store_and_swap<uint32_t>(e + 4, value);
      extra_ptr += kUserContextSize;
    }
  }
  memory::store_and_swap<uint32_t>(header + 0, count);
  memory::store_and_swap<uint32_t>(header + 4, count ? array_ptr : 0);
  return count;
}

}  // namespace

uint32_t xeXSessionCreateHandle() {
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  uint32_t handle = g_next_handle++;
  uint32_t obj = Mem()->SystemHeapAlloc(64);
  std::memset(Mem()->TranslateVirtual(obj), 0, 64);
  g_object_by_handle[handle] = obj;
  g_sessions[obj].handle = handle;
  return handle;
}

uint32_t xeXSessionObject(uint32_t handle) {
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  auto it = g_object_by_handle.find(handle);
  return it == g_object_by_handle.end() ? 0 : it->second;
}

void xeXSessionCreate(uint32_t obj, uint32_t flags, uint32_t max_public, uint32_t max_private,
                      uint32_t user_index, const uint8_t* info) {
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  Session& s = g_sessions[obj];
  s.flags = flags;
  s.max_public = max_public;
  s.max_private = max_private;
  s.filled_public = s.filled_private = 0;
  s.user_index = user_index < 4 ? user_index : 0;
  if (info) {
    std::memcpy(s.info, info, kSessionInfoSize);
  }
  s.created = true;
  UpdateAnnouncement();
}

void xeXSessionJoin(uint32_t obj, uint32_t count, bool private_slots) {
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  auto it = g_sessions.find(obj);
  if (it == g_sessions.end()) return;
  (private_slots ? it->second.filled_private : it->second.filled_public) += count;
  if (obj == g_advertised) UpdateAnnouncement();
}

void xeXSessionLeave(uint32_t obj, uint32_t count) {
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  auto it = g_sessions.find(obj);
  if (it == g_sessions.end()) return;
  Session& s = it->second;
  uint32_t from_public = std::min(count, s.filled_public);
  s.filled_public -= from_public;
  s.filled_private -= std::min(count - from_public, s.filled_private);
  if (obj == g_advertised) UpdateAnnouncement();
}

void xeXSessionModify(uint32_t obj, uint32_t flags, uint32_t max_public, uint32_t max_private) {
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  auto it = g_sessions.find(obj);
  if (it == g_sessions.end()) return;
  // XSESSION_CREATE_* modify flags only change joinability; keep host/usage.
  it->second.flags = (it->second.flags & 0xFF) | (flags & ~0xFFu);
  it->second.max_public = max_public;
  it->second.max_private = max_private;
  UpdateAnnouncement();
}

void xeXSessionDelete(uint32_t obj) {
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  auto it = g_sessions.find(obj);
  if (it == g_sessions.end()) return;
  it->second.created = false;
  UpdateAnnouncement();
}

void xeXUserSetContext(uint32_t user_index, uint32_t id, uint32_t value) {
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  g_contexts[user_index & 3][id] = value;
  if (g_advertised) UpdateAnnouncement();
}

void xeXUserSetProperty(uint32_t user_index, uint32_t id, uint32_t size, uint32_t value_ptr) {
  const uint8_t type = uint8_t(id >> 28);
  if (!value_ptr || type == 4 || type == 6) {  // strings/binary are not announced
    return;
  }
  const uint8_t* p = Mem()->TranslateVirtual<const uint8_t*>(value_ptr);
  uint64_t value = size >= 8 ? memory::load_and_swap<uint64_t>(p)
                             : memory::load_and_swap<uint32_t>(p);
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  g_properties[user_index & 3][id] = value;
  if (g_advertised) UpdateAnnouncement();
}

uint32_t xeXSessionSearch(uint32_t max_results, uint32_t num_contexts, uint32_t contexts_ptr,
                          uint32_t buffer_size, uint32_t results_ptr) {
  std::vector<std::pair<uint32_t, uint32_t>> wanted;
  for (uint32_t i = 0; contexts_ptr && i < num_contexts; ++i) {
    const uint8_t* c = Mem()->TranslateVirtual<const uint8_t*>(contexts_ptr + i * 8);
    wanted.emplace_back(memory::load_and_swap<uint32_t>(c), memory::load_and_swap<uint32_t>(c + 4));
  }
  auto all = CurrentAnnouncements();
  std::vector<const Announced*> found;
  for (const auto& a : all) {
    bool match = a.max_public > a.filled_public;
    for (const auto& [id, value] : wanted) {
      for (const auto& [aid, avalue] : a.contexts) {
        if (aid == id && avalue != value) match = false;
      }
    }
    if (match) found.push_back(&a);
  }
  uint32_t count = WriteResults(found, max_results, buffer_size, results_ptr);
  REXKRNL_INFO("XSessionSearch: {} session(s) in the room, {} match, {} returned", all.size(),
               found.size(), count);
  return count;
}

uint32_t xeXSessionSearchById(const uint8_t* kid, uint32_t buffer_size, uint32_t results_ptr) {
  auto all = CurrentAnnouncements();
  std::vector<const Announced*> found;
  for (const auto& a : all) {
    if (std::memcmp(a.info, kid, 8) == 0) found.push_back(&a);
  }
  uint32_t count = WriteResults(found, 1, buffer_size, results_ptr);
  REXKRNL_INFO("XSessionSearchByID: {} found", count);
  return count;
}

bool xeXSessionCurrentInfo(uint8_t* out) {
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  // The session we host and advertise, else the latest one we joined that
  // has public slots (a game, not a private party).
  const Session* pick = nullptr;
  auto it = g_sessions.find(g_advertised);
  if (it != g_sessions.end() && it->second.created) {
    pick = &it->second;
  } else {
    for (const auto& [obj, s] : g_sessions) {
      if (s.created && s.max_public > 0) pick = &s;
    }
  }
  static const uint8_t kZero[8] = {};
  if (!pick || std::memcmp(pick->info, kZero, 8) == 0) {
    return false;
  }
  std::memcpy(out, pick->info, kSessionInfoSize);
  return true;
}

void xeXNetQosSetListenData(const uint8_t* data, uint32_t size, bool clear) {
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  if (clear) {
    g_qos_data.clear();
  } else if (data) {
    g_qos_data.assign(data, data + size);
  }
  if (g_advertised) UpdateAnnouncement();
}

uint32_t xeXNetQosLookup(uint32_t count, uint32_t xnaddr_ptrs, uint32_t probes) {
  auto all = CurrentAnnouncements();
  std::vector<const Announced*> targets(count, nullptr);
  uint32_t data_bytes = 0;
  for (uint32_t i = 0; i < count && xnaddr_ptrs; ++i) {
    uint32_t xnaddr = memory::load_and_swap<uint32_t>(
        Mem()->TranslateVirtual<const uint8_t*>(xnaddr_ptrs + i * 4));
    if (!xnaddr) continue;
    uint32_t ina = memory::load_and_swap<uint32_t>(Mem()->TranslateVirtual<const uint8_t*>(xnaddr));
    for (const auto& a : all) {
      if (a.vip == ina) {
        targets[i] = &a;
        data_bytes += uint32_t(a.qos.size());
      }
    }
  }
  // XNQOS: cxnqos, cxnqosPending, then XNQOSINFO[count], data blobs after.
  uint32_t size = 8 + std::max(count, 1u) * kQosInfoSize + data_bytes;
  uint32_t qos = Mem()->SystemHeapAlloc(size);
  uint8_t* q = Mem()->TranslateVirtual<uint8_t*>(qos);
  std::memset(q, 0, size);
  memory::store_and_swap<uint32_t>(q + 0, count);
  memory::store_and_swap<uint32_t>(q + 4, 0);  // nothing pending: answered at once
  uint32_t data_ptr = qos + 8 + std::max(count, 1u) * kQosInfoSize;
  uint32_t contacted = 0;
  for (uint32_t i = 0; i < count; ++i) {
    uint8_t* info = q + 8 + i * kQosInfoSize;
    const Announced* a = targets[i];
    // XNET_XNQOSINFO_COMPLETE 0x01, TARGET_CONTACTED 0x02, DATA_RECEIVED 0x08
    uint8_t flags = 0x01;
    if (a) {
      flags |= 0x02;
      ++contacted;
      memory::store_and_swap<uint16_t>(info + 2, uint16_t(probes));  // probes sent
      memory::store_and_swap<uint16_t>(info + 4, uint16_t(probes));  // probes received
      memory::store_and_swap<uint16_t>(info + 12, 30);               // RTT min (ms)
      memory::store_and_swap<uint16_t>(info + 14, 40);               // RTT median (ms)
      memory::store_and_swap<uint32_t>(info + 16, 1000000);          // up bits/s
      memory::store_and_swap<uint32_t>(info + 20, 1000000);          // down bits/s
      if (!a->qos.empty()) {
        flags |= 0x08;
        memory::store_and_swap<uint16_t>(info + 6, uint16_t(a->qos.size()));
        memory::store_and_swap<uint32_t>(info + 8, data_ptr);
        std::memcpy(Mem()->TranslateVirtual(data_ptr), a->qos.data(), a->qos.size());
        data_ptr += uint32_t(a->qos.size());
      }
    }
    info[0] = flags;
  }
  REXKRNL_INFO("XNetQosLookup: {} target(s), {} in the room", count, contacted);
  return qos;
}

}  // namespace rex::kernel::xam
