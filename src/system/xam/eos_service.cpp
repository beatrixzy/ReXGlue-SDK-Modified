/**
 * @file        system/xam/eos_service.cpp
 * @brief       Epic Online Services: online identity and an internet
 *              virtual LAN for System Link play.
 *
 * Adapted from the acre (Ace Combat) rexglue port's eos_transport.cpp:
 * credentials come from a file beside the executable instead of cvars, the
 * EOS DLL is delay-loaded (checked with LoadLibrary first, so builds run
 * without it), and the transport sits under XSocket instead of per-title
 * socket-wrapper hooks.
 *
 * Model: the EOS lobby only introduces peers. Once every member knows every
 * other member's ProductUserId and virtual IP, the title's own System Link
 * discovery (broadcasts) and session traffic run over P2P unchanged.
 *
 *   bring-up:  EOS_Initialize -> Platform_Create -> Connect device-ID login
 *              -> lobby search-or-create by net_room
 *              -> claim a virtual IP as a lobby member attribute
 *   send:      guest sendto(10.77.0.x | broadcast) -> EOS_P2P_SendPacket
 *              (unreliable unordered: titles bring their own reliability)
 *   receive:   the EOS thread drains EOS_P2P_ReceivePacket into per-port
 *              queues that recvfrom pops.
 *
 * Virtual-IP claims: each member publishes NET_VIP (int) as a public member
 * attribute and takes the lowest free value; on a collision the member with
 * the smaller ProductUserId keeps it and the other re-claims.
 *
 * Threading: one thread owns all EOS calls and EOS_Platform_Tick (all
 * callbacks fire on it); guest threads must never call EOS directly. A
 * recursive mutex guards the state that guest threads reach through
 * SendTo/RecvFrom.
 */

#include <rex/system/xam/eos_service.h>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_STRING(net_transport, "eos", "Network",
                      "System Link transport: 'lan' = real UDP on this network (or a VPN such "
                      "as Hamachi); 'eos' = over the internet through Epic Online Services "
                      "(needs eos_credentials.ini, EOSSDK-Win64-Shipping.dll and a shared "
                      "net_room; without them it falls back to lan)");
REXCVAR_DEFINE_STRING(net_room, "", "Network",
                      "net_transport=eos: empty = the public room everyone without a code "
                      "shares; a code = a private room only players with the same code join "
                      "(treat it like a password)");
REXCVAR_DEFINE_STRING(eos_credentials, "eos_credentials.ini", "Network",
                      "EOS credentials file ([eos] product_id, sandbox_id, deployment_id, "
                      "client_id, client_secret), relative to the executable's folder");
REXCVAR_DEFINE_BOOL(eos_log_verbose, false, "Network",
                    "Forward EOS SDK info-level logging (warnings and errors always show)");

#if REX_HAS_EOS && defined(_WIN32)

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#pragma comment(lib, "ole32.lib")

#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#include <vector>

#include <eos_common.h>
#include <eos_connect.h>
#include <eos_lobby.h>
#include <eos_logging.h>
#include <eos_p2p.h>
#include <eos_rtc.h>
#include <eos_rtc_audio.h>
#include <Windows/eos_Windows.h>
#include <eos_sdk.h>

namespace rex::system::xam::eos {
namespace {

constexpr wchar_t kDllName[] = L"EOSSDK-Win64-Shipping.dll";

// WSA numerics the titles' soft error paths understand.
constexpr int kWsaWouldBlock = 10035;   // WSAEWOULDBLOCK
constexpr int kWsaMsgSize = 10040;      // WSAEMSGSIZE
constexpr int kWsaNetDown = 10050;      // WSAENETDOWN
constexpr int kWsaHostUnreach = 10065;  // WSAEHOSTUNREACH

constexpr char kSocketName[] = "REXNET";
// Builds with voice keep their rooms apart from older ones (NET_ROOM): the
// public room an older build made has no voice channel and, never empty, was
// never remade. Older builds neither find these rooms nor are found.
constexpr char kRoomAttrKey[] = "NET_ROOM2";
constexpr char kVipAttrKey[] = "NET_VIP";
constexpr char kBucketId[] = "rexglue-systemlink:v2";
constexpr uint32_t kVirtualNetBase = 0x0A4D0000u;  // 10.77.0.0
// EOS's per-lobby maximum; System Link races still cap players themselves.
constexpr uint32_t kMaxLobbyMembers = 64;
// EOS refuses rooms with a voice channel above 16 members
// (errors.com.epicgames.lobbies.invalid_session). A full room makes players
// try the next one, or a new one.
constexpr uint32_t kMaxVoiceLobbyMembers = 16;
// Room key used when net_room is empty: everyone without a code shares it.
// Codes typed by players never collide with it (they cannot start with '~').
constexpr char kPublicRoom[] = "~public";
constexpr uint32_t kHeaderBytes = 4;  // {u16 src, u16 dst} big-endian
constexpr size_t kMaxQueuedPerPort = 512;
constexpr auto kRetryDelay = std::chrono::seconds(5);

enum class State {
  kIdle,
  kLoggingIn,
  kLobbySearching,
  kLobbyCreating,
  kLobbyJoining,
  kInLobby,
  kFailed,
};

enum class RetryStage { kNone, kAuth, kLobby };

struct Credentials {
  std::string product_id, sandbox_id, deployment_id, client_id, client_secret;
  bool complete() const {
    return !product_id.empty() && !sandbox_id.empty() && !deployment_id.empty() &&
           !client_id.empty() && !client_secret.empty();
  }
};

struct Peer {
  EOS_ProductUserId id = nullptr;
  std::string id_str;
  uint32_t vip = 0;  // 0 = no claim published yet
};

struct Datagram {
  std::vector<uint8_t> payload;
  uint32_t src_ip = 0;
  uint16_t src_port = 0;
};

std::recursive_mutex g_mutex;
std::atomic<bool> g_active{false};   // eos transport selected and starting
std::atomic<bool> g_running{false};  // worker loop should keep going
std::thread g_thread;

Credentials g_creds;
std::string g_display_name;
std::string g_room;

State g_state = State::kIdle;
// A room change asked for from the Guide, applied by the worker thread.
std::string g_pending_room;
bool g_room_switch = false;
bool g_pending_create = false;  // make a new room rather than join one
std::function<void(const std::string&)> g_switch_note;  // told how it went
// Making a room: the search checking the code is free, and whether to create
// (not search) once the old lobby is left.
EOS_HLobbySearch g_check_search = nullptr;
std::string g_check_room;
bool g_create_after_leave = false;
bool g_announce_created = false;
RetryStage g_retry_stage = RetryStage::kNone;
std::chrono::steady_clock::time_point g_next_retry{};

EOS_HPlatform g_platform = nullptr;
EOS_HConnect g_connect = nullptr;
EOS_HLobby g_lobby = nullptr;
EOS_HP2P g_p2p = nullptr;
EOS_HRTCAudio g_rtc_audio = nullptr;  // null: no voice (no xaudio2_9redist.dll)
EOS_ProductUserId g_local_puid = nullptr;
std::string g_local_puid_str;
std::string g_lobby_id;
bool g_is_lobby_owner = false;
uint32_t g_self_vip = 0;

EOS_HLobbySearch g_search = nullptr;
EOS_HLobbyDetails g_join_details = nullptr;

std::vector<Peer> g_peers;  // remote members only
std::set<uint16_t> g_bound_ports;
std::unordered_map<uint16_t, std::deque<Datagram>> g_rx;

// Broadcast channels (each peer keeps the latest blob per sender):
// 1 = Live session announcements, 2 = presence. Channel 3 carries direct
// messages (invites). Game traffic is channel 0.
constexpr uint8_t kSessionChannel = 1;  // kPresenceChannel (2) is in the header
constexpr uint8_t kDirectChannel = 3;
constexpr auto kBroadcastInterval = std::chrono::seconds(1);
constexpr auto kBroadcastExpiry = std::chrono::seconds(4);
struct Received {
  uint32_t vip = 0;
  std::vector<uint8_t> blob;
  std::chrono::steady_clock::time_point when;
};
struct BroadcastState {
  std::vector<uint8_t> blob;
  bool dirty = false;
  std::chrono::steady_clock::time_point last{};
  std::unordered_map<std::string, Received> received;  // by sender ProductUserId
};
BroadcastState g_broadcasts[2];  // channels 1 and 2
BroadcastState* BroadcastFor(uint8_t channel) {
  return channel == kSessionChannel || channel == kPresenceChannel ? &g_broadcasts[channel - 1]
                                                                    : nullptr;
}
struct Outgoing {
  std::string puid;
  std::vector<uint8_t> blob;
};
std::deque<Outgoing> g_outbox;    // direct messages waiting for the EOS thread
std::deque<PeerBlob> g_inbox;     // direct messages received
std::set<std::string> g_traffic;  // peers we exchanged game traffic with
// Voice is only heard from the players in your game lobby (System Link or
// Xbox LIVE): room members whose session ID matches ours, from presence.
std::set<std::string> g_lobby_mates;

std::filesystem::path ExeDir() {
  wchar_t buf[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, buf, MAX_PATH);
  return std::filesystem::path(buf).parent_path();
}

std::string Trim(std::string s) {
  const char* ws = " \t\r\n";
  s.erase(0, s.find_first_not_of(ws));
  s.erase(s.find_last_not_of(ws) + 1);
  return s;
}

bool ReadCredentials(const std::filesystem::path& path, Credentials& out) {
  std::ifstream in(path);
  if (!in) {
    return false;
  }
  std::string line;
  while (std::getline(in, line)) {
    auto eq = line.find('=');
    if (eq == std::string::npos) {
      continue;
    }
    std::string key = Trim(line.substr(0, eq)), value = Trim(line.substr(eq + 1));
    if (key == "product_id") out.product_id = value;
    else if (key == "sandbox_id") out.sandbox_id = value;
    else if (key == "deployment_id") out.deployment_id = value;
    else if (key == "client_id") out.client_id = value;
    else if (key == "client_secret") out.client_secret = value;
  }
  return out.complete();
}

// Codes match case-insensitively (stored in capitals); none = public.
std::string NormalizeRoom(std::string code) {
  for (char& c : code) {
    c = char(std::toupper(static_cast<unsigned char>(c)));
  }
  return code.empty() ? std::string(kPublicRoom) : code;
}

std::string RoomLabel() {
  return g_room == kPublicRoom ? std::string("the public room") : "private room '" + g_room + "'";
}

std::string FormatIp(uint32_t ip) {
  char buf[20];
  std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
                (ip >> 8) & 0xFF, ip & 0xFF);
  return buf;
}

std::string PuidToString(EOS_ProductUserId id) {
  char buf[EOS_PRODUCTUSERID_MAX_LENGTH + 1] = {};
  int32_t len = sizeof(buf);
  if (EOS_ProductUserId_ToString(id, buf, &len) != EOS_EResult::EOS_Success) {
    return "<invalid>";
  }
  return buf;
}

bool RateLimited(std::atomic<int>& counter, int limit = 20) {
  return counter.fetch_add(1, std::memory_order_relaxed) >= limit;
}

void Fail(RetryStage stage, const char* what, EOS_EResult r) {
  REXSYS_WARN("eos: {} failed: {} (retrying in 5 s)", what, EOS_EResult_ToString(r));
  g_state = State::kFailed;
  g_retry_stage = stage;
  g_next_retry = std::chrono::steady_clock::now() + kRetryDelay;
}

// ---------------------------------------------------------------------------
// Peer table <- lobby membership.

void ClaimVip();

// Rebuilds g_peers from the lobby members and their NET_VIP attributes;
// yields our own claim when a smaller ProductUserId holds the same value.
void RefreshPeers() {
  if (g_lobby_id.empty() || !g_local_puid) {
    return;
  }
  EOS_Lobby_CopyLobbyDetailsHandleOptions copy_opt = {};
  copy_opt.ApiVersion = EOS_LOBBY_COPYLOBBYDETAILSHANDLE_API_LATEST;
  copy_opt.LobbyId = g_lobby_id.c_str();
  copy_opt.LocalUserId = g_local_puid;
  EOS_HLobbyDetails details = nullptr;
  const EOS_EResult cr = EOS_Lobby_CopyLobbyDetailsHandle(g_lobby, &copy_opt, &details);
  if (cr != EOS_EResult::EOS_Success || !details) {
    REXSYS_WARN("eos: CopyLobbyDetailsHandle: {}", EOS_EResult_ToString(cr));
    return;
  }

  EOS_LobbyDetails_GetMemberCountOptions count_opt = {};
  count_opt.ApiVersion = EOS_LOBBYDETAILS_GETMEMBERCOUNT_API_LATEST;
  const uint32_t count = EOS_LobbyDetails_GetMemberCount(details, &count_opt);

  std::vector<Peer> peers;
  bool self_conflict = false;
  for (uint32_t i = 0; i < count; ++i) {
    EOS_LobbyDetails_GetMemberByIndexOptions member_opt = {};
    member_opt.ApiVersion = EOS_LOBBYDETAILS_GETMEMBERBYINDEX_API_LATEST;
    member_opt.MemberIndex = i;
    EOS_ProductUserId member = EOS_LobbyDetails_GetMemberByIndex(details, &member_opt);
    if (!member) {
      continue;
    }
    Peer p;
    p.id = member;
    p.id_str = PuidToString(member);
    if (p.id_str == g_local_puid_str) {
      continue;
    }
    EOS_LobbyDetails_CopyMemberAttributeByKeyOptions attr_opt = {};
    attr_opt.ApiVersion = EOS_LOBBYDETAILS_COPYMEMBERATTRIBUTEBYKEY_API_LATEST;
    attr_opt.TargetUserId = member;
    attr_opt.AttrKey = kVipAttrKey;
    EOS_Lobby_Attribute* attr = nullptr;
    if (EOS_LobbyDetails_CopyMemberAttributeByKey(details, &attr_opt, &attr) ==
            EOS_EResult::EOS_Success &&
        attr && attr->Data) {
      p.vip = static_cast<uint32_t>(attr->Data->Value.AsInt64);
    }
    if (attr) {
      EOS_Lobby_Attribute_Release(attr);
    }
    if (p.vip != 0 && p.vip == g_self_vip && p.id_str < g_local_puid_str) {
      self_conflict = true;
    }
    peers.push_back(std::move(p));
  }
  EOS_LobbyDetails_Release(details);

  if (peers.size() != g_peers.size()) {
    // Tell newcomers about our session and presence right away.
    for (auto& b : g_broadcasts) {
      b.dirty = true;
    }
  }
  g_peers = std::move(peers);
  if (g_self_vip == 0 || self_conflict) {
    if (self_conflict) {
      REXSYS_WARN("eos: virtual IP {} taken by another player, picking another",
                  FormatIp(kVirtualNetBase | g_self_vip));
      g_self_vip = 0;
    }
    ClaimVip();
  }
  int mapped = 0;
  std::string listing;
  for (const auto& p : g_peers) {
    if (p.vip) {
      ++mapped;
    }
    listing += " " + p.id_str.substr(0, 8) + "=" +
               (p.vip ? FormatIp(kVirtualNetBase | p.vip) : "unclaimed");
  }
  REXSYS_INFO("eos: room peers {}/{}:{}", mapped, g_peers.size(), listing);
}

// Publishes our virtual-IP claim (and, as owner, the room code searchers
// match on).
void ClaimVip() {
  if (g_lobby_id.empty() || !g_local_puid) {
    return;
  }
  if (g_self_vip == 0) {
    std::set<uint32_t> used;
    for (const auto& p : g_peers) {
      if (p.vip) {
        used.insert(p.vip);
      }
    }
    for (uint32_t v = 1; v <= 250; ++v) {
      if (!used.count(v)) {
        g_self_vip = v;
        break;
      }
    }
  }

  EOS_Lobby_UpdateLobbyModificationOptions mod_opt = {};
  mod_opt.ApiVersion = EOS_LOBBY_UPDATELOBBYMODIFICATION_API_LATEST;
  mod_opt.LocalUserId = g_local_puid;
  mod_opt.LobbyId = g_lobby_id.c_str();
  EOS_HLobbyModification mod = nullptr;
  EOS_EResult r = EOS_Lobby_UpdateLobbyModification(g_lobby, &mod_opt, &mod);
  if (r != EOS_EResult::EOS_Success || !mod) {
    REXSYS_WARN("eos: UpdateLobbyModification: {}", EOS_EResult_ToString(r));
    return;
  }

  EOS_Lobby_AttributeData vip_attr = {};
  vip_attr.ApiVersion = EOS_LOBBY_ATTRIBUTEDATA_API_LATEST;
  vip_attr.Key = kVipAttrKey;
  vip_attr.Value.AsInt64 = static_cast<int64_t>(g_self_vip);
  vip_attr.ValueType = EOS_EAttributeType::EOS_AT_INT64;
  EOS_LobbyModification_AddMemberAttributeOptions member_opt = {};
  member_opt.ApiVersion = EOS_LOBBYMODIFICATION_ADDMEMBERATTRIBUTE_API_LATEST;
  member_opt.Attribute = &vip_attr;
  member_opt.Visibility = EOS_ELobbyAttributeVisibility::EOS_LAT_PUBLIC;
  r = EOS_LobbyModification_AddMemberAttribute(mod, &member_opt);
  if (r != EOS_EResult::EOS_Success) {
    REXSYS_WARN("eos: AddMemberAttribute: {}", EOS_EResult_ToString(r));
  }

  if (g_is_lobby_owner) {
    EOS_Lobby_AttributeData room_attr = {};
    room_attr.ApiVersion = EOS_LOBBY_ATTRIBUTEDATA_API_LATEST;
    room_attr.Key = kRoomAttrKey;
    room_attr.Value.AsUtf8 = g_room.c_str();
    room_attr.ValueType = EOS_EAttributeType::EOS_AT_STRING;
    EOS_LobbyModification_AddAttributeOptions attr_opt = {};
    attr_opt.ApiVersion = EOS_LOBBYMODIFICATION_ADDATTRIBUTE_API_LATEST;
    attr_opt.Attribute = &room_attr;
    attr_opt.Visibility = EOS_ELobbyAttributeVisibility::EOS_LAT_PUBLIC;
    r = EOS_LobbyModification_AddAttribute(mod, &attr_opt);
    if (r != EOS_EResult::EOS_Success) {
      REXSYS_WARN("eos: AddAttribute(room): {}", EOS_EResult_ToString(r));
    }
  }

  EOS_Lobby_UpdateLobbyOptions update_opt = {};
  update_opt.ApiVersion = EOS_LOBBY_UPDATELOBBY_API_LATEST;
  update_opt.LobbyModificationHandle = mod;
  EOS_Lobby_UpdateLobby(g_lobby, &update_opt, nullptr,
                        [](const EOS_Lobby_UpdateLobbyCallbackInfo* info) {
                          if (info->ResultCode != EOS_EResult::EOS_Success) {
                            REXSYS_WARN("eos: UpdateLobby (virtual IP claim): {}",
                                        EOS_EResult_ToString(info->ResultCode));
                          }
                        });
  EOS_LobbyModification_Release(mod);
  REXSYS_INFO("eos: virtual IP {} ({} other player(s) in the room)",
              FormatIp(kVirtualNetBase | g_self_vip), g_peers.size());
}

// ---------------------------------------------------------------------------
// Lobby search-or-create by room code.

void CreateRoomLobby();
bool g_lobby_rtc = false;          // the room being made has a voice channel
bool g_lobby_rtc_refused = false;  // EOS refused one this run: make rooms without

// Rooms the search found, tried in order; a full one fails the join and the
// next is tried, and when none are left a new room is created.
std::deque<EOS_HLobbyDetails> g_candidates;

void ReleaseCandidates() {
  for (auto d : g_candidates) {
    EOS_LobbyDetails_Release(d);
  }
  g_candidates.clear();
}

void OnJoinLobby(const EOS_Lobby_JoinLobbyCallbackInfo* info);

void JoinNextCandidate() {
  if (g_candidates.empty()) {
    CreateRoomLobby();
    return;
  }
  g_join_details = g_candidates.front();
  g_candidates.pop_front();
  g_state = State::kLobbyJoining;
  EOS_Lobby_JoinLobbyOptions join_opt = {};
  join_opt.ApiVersion = EOS_LOBBY_JOINLOBBY_API_LATEST;
  join_opt.LobbyDetailsHandle = g_join_details;
  join_opt.LocalUserId = g_local_puid;
  join_opt.bPresenceEnabled = EOS_FALSE;
  static EOS_Lobby_LocalRTCOptions rtc_opt = {};
  rtc_opt.ApiVersion = EOS_LOBBY_LOCALRTCOPTIONS_API_LATEST;
  rtc_opt.bLocalAudioDeviceInputStartsMuted = EOS_TRUE;
  join_opt.LocalRTCOptions = g_rtc_audio ? &rtc_opt : nullptr;
  EOS_Lobby_JoinLobby(g_lobby, &join_opt, nullptr, &OnJoinLobby);
}

void OnJoinLobby(const EOS_Lobby_JoinLobbyCallbackInfo* info) {
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  if (g_join_details) {
    EOS_LobbyDetails_Release(g_join_details);
    g_join_details = nullptr;
  }
  if (info->ResultCode != EOS_EResult::EOS_Success) {
    if (!g_candidates.empty() || info->ResultCode == EOS_EResult::EOS_Lobby_TooManyPlayers) {
      REXSYS_INFO("eos: room unavailable ({}), trying the next",
                  EOS_EResult_ToString(info->ResultCode));
      JoinNextCandidate();
      return;
    }
    Fail(RetryStage::kLobby, "JoinLobby", info->ResultCode);
    return;
  }
  ReleaseCandidates();
  g_lobby_id = info->LobbyId ? info->LobbyId : "";
  g_is_lobby_owner = false;
  g_state = State::kInLobby;
  REXSYS_INFO("eos: joined {}", RoomLabel());
  RefreshPeers();  // also claims our virtual IP
}

void OnCreateLobby(const EOS_Lobby_CreateLobbyCallbackInfo* info) {
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  if (info->ResultCode != EOS_EResult::EOS_Success) {
    // A room with a voice channel can be refused (the product's client policy
    // without voice: EOS_Lobby_InvalidSession); online play must not hang on
    // it, so the room is made again without voice.
    if (g_lobby_rtc && !g_lobby_rtc_refused) {
      g_lobby_rtc_refused = true;
      REXSYS_WARN("eos: a room with voice was refused ({}); making it without voice. Allow voice "
                  "in the EOS client policy to use voice chat.",
                  EOS_EResult_ToString(info->ResultCode));
      CreateRoomLobby();
      return;
    }
    Fail(RetryStage::kLobby, "CreateLobby", info->ResultCode);
    return;
  }
  g_lobby_id = info->LobbyId ? info->LobbyId : "";
  g_is_lobby_owner = true;
  g_state = State::kInLobby;
  REXSYS_INFO("eos: created {}", RoomLabel());
  if (g_announce_created) {
    g_announce_created = false;
    if (g_switch_note) {
      g_switch_note("You're in private room " + g_room + ". Share the code with your friends.");
    }
  }
  RefreshPeers();
  if (g_self_vip == 0) {
    ClaimVip();
  }
}

void CreateRoomLobby() {
  g_state = State::kLobbyCreating;
  EOS_Lobby_CreateLobbyOptions opt = {};
  opt.ApiVersion = EOS_LOBBY_CREATELOBBY_API_LATEST;
  opt.LocalUserId = g_local_puid;
  opt.PermissionLevel = EOS_ELobbyPermissionLevel::EOS_LPL_PUBLICADVERTISED;
  opt.bPresenceEnabled = EOS_FALSE;
  opt.bAllowInvites = EOS_TRUE;
  opt.BucketId = kBucketId;
  opt.bDisableHostMigration = EOS_FALSE;
  // A voice room comes with every room we make; the mic starts muted and
  // voice stays off until the player turns it on (ApplyVoice).
  static EOS_Lobby_LocalRTCOptions rtc_opt = {};
  rtc_opt.ApiVersion = EOS_LOBBY_LOCALRTCOPTIONS_API_LATEST;
  rtc_opt.bLocalAudioDeviceInputStartsMuted = EOS_TRUE;
  g_lobby_rtc = g_rtc_audio && !g_lobby_rtc_refused;
  opt.MaxLobbyMembers = g_lobby_rtc ? kMaxVoiceLobbyMembers : kMaxLobbyMembers;
  opt.bEnableRTCRoom = g_lobby_rtc ? EOS_TRUE : EOS_FALSE;
  opt.LocalRTCOptions = g_lobby_rtc ? &rtc_opt : nullptr;
  EOS_Lobby_CreateLobby(g_lobby, &opt, nullptr, &OnCreateLobby);
}

void OnLobbySearchDone(const EOS_LobbySearch_FindCallbackInfo* info) {
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  if (!g_search) {
    return;
  }
  uint32_t count = 0;
  if (info->ResultCode == EOS_EResult::EOS_Success ||
      info->ResultCode == EOS_EResult::EOS_NotFound) {
    EOS_LobbySearch_GetSearchResultCountOptions count_opt = {};
    count_opt.ApiVersion = EOS_LOBBYSEARCH_GETSEARCHRESULTCOUNT_API_LATEST;
    count = EOS_LobbySearch_GetSearchResultCount(g_search, &count_opt);
  } else {
    EOS_LobbySearch_Release(g_search);
    g_search = nullptr;
    Fail(RetryStage::kLobby, "LobbySearch_Find", info->ResultCode);
    return;
  }
  ReleaseCandidates();
  for (uint32_t i = 0; i < count; ++i) {
    EOS_LobbySearch_CopySearchResultByIndexOptions copy_opt = {};
    copy_opt.ApiVersion = EOS_LOBBYSEARCH_COPYSEARCHRESULTBYINDEX_API_LATEST;
    copy_opt.LobbyIndex = i;
    EOS_HLobbyDetails details = nullptr;
    if (EOS_LobbySearch_CopySearchResultByIndex(g_search, &copy_opt, &details) !=
            EOS_EResult::EOS_Success ||
        !details) {
      continue;
    }
    // Skip full rooms up front (a join would fail anyway).
    EOS_LobbyDetails_CopyInfoOptions info_opt = {};
    info_opt.ApiVersion = EOS_LOBBYDETAILS_COPYINFO_API_LATEST;
    EOS_LobbyDetails_Info* info_out = nullptr;
    bool full = false;
    if (EOS_LobbyDetails_CopyInfo(details, &info_opt, &info_out) == EOS_EResult::EOS_Success &&
        info_out) {
      full = info_out->AvailableSlots == 0;
      EOS_LobbyDetails_Info_Release(info_out);
    }
    if (full) {
      EOS_LobbyDetails_Release(details);
    } else {
      g_candidates.push_back(details);
    }
  }
  EOS_LobbySearch_Release(g_search);
  g_search = nullptr;
  JoinNextCandidate();  // creates a room when nothing joinable was found
}

void StartLobbyFlow() {
  g_state = State::kLobbySearching;
  g_lobby_id.clear();
  g_peers.clear();

  EOS_Lobby_CreateLobbySearchOptions search_opt = {};
  search_opt.ApiVersion = EOS_LOBBY_CREATELOBBYSEARCH_API_LATEST;
  // Lobbies of crashed hosts linger a while and still take result slots.
  search_opt.MaxResults = 10;
  EOS_EResult r = EOS_Lobby_CreateLobbySearch(g_lobby, &search_opt, &g_search);
  if (r != EOS_EResult::EOS_Success || !g_search) {
    Fail(RetryStage::kLobby, "CreateLobbySearch", r);
    return;
  }
  EOS_Lobby_AttributeData room_param = {};
  room_param.ApiVersion = EOS_LOBBY_ATTRIBUTEDATA_API_LATEST;
  room_param.Key = kRoomAttrKey;
  room_param.Value.AsUtf8 = g_room.c_str();
  room_param.ValueType = EOS_EAttributeType::EOS_AT_STRING;
  EOS_LobbySearch_SetParameterOptions param_opt = {};
  param_opt.ApiVersion = EOS_LOBBYSEARCH_SETPARAMETER_API_LATEST;
  param_opt.Parameter = &room_param;
  param_opt.ComparisonOp = EOS_EComparisonOp::EOS_CO_EQUAL;
  r = EOS_LobbySearch_SetParameter(g_search, &param_opt);
  if (r != EOS_EResult::EOS_Success) {
    EOS_LobbySearch_Release(g_search);
    g_search = nullptr;
    Fail(RetryStage::kLobby, "LobbySearch_SetParameter", r);
    return;
  }
  EOS_LobbySearch_FindOptions find_opt = {};
  find_opt.ApiVersion = EOS_LOBBYSEARCH_FIND_API_LATEST;
  find_opt.LocalUserId = g_local_puid;
  EOS_LobbySearch_Find(g_search, &find_opt, nullptr, &OnLobbySearchDone);
}

// ---------------------------------------------------------------------------
// Lobby / P2P notifications.

void RegisterNotifications() {
  {
    EOS_Lobby_AddNotifyLobbyMemberStatusReceivedOptions opt = {};
    opt.ApiVersion = EOS_LOBBY_ADDNOTIFYLOBBYMEMBERSTATUSRECEIVED_API_LATEST;
    EOS_Lobby_AddNotifyLobbyMemberStatusReceived(
        g_lobby, &opt, nullptr,
        [](const EOS_Lobby_LobbyMemberStatusReceivedCallbackInfo* info) {
          std::lock_guard<std::recursive_mutex> lock(g_mutex);
          // News about a room we already left (switching rooms).
          if (info->LobbyId && g_lobby_id != info->LobbyId) {
            return;
          }
          const bool about_self = PuidToString(info->TargetUserId) == g_local_puid_str;
          using S = EOS_ELobbyMemberStatus;
          if (about_self &&
              (info->CurrentStatus == S::EOS_LMS_LEFT || info->CurrentStatus == S::EOS_LMS_KICKED ||
               info->CurrentStatus == S::EOS_LMS_CLOSED ||
               info->CurrentStatus == S::EOS_LMS_DISCONNECTED)) {
            REXSYS_WARN("eos: removed from the room (status {}), rejoining in 5 s",
                        static_cast<int>(info->CurrentStatus));
            g_lobby_id.clear();
            g_peers.clear();
            g_self_vip = 0;
            Fail(RetryStage::kLobby, "room membership", EOS_EResult::EOS_NoConnection);
            return;
          }
          RefreshPeers();
        });
  }
  {
    EOS_Lobby_AddNotifyLobbyMemberUpdateReceivedOptions opt = {};
    opt.ApiVersion = EOS_LOBBY_ADDNOTIFYLOBBYMEMBERUPDATERECEIVED_API_LATEST;
    EOS_Lobby_AddNotifyLobbyMemberUpdateReceived(
        g_lobby, &opt, nullptr, [](const EOS_Lobby_LobbyMemberUpdateReceivedCallbackInfo*) {
          std::lock_guard<std::recursive_mutex> lock(g_mutex);
          RefreshPeers();  // member attributes (virtual IP claims) changed
        });
  }
  {
    EOS_Lobby_AddNotifyLobbyUpdateReceivedOptions opt = {};
    opt.ApiVersion = EOS_LOBBY_ADDNOTIFYLOBBYUPDATERECEIVED_API_LATEST;
    EOS_Lobby_AddNotifyLobbyUpdateReceived(g_lobby, &opt, nullptr,
                                           [](const EOS_Lobby_LobbyUpdateReceivedCallbackInfo*) {
                                             std::lock_guard<std::recursive_mutex> lock(g_mutex);
                                             RefreshPeers();
                                           });
  }
  {
    // Accept P2P connections on our socket name; the lobby is the
    // introduction mechanism, the title's session keys gate membership.
    EOS_P2P_AddNotifyPeerConnectionRequestOptions opt = {};
    opt.ApiVersion = EOS_P2P_ADDNOTIFYPEERCONNECTIONREQUEST_API_LATEST;
    opt.LocalUserId = g_local_puid;
    opt.SocketId = nullptr;
    EOS_P2P_AddNotifyPeerConnectionRequest(
        g_p2p, &opt, nullptr, [](const EOS_P2P_OnIncomingConnectionRequestInfo* info) {
          std::lock_guard<std::recursive_mutex> lock(g_mutex);
          if (!info->SocketId || std::strcmp(info->SocketId->SocketName, kSocketName) != 0) {
            return;
          }
          EOS_P2P_AcceptConnectionOptions accept_opt = {};
          accept_opt.ApiVersion = EOS_P2P_ACCEPTCONNECTION_API_LATEST;
          accept_opt.LocalUserId = g_local_puid;
          accept_opt.RemoteUserId = info->RemoteUserId;
          EOS_P2P_SocketId sid = {};
          sid.ApiVersion = EOS_P2P_SOCKETID_API_LATEST;
          std::snprintf(sid.SocketName, sizeof(sid.SocketName), "%s", kSocketName);
          accept_opt.SocketId = &sid;
          EOS_P2P_AcceptConnection(g_p2p, &accept_opt);
          REXSYS_INFO("eos: accepted P2P connection from {}", PuidToString(info->RemoteUserId));
        });
  }
}

// ---------------------------------------------------------------------------
// Connect (device-ID) login.

void OnConnectLogin(const EOS_Connect_LoginCallbackInfo* info);

void DoConnectLogin() {
  // A token refresh while in the room must not flip Ready() off.
  if (g_state != State::kInLobby) {
    g_state = State::kLoggingIn;
  }
  EOS_Connect_Credentials creds = {};
  creds.ApiVersion = EOS_CONNECT_CREDENTIALS_API_LATEST;
  creds.Token = nullptr;
  creds.Type = EOS_EExternalCredentialType::EOS_ECT_DEVICEID_ACCESS_TOKEN;
  EOS_Connect_UserLoginInfo login_info = {};
  login_info.ApiVersion = EOS_CONNECT_USERLOGININFO_API_LATEST;
  login_info.DisplayName = g_display_name.c_str();
  EOS_Connect_LoginOptions opt = {};
  opt.ApiVersion = EOS_CONNECT_LOGIN_API_LATEST;
  opt.Credentials = &creds;
  opt.UserLoginInfo = &login_info;
  EOS_Connect_Login(g_connect, &opt, nullptr, &OnConnectLogin);
}

void OnLoginComplete(EOS_ProductUserId puid) {
  g_local_puid = puid;
  g_local_puid_str = PuidToString(puid);
  REXSYS_INFO("eos: logged in as {} (\"{}\")", g_local_puid_str, g_display_name);

  static bool notifications_registered = false;
  if (!notifications_registered) {
    notifications_registered = true;
    EOS_Connect_AddNotifyAuthExpirationOptions exp_opt = {};
    exp_opt.ApiVersion = EOS_CONNECT_ADDNOTIFYAUTHEXPIRATION_API_LATEST;
    EOS_Connect_AddNotifyAuthExpiration(g_connect, &exp_opt, nullptr,
                                        [](const EOS_Connect_AuthExpirationCallbackInfo*) {
                                          std::lock_guard<std::recursive_mutex> lock(g_mutex);
                                          REXSYS_INFO("eos: login expiring, refreshing");
                                          DoConnectLogin();  // same user, same room
                                        });
    RegisterNotifications();
  }
  if (g_lobby_id.empty()) {
    StartLobbyFlow();
  } else {
    g_state = State::kInLobby;
  }
}

void OnConnectLogin(const EOS_Connect_LoginCallbackInfo* info) {
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  if (info->ResultCode == EOS_EResult::EOS_Success) {
    OnLoginComplete(info->LocalUserId);
    return;
  }
  if (info->ResultCode == EOS_EResult::EOS_InvalidUser && info->ContinuanceToken) {
    // First run on this PC: no EOS user behind the device ID yet.
    EOS_Connect_CreateUserOptions opt = {};
    opt.ApiVersion = EOS_CONNECT_CREATEUSER_API_LATEST;
    opt.ContinuanceToken = info->ContinuanceToken;
    EOS_Connect_CreateUser(g_connect, &opt, nullptr,
                           [](const EOS_Connect_CreateUserCallbackInfo* create_info) {
                             std::lock_guard<std::recursive_mutex> lock(g_mutex);
                             if (create_info->ResultCode != EOS_EResult::EOS_Success) {
                               Fail(RetryStage::kAuth, "Connect_CreateUser",
                                    create_info->ResultCode);
                               return;
                             }
                             REXSYS_INFO("eos: created an EOS user for this PC");
                             OnLoginComplete(create_info->LocalUserId);
                           });
    return;
  }
  Fail(RetryStage::kAuth, "Connect_Login", info->ResultCode);
}

void StartAuth() {
  g_state = State::kLoggingIn;
  EOS_Connect_CreateDeviceIdOptions opt = {};
  opt.ApiVersion = EOS_CONNECT_CREATEDEVICEID_API_LATEST;
  opt.DeviceModel = "PC Windows";
  EOS_Connect_CreateDeviceId(g_connect, &opt, nullptr,
                             [](const EOS_Connect_CreateDeviceIdCallbackInfo* info) {
                               std::lock_guard<std::recursive_mutex> lock(g_mutex);
                               // DuplicateNotAllowed: this PC already has one.
                               if (info->ResultCode != EOS_EResult::EOS_Success &&
                                   info->ResultCode != EOS_EResult::EOS_DuplicateNotAllowed) {
                                 Fail(RetryStage::kAuth, "CreateDeviceId", info->ResultCode);
                                 return;
                               }
                               DoConnectLogin();
                             });
}

// ---------------------------------------------------------------------------
// Receive pump (EOS thread).

void DrainPackets() {
  if (!g_local_puid) {
    return;
  }
  static std::atomic<int> drop_unknown_log{0};
  static std::atomic<int> drop_unbound_log{0};
  for (int budget = 0; budget < 512; ++budget) {
    EOS_P2P_GetNextReceivedPacketSizeOptions size_opt = {};
    size_opt.ApiVersion = EOS_P2P_GETNEXTRECEIVEDPACKETSIZE_API_LATEST;
    size_opt.LocalUserId = g_local_puid;
    size_opt.RequestedChannel = nullptr;
    uint32_t size = 0;
    if (EOS_P2P_GetNextReceivedPacketSize(g_p2p, &size_opt, &size) != EOS_EResult::EOS_Success) {
      break;  // nothing waiting
    }
    uint8_t buf[EOS_P2P_MAX_PACKET_SIZE];
    EOS_P2P_ReceivePacketOptions recv_opt = {};
    recv_opt.ApiVersion = EOS_P2P_RECEIVEPACKET_API_LATEST;
    recv_opt.LocalUserId = g_local_puid;
    recv_opt.MaxDataSizeBytes = sizeof(buf);
    recv_opt.RequestedChannel = nullptr;
    EOS_ProductUserId sender = nullptr;
    EOS_P2P_SocketId sid = {};
    uint8_t channel = 0;
    uint32_t bytes = 0;
    if (EOS_P2P_ReceivePacket(g_p2p, &recv_opt, &sender, &sid, &channel, buf, &bytes) !=
        EOS_EResult::EOS_Success) {
      break;
    }
    uint32_t src_ip = 0;
    const std::string sender_str = PuidToString(sender);
    for (const auto& p : g_peers) {
      if (p.id_str == sender_str) {
        src_ip = p.vip ? (kVirtualNetBase | p.vip) : 0;
        break;
      }
    }
    if (auto* b = BroadcastFor(channel)) {
      if (src_ip) {
        if (bytes <= 1) {
          b->received.erase(sender_str);  // withdrawn
        } else {
          auto& r = b->received[sender_str];
          r.vip = src_ip;
          r.blob.assign(buf, buf + bytes);
          r.when = std::chrono::steady_clock::now();
        }
      }
      continue;
    }
    if (channel == kDirectChannel) {
      if (g_inbox.size() < 64) {
        g_inbox.push_back({src_ip, sender_str, std::vector<uint8_t>(buf, buf + bytes)});
      }
      continue;
    }
    if (bytes < kHeaderBytes) {
      continue;
    }
    if (src_ip == 0) {
      if (!RateLimited(drop_unknown_log)) {
        REXSYS_WARN("eos: dropped a packet from {} (no virtual IP yet)", sender_str);
      }
      continue;
    }
    const uint16_t src_port = static_cast<uint16_t>((buf[0] << 8) | buf[1]);
    const uint16_t dst_port = static_cast<uint16_t>((buf[2] << 8) | buf[3]);
    if (!g_bound_ports.count(dst_port)) {
      if (!RateLimited(drop_unbound_log)) {
        REXSYS_WARN("eos: dropped a packet for port {} (nothing bound)", dst_port);
      }
      continue;
    }
    auto& q = g_rx[dst_port];
    if (q.size() >= kMaxQueuedPerPort) {
      q.pop_front();  // shed the oldest
    }
    Datagram d;
    d.payload.assign(buf + kHeaderBytes, buf + bytes);
    d.src_ip = src_ip;
    d.src_port = src_port;
    q.push_back(std::move(d));
    g_traffic.insert(sender_str);
  }
}

// Reliable control packet on a non-game channel.
void SendControl(EOS_ProductUserId to, uint8_t channel, const uint8_t* data, uint32_t len) {
  EOS_P2P_SocketId sid = {};
  sid.ApiVersion = EOS_P2P_SOCKETID_API_LATEST;
  std::snprintf(sid.SocketName, sizeof(sid.SocketName), "%s", kSocketName);
  EOS_P2P_SendPacketOptions opt = {};
  opt.ApiVersion = EOS_P2P_SENDPACKET_API_LATEST;
  opt.LocalUserId = g_local_puid;
  opt.RemoteUserId = to;
  opt.SocketId = &sid;
  opt.Channel = channel;
  opt.DataLengthBytes = len;
  opt.Data = data;
  opt.bAllowDelayedDelivery = EOS_TRUE;
  opt.Reliability = EOS_EPacketReliability::EOS_PR_ReliableOrdered;
  opt.bDisableAutoAcceptConnection = EOS_FALSE;
  EOS_P2P_SendPacket(g_p2p, &opt);
}

// Sends each broadcast (possibly empty = withdrawn) to every peer when it
// changed and once a second, drops ones that stopped arriving, and delivers
// queued direct messages.
void PumpBroadcasts() {
  const auto now = std::chrono::steady_clock::now();
  for (auto& b : g_broadcasts) {
    for (auto it = b.received.begin(); it != b.received.end();) {
      it = now - it->second.when > kBroadcastExpiry ? b.received.erase(it) : std::next(it);
    }
  }
  if (g_state != State::kInLobby || !g_local_puid) {
    return;
  }
  static const uint8_t kEmpty = 0;
  for (uint8_t channel : {kSessionChannel, kPresenceChannel}) {
    auto& b = *BroadcastFor(channel);
    const bool periodic = !b.blob.empty() && now - b.last >= kBroadcastInterval;
    if (!b.dirty && !periodic) {
      continue;
    }
    b.dirty = false;
    b.last = now;
    for (const auto& p : g_peers) {
      if (p.vip) {
        // A withdrawal is one zero byte (P2P packets cannot be empty).
        SendControl(p.id, channel, b.blob.empty() ? &kEmpty : b.blob.data(),
                    b.blob.empty() ? 1 : static_cast<uint32_t>(b.blob.size()));
      }
    }
  }
  while (!g_outbox.empty()) {
    Outgoing out = std::move(g_outbox.front());
    g_outbox.pop_front();
    for (const auto& p : g_peers) {
      if (p.id_str == out.puid) {
        SendControl(p.id, kDirectChannel, out.blob.data(), static_cast<uint32_t>(out.blob.size()));
      }
    }
  }
}

void EOS_CALL EosLogSink(const EOS_LogMessage* msg) {
  if (!msg || !msg->Message) {
    return;
  }
  if (msg->Level <= EOS_ELogLevel::EOS_LOG_Error) {
    REXSYS_ERROR("eos sdk [{}] {}", msg->Category, msg->Message);
  } else if (msg->Level <= EOS_ELogLevel::EOS_LOG_Warning) {
    REXSYS_WARN("eos sdk [{}] {}", msg->Category, msg->Message);
  } else if (REXCVAR_GET(eos_log_verbose)) {
    REXSYS_INFO("eos sdk [{}] {}", msg->Category, msg->Message);
  }
}

bool InitPlatform() {
  EOS_InitializeOptions init_opt = {};
  init_opt.ApiVersion = EOS_INITIALIZE_API_LATEST;
  init_opt.ProductName = "rexglue";
  init_opt.ProductVersion = "1.0";
  const EOS_EResult ir = EOS_Initialize(&init_opt);
  if (ir != EOS_EResult::EOS_Success && ir != EOS_EResult::EOS_AlreadyConfigured) {
    REXSYS_ERROR("eos: EOS_Initialize: {}", EOS_EResult_ToString(ir));
    return false;
  }
  EOS_Logging_SetCallback(&EosLogSink);
  EOS_Logging_SetLogLevel(EOS_ELogCategory::EOS_LC_ALL_CATEGORIES,
                          REXCVAR_GET(eos_log_verbose) ? EOS_ELogLevel::EOS_LOG_Info
                                                       : EOS_ELogLevel::EOS_LOG_Warning);

  static std::string cache;
  auto cache_dir = ExeDir() / "eos_cache";
  std::error_code ec;
  std::filesystem::create_directories(cache_dir, ec);
  cache = cache_dir.string();

  EOS_Platform_Options plat_opt = {};
  plat_opt.ApiVersion = EOS_PLATFORM_OPTIONS_API_LATEST;
  plat_opt.ProductId = g_creds.product_id.c_str();
  plat_opt.SandboxId = g_creds.sandbox_id.c_str();
  plat_opt.DeploymentId = g_creds.deployment_id.c_str();
  plat_opt.ClientCredentials.ClientId = g_creds.client_id.c_str();
  plat_opt.ClientCredentials.ClientSecret = g_creds.client_secret.c_str();
  plat_opt.bIsServer = EOS_FALSE;
  plat_opt.Flags = EOS_PF_DISABLE_OVERLAY;
  plat_opt.CacheDirectory = cache.c_str();
  // Voice chat (lobby RTC) needs XAudio 2.9's redistributable beside the exe.
  static std::string xaudio_path;
  static EOS_Windows_RTCOptions windows_rtc = {};
  static EOS_Platform_RTCOptions rtc = {};
  const auto xaudio = ExeDir() / "xaudio2_9redist.dll";
  if (std::filesystem::exists(xaudio, ec)) {
    xaudio_path = xaudio.string();
    windows_rtc.ApiVersion = EOS_WINDOWS_RTCOPTIONS_API_LATEST;
    windows_rtc.XAudio29DllPath = xaudio_path.c_str();
    rtc.ApiVersion = EOS_PLATFORM_RTCOPTIONS_API_LATEST;
    rtc.PlatformSpecificOptions = &windows_rtc;
    rtc.BackgroundMode = EOS_ERTCBackgroundMode::EOS_RTCBM_KeepRoomsAlive;
    plat_opt.RTCOptions = &rtc;
  } else {
    REXSYS_INFO("eos: no xaudio2_9redist.dll beside the exe; voice chat off");
  }
  g_platform = EOS_Platform_Create(&plat_opt);
  if (!g_platform) {
    REXSYS_ERROR("eos: EOS_Platform_Create failed (check {})", REXCVAR_GET(eos_credentials));
    return false;
  }
  g_connect = EOS_Platform_GetConnectInterface(g_platform);
  g_lobby = EOS_Platform_GetLobbyInterface(g_platform);
  g_p2p = EOS_Platform_GetP2PInterface(g_platform);
  if (plat_opt.RTCOptions) {
    if (EOS_HRTC rtc_handle = EOS_Platform_GetRTCInterface(g_platform)) {
      g_rtc_audio = EOS_RTC_GetAudioInterface(rtc_handle);
    }
  }
  return g_connect && g_lobby && g_p2p;
}

void Note(const std::string& text) {
  REXSYS_INFO("eos: {}", text);
  if (g_switch_note) {
    g_switch_note(text);
  }
}

// After leaving the old lobby: join the new room's (search, else create), or
// create it straight away when making a room.
void EnterNewRoom() {
  if (g_create_after_leave) {
    g_create_after_leave = false;
    g_announce_created = true;
    g_lobby_id.clear();
    g_peers.clear();
    CreateRoomLobby();
  } else {
    StartLobbyFlow();
  }
}

// Leaves the current room's lobby (if in one) for room; our virtual IP is
// claimed again there.
void MoveToRoom(const std::string& room, bool create) {
  g_room = room;
  REXSYS_INFO("eos: {} {}", create ? "making" : "moving to", RoomLabel());
  g_retry_stage = RetryStage::kNone;
  g_create_after_leave = create;
  const std::string old_lobby = g_lobby_id;
  g_lobby_id.clear();
  g_peers.clear();
  g_self_vip = 0;
  if (old_lobby.empty()) {
    EnterNewRoom();
    return;
  }
  g_state = State::kLobbySearching;
  EOS_Lobby_LeaveLobbyOptions opt = {};
  opt.ApiVersion = EOS_LOBBY_LEAVELOBBY_API_LATEST;
  opt.LocalUserId = g_local_puid;
  opt.LobbyId = old_lobby.c_str();
  EOS_Lobby_LeaveLobby(g_lobby, &opt, nullptr, [](const EOS_Lobby_LeaveLobbyCallbackInfo* info) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (info->ResultCode != EOS_EResult::EOS_Success) {
      REXSYS_WARN("eos: LeaveLobby: {}", EOS_EResult_ToString(info->ResultCode));
    }
    EnterNewRoom();
  });
}

void OnRoomCheckDone(const EOS_LobbySearch_FindCallbackInfo* info) {
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  if (!g_check_search) {
    return;
  }
  uint32_t count = 0;
  const bool ok = info->ResultCode == EOS_EResult::EOS_Success ||
                  info->ResultCode == EOS_EResult::EOS_NotFound;
  if (ok) {
    EOS_LobbySearch_GetSearchResultCountOptions count_opt = {};
    count_opt.ApiVersion = EOS_LOBBYSEARCH_GETSEARCHRESULTCOUNT_API_LATEST;
    count = EOS_LobbySearch_GetSearchResultCount(g_check_search, &count_opt);
  }
  EOS_LobbySearch_Release(g_check_search);
  g_check_search = nullptr;
  if (!ok) {
    Note("Couldn't check room code " + g_check_room + " (" +
         EOS_EResult_ToString(info->ResultCode) + "). Try again.");
  } else if (count > 0) {
    Note("Room code " + g_check_room + " is already in use. Pick another code.");
  } else {
    MoveToRoom(g_check_room, true);
  }
}

// Making a room: first looks for a lobby already using the code, from the
// room we are still in.
void StartRoomCheck(const std::string& room) {
  g_check_room = room;
  EOS_Lobby_CreateLobbySearchOptions search_opt = {};
  search_opt.ApiVersion = EOS_LOBBY_CREATELOBBYSEARCH_API_LATEST;
  search_opt.MaxResults = 1;
  EOS_EResult r = EOS_Lobby_CreateLobbySearch(g_lobby, &search_opt, &g_check_search);
  if (r != EOS_EResult::EOS_Success || !g_check_search) {
    g_check_search = nullptr;
    Note(std::string("Couldn't check the room code (") + EOS_EResult_ToString(r) + ").");
    return;
  }
  EOS_Lobby_AttributeData room_param = {};
  room_param.ApiVersion = EOS_LOBBY_ATTRIBUTEDATA_API_LATEST;
  room_param.Key = kRoomAttrKey;
  room_param.Value.AsUtf8 = g_check_room.c_str();
  room_param.ValueType = EOS_EAttributeType::EOS_AT_STRING;
  EOS_LobbySearch_SetParameterOptions param_opt = {};
  param_opt.ApiVersion = EOS_LOBBYSEARCH_SETPARAMETER_API_LATEST;
  param_opt.Parameter = &room_param;
  param_opt.ComparisonOp = EOS_EComparisonOp::EOS_CO_EQUAL;
  r = EOS_LobbySearch_SetParameter(g_check_search, &param_opt);
  if (r != EOS_EResult::EOS_Success) {
    EOS_LobbySearch_Release(g_check_search);
    g_check_search = nullptr;
    Note(std::string("Couldn't check the room code (") + EOS_EResult_ToString(r) + ").");
    return;
  }
  EOS_LobbySearch_FindOptions find_opt = {};
  find_opt.ApiVersion = EOS_LOBBYSEARCH_FIND_API_LATEST;
  find_opt.LocalUserId = g_local_puid;
  EOS_LobbySearch_Find(g_check_search, &find_opt, nullptr, &OnRoomCheckDone);
}

void ApplyRoomSwitch() {
  g_room_switch = false;
  if (g_pending_create) {
    g_pending_create = false;
    StartRoomCheck(g_pending_room);
  } else {
    MoveToRoom(g_pending_room, false);
  }
}

// ---- Voice ----
std::atomic<bool> g_voice_on{false};
std::atomic<bool> g_mic_muted{false};  // voice on, but not sending
std::set<std::string> g_voice_muted;     // players this one muted (puid strings)
std::set<std::string> g_voice_talking;   // speaking now
std::string g_voice_room;                // the current lobby's RTC room, "" = none
std::string g_voice_room_lobby;          // the lobby g_voice_room belongs to
EOS_NotificationId g_voice_notify = EOS_INVALID_NOTIFICATIONID;
// Our own speaking: EOS reports others only, so the outgoing mic audio is
// measured (AudioBeforeSend) and counts as talking until this time (ms).
EOS_NotificationId g_voice_send_notify = EOS_INVALID_NOTIFICATIONID;
std::atomic<int64_t> g_self_talk_until{0};
std::vector<MicDevice> g_mics;           // last query
bool g_mics_refresh = true;              // query the devices again
bool g_mics_querying = false;
std::string g_mic_id;                    // chosen input; "" = system default
bool g_mic_dirty = true;                 // apply g_mic_id
std::string g_mic_user;                  // the sign-in it was applied for

int64_t SteadyMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Runs on EOS's audio thread: no locks.
void OnAudioBeforeSend(const EOS_RTCAudio_AudioBeforeSendCallbackInfo* info) {
  const EOS_RTCAudio_AudioBuffer* buf = info->Buffer;
  if (!buf || !buf->Frames || !buf->FramesCount) return;
  const uint32_t n = buf->FramesCount * std::max<uint32_t>(1, buf->Channels);
  double sum = 0;
  for (uint32_t i = 0; i < n; ++i) sum += double(buf->Frames[i]) * buf->Frames[i];
  // About -36 dBFS: above room noise, below normal speech.
  if (std::sqrt(sum / n) > 500.0) g_self_talk_until = SteadyMs() + 300;
}

// Our own level, measured from the microphone directly (WASAPI, shared mode)
// while voice is on: EOS leaves the mic closed while nobody else is in the
// room, and its outgoing audio is the only other place to see it. Marks us as
// talking above about -36 dBFS. Restarts when the chosen device changes.
std::atomic<bool> g_meter_running{false};
std::thread g_meter_thread;

void MeterLoop() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  std::string open_id = "\x01";  // nothing opened yet
  IMMDeviceEnumerator* enumerator = nullptr;
  CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                   reinterpret_cast<void**>(&enumerator));
  IAudioClient* client = nullptr;
  IAudioCaptureClient* capture = nullptr;
  bool is_float = false;
  uint32_t channels = 1;
  auto close = [&]() {
    if (client) client->Stop();
    if (capture) capture->Release();
    if (client) client->Release();
    capture = nullptr;
    client = nullptr;
  };
  while (g_meter_running) {
    std::string want;
    bool on = false;
    {
      std::lock_guard<std::recursive_mutex> lock(g_mutex);
      want = g_mic_id;
      on = g_voice_on;
    }
    if (!on) {
      close();
      open_id = "\x01";
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
      continue;
    }
    if (want != open_id && enumerator) {
      close();
      open_id = want;
      IMMDevice* device = nullptr;
      if (!want.empty()) {
        std::wstring wid(want.begin(), want.end());
        enumerator->GetDevice(wid.c_str(), &device);
      }
      if (!device) enumerator->GetDefaultAudioEndpoint(eCapture, eCommunications, &device);
      WAVEFORMATEX* fmt = nullptr;
      if (device && SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                               reinterpret_cast<void**>(&client))) &&
          SUCCEEDED(client->GetMixFormat(&fmt)) &&
          SUCCEEDED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 200 * 10000, 0, fmt, nullptr)) &&
          SUCCEEDED(client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&capture)))) {
        channels = std::max<uint32_t>(1, fmt->nChannels);
        is_float = fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                   (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && fmt->wBitsPerSample == 32);
        if (!is_float && fmt->wBitsPerSample != 16) {
          close();  // an unusual format: no meter
        } else {
          client->Start();
        }
      } else {
        close();
      }
      if (fmt) CoTaskMemFree(fmt);
      if (device) device->Release();
    }
    if (capture) {
      UINT32 packet = 0;
      while (SUCCEEDED(capture->GetNextPacketSize(&packet)) && packet) {
        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
        if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && frames) {
          const uint32_t n = frames * channels;
          double sum = 0;
          if (is_float) {
            const float* s = reinterpret_cast<const float*>(data);
            for (uint32_t i = 0; i < n; ++i) sum += double(s[i]) * s[i];
            if (std::sqrt(sum / n) > 0.0158) g_self_talk_until = SteadyMs() + 300;
          } else {
            const int16_t* s = reinterpret_cast<const int16_t*>(data);
            for (uint32_t i = 0; i < n; ++i) sum += double(s[i]) * s[i];
            if (std::sqrt(sum / n) > 500.0) g_self_talk_until = SteadyMs() + 300;
          }
        }
        capture->ReleaseBuffer(frames);
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  close();
  if (enumerator) enumerator->Release();
  CoUninitialize();
}

void RemoveVoiceNotifies() {
  if (g_voice_notify != EOS_INVALID_NOTIFICATIONID) {
    EOS_RTCAudio_RemoveNotifyParticipantUpdated(g_rtc_audio, g_voice_notify);
    g_voice_notify = EOS_INVALID_NOTIFICATIONID;
  }
  if (g_voice_send_notify != EOS_INVALID_NOTIFICATIONID) {
    EOS_RTCAudio_RemoveNotifyAudioBeforeSend(g_rtc_audio, g_voice_send_notify);
    g_voice_send_notify = EOS_INVALID_NOTIFICATIONID;
  }
  g_self_talk_until = 0;
}
bool g_voice_dirty = true;               // re-apply sending/receiving
size_t g_voice_peer_count = 0;
std::set<std::string> g_voice_mates;   // game mates when receiving was last applied
std::chrono::steady_clock::time_point g_voice_next{};

std::filesystem::path VoicePath() { return ExeDir() / "voice.txt"; }

void LoadVoice() {
  std::ifstream in(VoicePath());
  std::string line;
  while (std::getline(in, line)) {
    line = Trim(line);
    if (line == "on") g_voice_on = true;
    else if (line == "micmute") g_mic_muted = true;
    else if (line.rfind("mute ", 0) == 0) g_voice_muted.insert(line.substr(5));
    else if (line.rfind("mic ", 0) == 0) g_mic_id = line.substr(4);
  }
}

void SaveVoice() {
  std::ofstream out(VoicePath(), std::ios::trunc);
  out << (g_voice_on ? "on" : "off") << "\n";
  if (g_mic_muted) out << "micmute\n";
  for (const auto& p : g_voice_muted) out << "mute " << p << "\n";
  if (!g_mic_id.empty()) out << "mic " << g_mic_id << "\n";
}

void OnMicsQueried(const EOS_RTCAudio_OnQueryInputDevicesInformationCallbackInfo* info) {
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  g_mics_querying = false;
  if (info->ResultCode != EOS_EResult::EOS_Success) return;
  EOS_RTCAudio_GetInputDevicesCountOptions count_opt = {};
  count_opt.ApiVersion = EOS_RTCAUDIO_GETINPUTDEVICESCOUNT_API_LATEST;
  const uint32_t count = EOS_RTCAudio_GetInputDevicesCount(g_rtc_audio, &count_opt);
  std::vector<MicDevice> mics;
  for (uint32_t i = 0; i < count; ++i) {
    EOS_RTCAudio_CopyInputDeviceInformationByIndexOptions copy_opt = {};
    copy_opt.ApiVersion = EOS_RTCAUDIO_COPYINPUTDEVICEINFORMATIONBYINDEX_API_LATEST;
    copy_opt.DeviceIndex = i;
    EOS_RTCAudio_InputDeviceInformation* dev = nullptr;
    if (EOS_RTCAudio_CopyInputDeviceInformationByIndex(g_rtc_audio, &copy_opt, &dev) == EOS_EResult::EOS_Success && dev) {
      mics.push_back({dev->DeviceId ? dev->DeviceId : "", dev->DeviceName ? dev->DeviceName : "Microphone",
                      dev->bDefaultDevice == EOS_TRUE});
      EOS_RTCAudio_InputDeviceInformation_Release(dev);
    }
  }
  g_mics = std::move(mics);
}

// Device list queries and the chosen microphone, on the worker thread.
void ApplyMic() {
  if (!g_rtc_audio) return;
  if (g_mics_refresh && !g_mics_querying) {
    g_mics_refresh = false;
    g_mics_querying = true;
    EOS_RTCAudio_QueryInputDevicesInformationOptions q_opt = {};
    q_opt.ApiVersion = EOS_RTCAUDIO_QUERYINPUTDEVICESINFORMATION_API_LATEST;
    EOS_RTCAudio_QueryInputDevicesInformation(g_rtc_audio, &q_opt, nullptr, &OnMicsQueried);
  }
  if (g_local_puid && (g_mic_dirty || g_mic_user != g_local_puid_str)) {
    g_mic_dirty = false;
    g_mic_user = g_local_puid_str;
    EOS_RTCAudio_SetInputDeviceSettingsOptions opt = {};
    opt.ApiVersion = EOS_RTCAUDIO_SETINPUTDEVICESETTINGS_API_LATEST;
    opt.LocalUserId = g_local_puid;
    opt.RealDeviceId = g_mic_id.empty() ? nullptr : g_mic_id.c_str();
    opt.bPlatformAEC = EOS_TRUE;
    EOS_RTCAudio_SetInputDeviceSettings(g_rtc_audio, &opt, nullptr,
        [](const EOS_RTCAudio_OnSetInputDeviceSettingsCallbackInfo* info) {
          if (info->ResultCode != EOS_EResult::EOS_Success) {
            REXSYS_WARN("eos: microphone not set: {}", EOS_EResult_ToString(info->ResultCode));
          }
        });
  }
}

void OnVoiceParticipant(const EOS_RTCAudio_ParticipantUpdatedCallbackInfo* info) {
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  if (!info->ParticipantId) return;
  const std::string id = PuidToString(info->ParticipantId);
  if (info->bSpeaking) g_voice_talking.insert(id);
  else g_voice_talking.erase(id);
}

void SetReceiving(EOS_ProductUserId participant, bool enabled) {
  EOS_RTCAudio_UpdateReceivingOptions opt = {};
  opt.ApiVersion = EOS_RTCAUDIO_UPDATERECEIVING_API_LATEST;
  opt.LocalUserId = g_local_puid;
  opt.RoomName = g_voice_room.c_str();
  opt.ParticipantId = participant;
  opt.bAudioEnabled = enabled ? EOS_TRUE : EOS_FALSE;
  EOS_RTCAudio_UpdateReceiving(g_rtc_audio, &opt, nullptr,
                               [](const EOS_RTCAudio_UpdateReceivingCallbackInfo*) {});
}

// Follows the lobby's RTC room and applies the player's choices to it: mic
// sends and others are heard only with voice on, muted players never.
void ApplyVoice() {
  if (!g_rtc_audio || !g_local_puid) return;
  const auto now = std::chrono::steady_clock::now();
  if (now < g_voice_next) return;
  g_voice_next = now + std::chrono::milliseconds(500);
  if (g_state != State::kInLobby || g_lobby_id.empty()) {
    if (!g_voice_room.empty()) {
      RemoveVoiceNotifies();
      g_voice_room.clear();
      g_voice_room_lobby.clear();
      g_voice_talking.clear();
      g_voice_mates.clear();
    }
    return;
  }
  if (g_voice_room_lobby != g_lobby_id) {
    RemoveVoiceNotifies();
    g_voice_talking.clear();
    g_voice_room.clear();
    g_voice_room_lobby = g_lobby_id;
    EOS_Lobby_GetRTCRoomNameOptions name_opt = {};
    name_opt.ApiVersion = EOS_LOBBY_GETRTCROOMNAME_API_LATEST;
    name_opt.LobbyId = g_lobby_id.c_str();
    name_opt.LocalUserId = g_local_puid;
    char name[256] = {};
    uint32_t len = sizeof(name);
    if (EOS_Lobby_GetRTCRoomName(g_lobby, &name_opt, name, &len) == EOS_EResult::EOS_Success) {
      g_voice_room = name;
      EOS_RTCAudio_AddNotifyParticipantUpdatedOptions n_opt = {};
      n_opt.ApiVersion = EOS_RTCAUDIO_ADDNOTIFYPARTICIPANTUPDATED_API_LATEST;
      n_opt.LocalUserId = g_local_puid;
      n_opt.RoomName = g_voice_room.c_str();
      g_voice_notify = EOS_RTCAudio_AddNotifyParticipantUpdated(g_rtc_audio, &n_opt, nullptr, &OnVoiceParticipant);
      EOS_RTCAudio_AddNotifyAudioBeforeSendOptions s_opt = {};
      s_opt.ApiVersion = EOS_RTCAUDIO_ADDNOTIFYAUDIOBEFORESEND_API_LATEST;
      s_opt.LocalUserId = g_local_puid;
      s_opt.RoomName = g_voice_room.c_str();
      g_voice_send_notify = EOS_RTCAudio_AddNotifyAudioBeforeSend(g_rtc_audio, &s_opt, nullptr, &OnAudioBeforeSend);
      REXSYS_INFO("eos: voice room ready (voice {})", g_voice_on ? "on" : "off");
    } else {
      REXSYS_INFO("eos: this room has no voice channel (made without one: an older build, or voice not allowed by the EOS client policy)");
    }
    g_voice_dirty = true;
  }
  if (g_voice_room.empty()) return;
  if (g_peers.size() != g_voice_peer_count) {
    g_voice_peer_count = g_peers.size();
    g_voice_dirty = true;  // newcomers get the same treatment
  }
  // Heard: only players in your game lobby (the room itself is shared by
  // everyone online, so room-wide voice would be everyone at once).
  std::set<std::string> mates;
  for (const auto& p : g_peers) {
    if (g_lobby_mates.count(p.id_str)) mates.insert(p.id_str);
  }
  if (mates != g_voice_mates) {
    REXSYS_INFO("eos: voice: hearing {} player(s) in your game lobby", mates.size());
    g_voice_mates = std::move(mates);
    g_voice_dirty = true;
  }
  if (!g_voice_dirty) return;
  g_voice_dirty = false;
  EOS_RTCAudio_UpdateSendingOptions send_opt = {};
  send_opt.ApiVersion = EOS_RTCAUDIO_UPDATESENDING_API_LATEST;
  send_opt.LocalUserId = g_local_puid;
  send_opt.RoomName = g_voice_room.c_str();
  send_opt.AudioStatus = g_voice_on && !g_mic_muted ? EOS_ERTCAudioStatus::EOS_RTCAS_Enabled
                                                    : EOS_ERTCAudioStatus::EOS_RTCAS_Disabled;
  EOS_RTCAudio_UpdateSending(g_rtc_audio, &send_opt, nullptr, [](const EOS_RTCAudio_UpdateSendingCallbackInfo*) {});
  for (const auto& p : g_peers) {
    SetReceiving(p.id, g_voice_on && !g_voice_muted.count(p.id_str) && g_voice_mates.count(p.id_str));
  }
}

void WorkerLoop() {
  // All EOS calls happen on this plain host thread; the acre port saw the SDK
  // crash in Tick when the platform was created from a guest thread.
  {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (!InitPlatform()) {
      REXSYS_ERROR("eos: platform init failed; online play off for this run");
      g_state = State::kFailed;
      g_active = false;
      return;
    }
    REXSYS_INFO("eos: logging in ({})", RoomLabel());
    LoadVoice();
    g_meter_running = true;
    g_meter_thread = std::thread(MeterLoop);
    StartAuth();
  }
  while (g_running) {
    {
      std::lock_guard<std::recursive_mutex> lock(g_mutex);
      EOS_Platform_Tick(g_platform);
      DrainPackets();
      PumpBroadcasts();
      ApplyVoice();
      ApplyMic();
      // Only between lobby operations, so no callback is left half done.
      if (g_room_switch && !g_check_search &&
          (g_state == State::kInLobby || g_state == State::kFailed) && g_local_puid) {
        ApplyRoomSwitch();
      }
      if (g_state == State::kFailed && g_retry_stage != RetryStage::kNone &&
          std::chrono::steady_clock::now() >= g_next_retry) {
        const RetryStage stage = g_retry_stage;
        g_retry_stage = RetryStage::kNone;
        if (stage == RetryStage::kAuth) {
          StartAuth();
        } else {
          StartLobbyFlow();
        }
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  g_meter_running = false;
  if (g_meter_thread.joinable()) g_meter_thread.join();
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  EOS_Platform_Release(g_platform);
  g_platform = nullptr;
  EOS_Shutdown();
}

}  // namespace

void Startup(const std::string& display_name) {
  if (g_running || REXCVAR_GET(net_transport) != "eos") {
    return;
  }
  // No code: the shared public room. A code: a private room only players with
  // the same code reach.
  g_room = NormalizeRoom(REXCVAR_GET(net_room));
  auto dir = ExeDir();
  std::filesystem::path creds_path = REXCVAR_GET(eos_credentials);
  if (creds_path.is_relative()) {
    creds_path = dir / creds_path;
  }
  if (!ReadCredentials(creds_path, g_creds)) {
    REXSYS_WARN("eos: no complete credentials at {}; online play off", creds_path.string());
    return;
  }
  // The DLL is delay-loaded: make sure it is there before any EOS call.
  if (!LoadLibraryW((dir / kDllName).c_str())) {
    REXSYS_WARN("eos: EOSSDK-Win64-Shipping.dll not found next to the exe; online play off");
    return;
  }
  // Connect display names are at most 32 characters.
  g_display_name = display_name.substr(0, 32);
  if (g_display_name.empty()) {
    g_display_name = "Player";
  }
  g_active = true;
  g_running = true;
  g_thread = std::thread(WorkerLoop);
}

void Shutdown() {
  if (!g_running) {
    return;
  }
  g_running = false;
  if (g_thread.joinable()) {
    g_thread.join();
  }
  g_active = false;
}

bool Active() {
  return g_active;
}

bool VoiceAvailable() {
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  return g_rtc_audio && !g_voice_room.empty();
}

bool VoiceEnabled() {
  return g_voice_on;
}

void SetVoiceEnabled(bool enabled) {
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  g_voice_on = enabled;
  g_voice_dirty = true;
  g_voice_next = {};
  SaveVoice();
}

void SetVoiceLobby(const std::vector<std::string>& puids) {
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  g_lobby_mates = std::set<std::string>(puids.begin(), puids.end());
}

bool VoiceMicMuted() {
  return g_mic_muted;
}

void SetVoiceMicMuted(bool muted) {
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  g_mic_muted = muted;
  g_voice_dirty = true;
  g_voice_next = {};
  SaveVoice();
}

bool VoiceMuted(const std::string& puid) {
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  return g_voice_muted.count(puid) != 0;
}

void SetVoiceMuted(const std::string& puid, bool muted) {
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  if (muted) g_voice_muted.insert(puid);
  else g_voice_muted.erase(puid);
  g_voice_dirty = true;
  g_voice_next = {};
  SaveVoice();
}

bool VoiceTalking(const std::string& puid) {
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  return g_voice_talking.count(puid) != 0 && g_voice_mates.count(puid) != 0;
}

std::vector<std::string> VoiceTalkers() {
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  std::vector<std::string> out;
  for (const auto& id : g_voice_talking) {
    if (g_voice_mates.count(id)) out.push_back(id);
  }
  if (g_voice_on && !g_mic_muted && SteadyMs() < g_self_talk_until && !g_local_puid_str.empty() &&
      !g_voice_talking.count(g_local_puid_str)) {
    out.push_back(g_local_puid_str);
  }
  return out;
}

std::vector<MicDevice> VoiceMics() {
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  g_mics_refresh = true;
  return g_mics;
}

std::string VoiceMic() {
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  return g_mic_id;
}

void SetVoiceMic(const std::string& id) {
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  g_mic_id = id;
  g_mic_dirty = true;
  SaveVoice();
}

bool Ready() {
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  return g_state == State::kInLobby && g_self_vip != 0;
}

uint32_t SelfVirtualIp() {
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  return g_self_vip ? (kVirtualNetBase | g_self_vip) : 0;
}

int PeerCount() {
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  int n = 0;
  for (const auto& p : g_peers) {
    if (p.vip) {
      ++n;
    }
  }
  return n;
}

bool IsPeer(uint32_t vip) {
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  for (const auto& p : g_peers) {
    if (p.vip && (kVirtualNetBase | p.vip) == vip) {
      return true;
    }
  }
  return false;
}

void BindPort(uint16_t guest_port) {
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  g_bound_ports.insert(guest_port);
  g_rx.try_emplace(guest_port);
}

void UnbindPort(uint16_t guest_port) {
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  g_bound_ports.erase(guest_port);
  g_rx.erase(guest_port);
}

int SendTo(uint32_t dst_ip, uint16_t dst_port, uint16_t src_port, const uint8_t* data,
           uint32_t len, int* out_wsa_err) {
  *out_wsa_err = 0;
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  if (g_state != State::kInLobby || g_self_vip == 0 || !g_p2p) {
    *out_wsa_err = kWsaNetDown;
    return -1;
  }
  if (len + kHeaderBytes > EOS_P2P_MAX_PACKET_SIZE) {
    static std::atomic<int> oversize_log{0};
    if (!RateLimited(oversize_log)) {
      REXSYS_ERROR("eos: datagram too large for EOS P2P ({} > {} bytes)", len,
                   EOS_P2P_MAX_PACKET_SIZE - kHeaderBytes);
    }
    *out_wsa_err = kWsaMsgSize;
    return -1;
  }

  uint8_t buf[EOS_P2P_MAX_PACKET_SIZE];
  buf[0] = static_cast<uint8_t>(src_port >> 8);
  buf[1] = static_cast<uint8_t>(src_port & 0xFF);
  buf[2] = static_cast<uint8_t>(dst_port >> 8);
  buf[3] = static_cast<uint8_t>(dst_port & 0xFF);
  std::memcpy(buf + kHeaderBytes, data, len);

  EOS_P2P_SocketId sid = {};
  sid.ApiVersion = EOS_P2P_SOCKETID_API_LATEST;
  std::snprintf(sid.SocketName, sizeof(sid.SocketName), "%s", kSocketName);

  EOS_P2P_SendPacketOptions opt = {};
  opt.ApiVersion = EOS_P2P_SENDPACKET_API_LATEST;
  opt.LocalUserId = g_local_puid;
  opt.SocketId = &sid;
  opt.Channel = 0;
  opt.DataLengthBytes = len + kHeaderBytes;
  opt.Data = buf;
  // Queue while the NAT punch is in progress, or the first beacons vanish.
  opt.bAllowDelayedDelivery = EOS_TRUE;
  // Titles do their own sequencing/retransmit; reliability here doubles it.
  opt.Reliability = EOS_EPacketReliability::EOS_PR_UnreliableUnordered;
  opt.bDisableAutoAcceptConnection = EOS_FALSE;

  const bool broadcast = dst_ip == 0xFFFFFFFFu || (dst_ip & 0xFFu) == 0xFFu;
  if (broadcast) {
    for (const auto& p : g_peers) {
      if (!p.vip) {
        continue;
      }
      opt.RemoteUserId = p.id;
      const EOS_EResult r = EOS_P2P_SendPacket(g_p2p, &opt);
      if (r != EOS_EResult::EOS_Success) {
        static std::atomic<int> bcast_err_log{0};
        if (!RateLimited(bcast_err_log)) {
          REXSYS_WARN("eos: broadcast to {}: {}", p.id_str, EOS_EResult_ToString(r));
        }
      }
    }
    return static_cast<int>(len);
  }

  for (const auto& p : g_peers) {
    if (p.vip && (kVirtualNetBase | p.vip) == dst_ip) {
      opt.RemoteUserId = p.id;
      const EOS_EResult r = EOS_P2P_SendPacket(g_p2p, &opt);
      if (r != EOS_EResult::EOS_Success) {
        static std::atomic<int> send_err_log{0};
        if (!RateLimited(send_err_log)) {
          REXSYS_WARN("eos: send to {}: {}", p.id_str, EOS_EResult_ToString(r));
        }
        *out_wsa_err = kWsaNetDown;
        return -1;
      }
      g_traffic.insert(p.id_str);
      return static_cast<int>(len);
    }
  }
  static std::atomic<int> unreach_log{0};
  if (!RateLimited(unreach_log)) {
    REXSYS_WARN("eos: sendto {}: nobody in the room has that address", FormatIp(dst_ip));
  }
  *out_wsa_err = kWsaHostUnreach;
  return -1;
}

int RecvFrom(uint16_t guest_port, uint8_t* buf, uint32_t buf_len, uint32_t* out_src_ip,
             uint16_t* out_src_port, int* out_wsa_err) {
  *out_wsa_err = 0;
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  auto it = g_rx.find(guest_port);
  if (it == g_rx.end() || it->second.empty()) {
    *out_wsa_err = kWsaWouldBlock;
    return -1;
  }
  Datagram d = std::move(it->second.front());
  it->second.pop_front();
  const uint32_t n =
      d.payload.size() < buf_len ? static_cast<uint32_t>(d.payload.size()) : buf_len;
  std::memcpy(buf, d.payload.data(), n);
  *out_src_ip = d.src_ip;
  *out_src_port = d.src_port;
  return static_cast<int>(n);
}

bool HasPending(uint16_t guest_port) {
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  auto it = g_rx.find(guest_port);
  return it != g_rx.end() && !it->second.empty();
}

void SetBroadcast(uint8_t channel, std::vector<uint8_t> blob) {
  if (blob.size() > kMaxAnnouncementBytes) {
    REXSYS_WARN("eos: channel {} broadcast too large ({} bytes), not sent", channel, blob.size());
    blob.clear();
  }
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  auto* b = BroadcastFor(channel);
  if (b && blob != b->blob) {
    b->blob = std::move(blob);
    b->dirty = true;
  }
}

std::vector<PeerBlob> Broadcasts(uint8_t channel) {
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  std::vector<PeerBlob> out;
  if (auto* b = BroadcastFor(channel)) {
    for (const auto& [puid, r] : b->received) {
      out.push_back({r.vip, puid, r.blob});
    }
  }
  return out;
}

void SetAnnouncement(std::vector<uint8_t> blob) {
  SetBroadcast(kSessionChannel, std::move(blob));
}

std::vector<Announcement> Announcements() {
  std::vector<Announcement> out;
  for (auto& b : Broadcasts(kSessionChannel)) {
    out.push_back({b.vip, std::move(b.blob)});
  }
  return out;
}

void SendDirect(const std::string& puid, std::vector<uint8_t> blob) {
  if (blob.empty() || blob.size() > kMaxAnnouncementBytes) {
    return;
  }
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  g_outbox.push_back({puid, std::move(blob)});
}

std::vector<PeerBlob> TakeDirectMessages() {
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  std::vector<PeerBlob> out(g_inbox.begin(), g_inbox.end());
  g_inbox.clear();
  return out;
}

std::vector<std::string> TakeTrafficPeers() {
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  std::vector<std::string> out(g_traffic.begin(), g_traffic.end());
  g_traffic.clear();
  return out;
}

std::string LocalUserId() {
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  return g_local_puid_str;
}

std::string RoomCode() {
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  const std::string& room = g_room_switch ? g_pending_room : g_room;
  return room == kPublicRoom ? std::string() : room;
}

void SwitchRoom(const std::string& code, bool create_new,
                std::function<void(const std::string&)> note) {
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  if (!g_active) {
    return;
  }
  g_pending_room = NormalizeRoom(code);
  g_switch_note = std::move(note);
  if (g_pending_room == g_room) {
    g_room_switch = false;
    if (create_new) {
      Note("You're already in room " + g_room + ".");
    }
    return;
  }
  g_pending_create = create_new && g_pending_room != kPublicRoom;
  g_room_switch = true;
}

}  // namespace rex::system::xam::eos

#else  // !REX_HAS_EOS

namespace rex::system::xam::eos {

void Startup(const std::string&) {}
void Shutdown() {}
bool Active() {
  return false;
}
bool VoiceAvailable() {
  return false;
}
bool VoiceEnabled() {
  return false;
}
void SetVoiceEnabled(bool) {}
void SetVoiceLobby(const std::vector<std::string>&) {}
bool VoiceMicMuted() {
  return false;
}
void SetVoiceMicMuted(bool) {}
bool VoiceMuted(const std::string&) {
  return false;
}
void SetVoiceMuted(const std::string&, bool) {}
bool VoiceTalking(const std::string&) {
  return false;
}
std::vector<std::string> VoiceTalkers() {
  return {};
}
std::vector<MicDevice> VoiceMics() {
  return {};
}
std::string VoiceMic() {
  return {};
}
void SetVoiceMic(const std::string&) {}
bool Ready() {
  return false;
}
uint32_t SelfVirtualIp() {
  return 0;
}
int PeerCount() {
  return 0;
}
bool IsPeer(uint32_t) {
  return false;
}
void BindPort(uint16_t) {}
void UnbindPort(uint16_t) {}
int SendTo(uint32_t, uint16_t, uint16_t, const uint8_t*, uint32_t, int* out_wsa_err) {
  *out_wsa_err = 10050;  // WSAENETDOWN
  return -1;
}
int RecvFrom(uint16_t, uint8_t*, uint32_t, uint32_t*, uint16_t*, int* out_wsa_err) {
  *out_wsa_err = 10035;  // WSAEWOULDBLOCK
  return -1;
}
bool HasPending(uint16_t) {
  return false;
}
void SetAnnouncement(std::vector<uint8_t>) {}
std::vector<Announcement> Announcements() {
  return {};
}
void SetBroadcast(uint8_t, std::vector<uint8_t>) {}
std::vector<PeerBlob> Broadcasts(uint8_t) {
  return {};
}
void SendDirect(const std::string&, std::vector<uint8_t>) {}
std::vector<PeerBlob> TakeDirectMessages() {
  return {};
}
std::vector<std::string> TakeTrafficPeers() {
  return {};
}
std::string LocalUserId() {
  return {};
}
std::string RoomCode() {
  return {};
}
void SwitchRoom(const std::string&, bool, std::function<void(const std::string&)>) {}

}  // namespace rex::system::xam::eos

#endif
