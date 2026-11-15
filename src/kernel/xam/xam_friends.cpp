/**
 * @file        kernel/xam/xam_friends.cpp
 * @brief       Friends, recent players, presence and game invites over the
 *              EOS room (no Epic accounts).
 *
 * Identity is the EOS ProductUserId of each PC (an anonymous device login);
 * each one also gets a stable "online" XUID derived from it for the title's
 * friend lists and invites. The player's own profile XUID is left alone so
 * saves keep matching.
 *
 *   presence   every room member broadcasts (EOS channel 2) its gamertag and
 *              the game session it is in, so friends show online, playing and
 *              joinable;
 *   friends    kept with recent players (people you exchanged game traffic
 *              with) in friends.txt beside the executable; added/removed
 *              from the players / gamer card UIs;
 *   invites    XamShowGameInviteUI sends the session (XSESSION_INFO) to the
 *              chosen player; the receiver gets an Accept prompt and, on
 *              accept, XN_LIVE_INVITE_ACCEPTED plus the invite through
 *              XInviteGetAcceptedInfo (XLiveBase 0x58023).
 *
 * Structures follow xenia-canary's netplay branch (X_ONLINE_FRIEND 0xC4,
 * X_INVITE_INFO, X_ARGUMENT_LIST entries of 16 bytes with object_ptr at +12).
 */

#include <deque>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <tuple>
#include <sstream>
#include <thread>

#include <rex/kernel/xam/private.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/string.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/eos_service.h>
#include <rex/system/xenumerator.h>

#if REX_PLATFORM_WIN32
#include <windows.h>
#endif

namespace rex::kernel::xam {
using namespace rex::system;
namespace eos = rex::system::xam::eos;

namespace {

constexpr uint32_t kFriendSize = 0xC4;  // X_ONLINE_FRIEND
constexpr uint32_t kSessionInfoSize = 60;
constexpr uint32_t kFriendOnline = 0x01, kFriendPlaying = 0x02, kFriendJoinable = 0x10;
constexpr uint32_t kNotifyInviteAccepted = 0x02000002;  // XN_LIVE_INVITE_ACCEPTED
constexpr size_t kMaxRecent = 50;
constexpr uint8_t kPresenceMagic[3] = {'X', 'P', 1};
constexpr uint8_t kInviteMagic[3] = {'X', 'I', 1};
// Room chat: magic, sender's gamertag (16 bytes), UTF-8 text.
constexpr uint8_t kChatMagic[3] = {'X', 'C', 1};
constexpr size_t kChatMaxText = 200;
constexpr size_t kChatKeep = 50;

struct Player {
  std::string puid;
  std::string gamertag;
  bool is_friend = false;
  int64_t last_seen = 0;  // unix time of the last game together (recent)
};

struct Presence {
  std::string gamertag;
  bool in_session = false;
  uint8_t info[kSessionInfoSize] = {};
};

struct AcceptedInvite {
  std::string puid;
  uint8_t info[kSessionInfoSize] = {};
};

std::recursive_mutex g_mutex;
bool g_loaded = false;
std::vector<Player> g_players;  // friends and recent players
std::optional<AcceptedInvite> g_accepted;
// Invites received and not yet answered, newest last (shown in the Guide).
struct PendingInvite {
  std::string puid, gamertag;
  std::vector<uint8_t> info;
  int64_t time = 0;
};
std::vector<PendingInvite> g_invites;
constexpr int64_t kInviteLifetime = 10 * 60;  // seconds

std::filesystem::path FriendsFile() {
#if REX_PLATFORM_WIN32
  wchar_t buf[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, buf, MAX_PATH);
  return std::filesystem::path(buf).parent_path() / "friends.txt";
#else
  return "friends.txt";
#endif
}

// Stable online XUID for a ProductUserId (0x0009... like Live XUIDs).
uint64_t XuidFor(const std::string& puid) {
  uint64_t h = 0xCBF29CE484222325ull;
  for (char c : puid) {
    h = (h ^ uint8_t(c)) * 0x100000001B3ull;
  }
  return 0x0009000000000000ull | (h & 0x0000FFFFFFFFFFFFull);
}

void Load() {
  if (g_loaded) return;
  g_loaded = true;
  std::ifstream in(FriendsFile());
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream fields(line);
    std::string kind, puid, gamertag, seen;
    if (!std::getline(fields, kind, '\t') || !std::getline(fields, puid, '\t') ||
        !std::getline(fields, gamertag, '\t')) {
      continue;
    }
    std::getline(fields, seen, '\t');
    Player p;
    p.puid = puid;
    p.gamertag = gamertag;
    p.is_friend = kind == "F";
    p.last_seen = seen.empty() ? 0 : std::atoll(seen.c_str());
    g_players.push_back(std::move(p));
  }
}

void Save() {
  std::ofstream out(FriendsFile(), std::ios::trunc);
  out << "# PGR4-Recomp friends (F) and recent players (R): kind, EOS id, gamertag, last seen\n";
  for (const auto& p : g_players) {
    out << (p.is_friend ? "F" : "R") << '\t' << p.puid << '\t' << p.gamertag << '\t'
        << p.last_seen << '\n';
  }
}

Player* Find(const std::string& puid) {
  for (auto& p : g_players) {
    if (p.puid == puid) return &p;
  }
  return nullptr;
}

Player* FindByXuid(uint64_t xuid) {
  for (auto& p : g_players) {
    if (XuidFor(p.puid) == xuid) return &p;
  }
  return nullptr;
}

// Room members' presence by ProductUserId.
std::map<std::string, Presence> Presences() {
  std::map<std::string, Presence> out;
  for (const auto& b : eos::Broadcasts(eos::kPresenceChannel)) {
    const auto& d = b.blob;
    if (d.size() < 3 + 16 + 4 || std::memcmp(d.data(), kPresenceMagic, 3) != 0) continue;
    Presence p;
    p.gamertag.assign(reinterpret_cast<const char*>(d.data() + 3), 16);
    p.gamertag = p.gamertag.c_str();  // trim padding
    uint32_t flags;
    std::memcpy(&flags, d.data() + 19, 4);
    if ((flags & 1) && d.size() >= 23 + kSessionInfoSize) {
      p.in_session = true;
      std::memcpy(p.info, d.data() + 23, kSessionInfoSize);
    }
    out[b.puid] = p;
  }
  return out;
}

std::string LocalGamertag() {
  auto* profile = REX_KERNEL_STATE()->user_profile();
  return profile ? profile->name() : "Player";
}

void PublishPresence() {
  std::vector<uint8_t> blob(kPresenceMagic, kPresenceMagic + 3);
  char tag[16] = {};
  std::string name = LocalGamertag();
  std::memcpy(tag, name.data(), std::min<size_t>(name.size(), 15));
  blob.insert(blob.end(), tag, tag + 16);
  uint8_t info[kSessionInfoSize];
  uint32_t flags = xeXSessionCurrentInfo(info) ? 1 : 0;
  blob.insert(blob.end(), reinterpret_cast<uint8_t*>(&flags), reinterpret_cast<uint8_t*>(&flags) + 4);
  if (flags & 1) blob.insert(blob.end(), info, info + kSessionInfoSize);
  eos::SetBroadcast(eos::kPresenceChannel, std::move(blob));
}

// PGR4's notification handler (sub_828A2150) answers XN_LIVE_INVITE_ACCEPTED
// for a Live-signed-in user by calling XInviteGetAcceptedInfo and joining the
// session in it; accepting an invite and "join their game" both use that.
void AcceptSession(const std::string& puid, const uint8_t* info) {
  {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    AcceptedInvite a;
    a.puid = puid;
    std::memcpy(a.info, info, kSessionInfoSize);
    g_accepted = a;
  }
  REX_KERNEL_STATE()->BroadcastNotification(kNotifyInviteAccepted, 0);
}

std::vector<uint8_t> InviteBlob(const uint8_t* info) {
  std::vector<uint8_t> blob(kInviteMagic, kInviteMagic + 3);
  char tag[16] = {};
  std::string name = LocalGamertag();
  std::memcpy(tag, name.data(), std::min<size_t>(name.size(), 15));
  blob.insert(blob.end(), tag, tag + 16);
  blob.insert(blob.end(), info, info + kSessionInfoSize);
  return blob;
}

// Like the console: the invite waits in the Guide (Game Invites) until it is
// accepted or declined; a newer one from the same player replaces it.
void AddPendingInvite(std::string from_puid, std::string from_tag, std::vector<uint8_t> info) {
  g_invites.erase(std::remove_if(g_invites.begin(), g_invites.end(),
                                 [&](const PendingInvite& i) { return i.puid == from_puid; }),
                  g_invites.end());
  g_invites.push_back({std::move(from_puid), std::move(from_tag), std::move(info),
                       int64_t(std::time(nullptr))});
}

std::deque<GuideChatLine> g_chat;
int g_chat_unread = 0;

// Printable text only, at most kChatMaxText bytes (not splitting a UTF-8
// sequence).
std::string CleanChat(std::string s) {
  std::string out;
  for (unsigned char ch : s) {
    if (ch >= 0x20 && ch != 0x7F) out += char(ch);
    else if (ch == '\n' || ch == '\t') out += ' ';
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  while (!out.empty() && out.front() == ' ') out.erase(out.begin());
  if (out.size() > kChatMaxText) {
    size_t n = kChatMaxText;
    while (n > 0 && (static_cast<unsigned char>(out[n]) & 0xC0) == 0x80) --n;
    out.resize(n);
  }
  return out;
}

void AddChat(GuideChatLine line) {
  g_chat.push_back(std::move(line));
  while (g_chat.size() > kChatKeep) g_chat.pop_front();
}

void HandleDirectMessages() {
  for (auto& m : eos::TakeDirectMessages()) {
    const auto& d = m.blob;
    if (d.size() >= 3 + 16 + kSessionInfoSize && std::memcmp(d.data(), kInviteMagic, 3) == 0) {
      std::string tag(reinterpret_cast<const char*>(d.data() + 3), 16);
      tag = tag.c_str();
      xeXamShowToast("Game invite from " + tag + " - open the Guide to accept");
      AddPendingInvite(m.puid, tag,
                       std::vector<uint8_t>(d.begin() + 19, d.begin() + 19 + kSessionInfoSize));
    } else if (d.size() > 3 + 16 && std::memcmp(d.data(), kChatMagic, 3) == 0) {
      std::string tag(reinterpret_cast<const char*>(d.data() + 3), 16);
      tag = tag.c_str();
      std::string text = CleanChat(std::string(d.begin() + 19, d.end()));
      if (tag.empty() || text.empty()) continue;
      AddChat({tag, text, int64_t(std::time(nullptr)), false});
      ++g_chat_unread;
      std::string shown = text.size() > 60 ? text.substr(0, 57) + "..." : text;
      xeXamShowToast(tag + ": " + shown);
    }
  }
}

// People we exchanged game traffic with become recent players.
void RecordRecentPlayers(const std::map<std::string, Presence>& presences) {
  auto peers = eos::TakeTrafficPeers();
  if (peers.empty()) return;
  const int64_t now = std::time(nullptr);
  bool changed = false;
  for (const auto& puid : peers) {
    Player* p = Find(puid);
    if (!p) {
      g_players.push_back({puid, "", false, 0});
      p = &g_players.back();
    }
    auto it = presences.find(puid);
    if (it != presences.end() && !it->second.gamertag.empty()) p->gamertag = it->second.gamertag;
    if (now - p->last_seen > 60) changed = true;
    p->last_seen = now;
  }
  // Keep friends and the most recent others.
  std::stable_sort(g_players.begin(), g_players.end(),
                   [](const Player& a, const Player& b) { return a.last_seen > b.last_seen; });
  size_t recent = 0;
  g_players.erase(std::remove_if(g_players.begin(), g_players.end(),
                                 [&recent](const Player& p) {
                                   return !p.is_friend && ++recent > kMaxRecent;
                                 }),
                  g_players.end());
  if (changed) Save();
}

void WriteFriend(uint8_t* f, const Player& p, const Presence* presence) {
  std::memset(f, 0, kFriendSize);
  memory::store_and_swap<uint64_t>(f + 0, XuidFor(p.puid));
  std::string tag = presence && !presence->gamertag.empty() ? presence->gamertag : p.gamertag;
  std::memcpy(f + 8, tag.data(), std::min<size_t>(tag.size(), 15));
  uint32_t state = 0;
  std::u16string rich = u"Offline";
  if (presence) {
    state = kFriendOnline | kFriendPlaying;
    rich = u"Online";
    if (presence->in_session) {
      state |= kFriendJoinable;
      std::memcpy(f + 28, presence->info, 8);  // session XNKID
      rich = u"In a game";
    }
  }
  memory::store_and_swap<uint32_t>(f + 24, state);
  memory::store_and_swap<uint32_t>(f + 36, REX_KERNEL_STATE()->title_id());
  memory::store_and_swap<uint32_t>(f + 64, uint32_t(rich.size() + 1));
  for (size_t i = 0; i < rich.size() && i < 63; ++i) {
    memory::store_and_swap<uint16_t>(f + 68 + i * 2, uint16_t(rich[i]));
  }
}

uint32_t ArgValue(uint32_t arg_list, uint32_t index) {
  uint32_t obj = memory::load_and_swap<uint32_t>(
      REX_KERNEL_MEMORY()->TranslateVirtual(arg_list + index * 16 + 12));
  return obj ? memory::load_and_swap<uint32_t>(REX_KERNEL_MEMORY()->TranslateVirtual(obj)) : 0;
}

uint32_t ArgAddress(uint32_t arg_list, uint32_t index) {
  return memory::load_and_swap<uint32_t>(
      REX_KERNEL_MEMORY()->TranslateVirtual(arg_list + index * 16 + 12));
}

}  // namespace

std::string xeXFriendsGamertag(uint64_t xuid) {
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  Load();
  const Player* p = FindByXuid(xuid);
  return p ? p->gamertag : std::string();
}

void xeXFriendsTick() {
  if (!eos::Active()) return;
  static std::atomic<int64_t> last_ms{0};
  int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count();
  int64_t last = last_ms.load();
  if (now - last < 500 || !last_ms.compare_exchange_strong(last, now)) return;

  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  Load();
  PublishPresence();
  HandleDirectMessages();
  auto presences = Presences();
  RecordRecentPlayers(presences);
  // Voice: the players in our game lobby share our session ID (XNKID).
  {
    std::vector<std::string> lobby;
    uint8_t mine[kSessionInfoSize];
    if (xeXSessionCurrentInfo(mine)) {
      for (const auto& [puid, pr] : presences) {
        if (pr.in_session && std::memcmp(pr.info, mine, 8) == 0) lobby.push_back(puid);
      }
    }
    eos::SetVoiceLobby(lobby);
  }
  // Players who renamed themselves: keep friends.txt current.
  bool renamed = false;
  for (auto& p : g_players) {
    auto it = presences.find(p.puid);
    if (it != presences.end() && !it->second.gamertag.empty() && it->second.gamertag != p.gamertag) {
      p.gamertag = it->second.gamertag;
      renamed = true;
    }
  }
  if (renamed) Save();

  // Friends coming online / going offline (seen in the room or not).
  static std::set<std::string> online_friends;
  static bool first = true;
  std::set<std::string> now_online;
  for (const auto& p : g_players) {
    if (p.is_friend && presences.count(p.puid)) now_online.insert(p.puid);
  }
  if (!first) {
    for (const auto& puid : now_online) {
      if (!online_friends.count(puid)) xeXamShowToast(Find(puid)->gamertag + " is online");
    }
    for (const auto& puid : online_friends) {
      if (!now_online.count(puid)) {
        if (Player* p = Find(puid)) xeXamShowToast(p->gamertag + " went offline");
      }
    }
  }
  first = false;
  online_friends = std::move(now_online);
}

X_HRESULT xeXFriendsCreateEnumerator(uint32_t arg_list) {
  if (!arg_list) return X_E_INVALIDARG;
  const uint32_t user_index = ArgValue(arg_list, 0);
  const uint32_t start = ArgValue(arg_list, 1);
  const uint32_t amount = ArgValue(arg_list, 2);
  const uint32_t buffer_size_addr = ArgAddress(arg_list, 3);
  const uint32_t handle_addr = ArgAddress(arg_list, 4);
  auto* mem = REX_KERNEL_MEMORY();
  if (buffer_size_addr) memory::store_and_swap<uint32_t>(mem->TranslateVirtual(buffer_size_addr), 0);
  if (handle_addr) {
    memory::store_and_swap<uint32_t>(mem->TranslateVirtual(handle_addr), 0xFFFFFFFF);
  }
  if (!buffer_size_addr || !handle_addr || amount == 0 || amount > 100) return X_E_INVALIDARG;
  if (!xeXamSignedInProfile(user_index)) return X_E_NO_SUCH_USER;

  auto e = object_ref<XStaticUntypedEnumerator>(
      new XStaticUntypedEnumerator(REX_KERNEL_STATE(), amount, kFriendSize));
  auto result = e->Initialize(0xFFFFFFFF, 0xFC, 0x58021, 0x58022, 0);
  if (XFAILED(result)) return result;

  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  Load();
  auto presences = Presences();
  uint32_t index = 0, added = 0;
  for (const auto& p : g_players) {
    if (!p.is_friend) continue;
    if (index++ < start) continue;
    if (added >= amount) break;
    auto it = presences.find(p.puid);
    WriteFriend(e->AppendItem(), p, it == presences.end() ? nullptr : &it->second);
    ++added;
  }
  memory::store_and_swap<uint32_t>(mem->TranslateVirtual(buffer_size_addr),
                                   uint32_t(e->items_per_enumerate() * e->item_size()));
  memory::store_and_swap<uint32_t>(mem->TranslateVirtual(handle_addr), e->handle());
  REXKRNL_INFO("Friends enumerator: {} friend(s)", added);
  return X_E_SUCCESS;
}

X_HRESULT xeXInviteGetAcceptedInfo(uint32_t arg_list) {
  if (!arg_list) return X_E_INVALIDARG;
  const uint32_t info_addr = ArgAddress(arg_list, 1);
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  REXKRNL_INFO("XInviteGetAcceptedInfo: {}", g_accepted ? "returning the accepted invite"
                                                        : "no accepted invite");
  if (!g_accepted || !info_addr) return X_E_FAIL;
  uint8_t* out = REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(info_addr);
  // X_INVITE_INFO: invitee XUID, inviter XUID, title ID, XSESSION_INFO,
  // from-game-invite flag.
  std::memset(out, 0, 84);
  memory::store_and_swap<uint64_t>(out + 0, REX_KERNEL_STATE()->user_profile()->xuid());
  memory::store_and_swap<uint64_t>(out + 8, XuidFor(g_accepted->puid));
  memory::store_and_swap<uint32_t>(out + 16, REX_KERNEL_STATE()->title_id());
  std::memcpy(out + 20, g_accepted->info, kSessionInfoSize);
  memory::store_and_swap<uint32_t>(out + 80, 1);
  g_accepted.reset();
  return X_E_SUCCESS;
}

void xeXInviteShowUI(uint32_t user_index, const uint64_t* recipients, uint32_t count) {
  if (!eos::Active()) {
    xeXamShowListDialog("Invite", "Online play is off (net_transport is not eos).", {});
    return;
  }
  uint8_t info[kSessionInfoSize];
  if (!xeXSessionCurrentInfo(info)) {
    xeXamShowListDialog("Invite", "You are not in a game to invite people to.", {});
    return;
  }
  std::vector<std::pair<std::string, std::string>> targets;  // puid, gamertag
  {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    Load();
    auto presences = Presences();
    if (count) {
      // The title picked people (from its friends list): XUIDs are ours.
      for (uint32_t i = 0; i < count; ++i) {
        if (Player* p = FindByXuid(recipients[i])) targets.emplace_back(p->puid, p->gamertag);
      }
    } else {
      // Pick from friends online, then everyone else in the room.
      std::vector<std::pair<std::string, std::string>> options;
      for (const auto& [puid, pr] : presences) {
        Player* p = Find(puid);
        options.emplace_back(puid, pr.gamertag + (p && p->is_friend ? "  (friend)" : ""));
      }
      std::stable_sort(options.begin(), options.end(), [](const auto& a, const auto& b) {
        return a.second.find("(friend)") != std::string::npos &&
               b.second.find("(friend)") == std::string::npos;
      });
      std::vector<std::string> names;
      for (const auto& o : options) names.push_back(o.second);
      if (names.empty()) {
        // Released below; the dialog must not hold the lock.
      } else {
        targets = options;
      }
    }
  }
  if (!count) {
    std::vector<std::string> names;
    for (const auto& t : targets) names.push_back(t.second);
    if (names.empty()) {
      xeXamShowListDialog("Invite", "Nobody else is in your online room right now.", {});
      return;
    }
    int chosen = xeXamShowListDialog("Invite to game", "Choose who to invite:", names);
    if (chosen < 0) return;
    targets = {targets[chosen]};
  }
  auto blob = InviteBlob(info);
  for (const auto& [puid, gamertag] : targets) {
    eos::SendDirect(puid, blob);
    xeXamShowToast("Invite sent to " + gamertag);
  }
}

bool xeXamEnterKeyDown() {
#if REX_PLATFORM_WIN32
  DWORD pid = 0;
  GetWindowThreadProcessId(GetForegroundWindow(), &pid);
  if (pid != GetCurrentProcessId() || (GetAsyncKeyState(VK_MENU) & 0x8000)) {
    return false;
  }
  return (GetAsyncKeyState(VK_RETURN) & 0x8000) != 0;
#else
  return false;
#endif
}

void xeXFriendsShowPlayers() {
  xeXamShowGuide(kGuideBladeRecent);
}

GuideSnapshot xeXFriendsSnapshot() {
  GuideSnapshot out;
  out.online_enabled = eos::Active();
  if (!out.online_enabled) return out;
  out.connected = eos::Ready();
  out.room = eos::RoomCode();
  uint8_t info[kSessionInfoSize];
  out.in_game = xeXSessionCurrentInfo(info);

  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  Load();
  auto presences = Presences();
  auto make = [&](const std::string& puid, const Player* p) {
    GuidePlayer g;
    g.puid = puid;
    auto it = presences.find(puid);
    g.online = it != presences.end();
    g.in_game = g.online && it->second.in_session;
    // The name they broadcast now wins over the one saved when we met.
    g.gamertag = g.online && !it->second.gamertag.empty() ? it->second.gamertag
                 : p && !p->gamertag.empty()               ? p->gamertag
                                                           : puid.substr(0, 8);
    g.is_friend = p && p->is_friend;
    g.last_seen = p ? p->last_seen : 0;
    return g;
  };
  for (const auto& p : g_players) {
    if (p.is_friend) out.friends.push_back(make(p.puid, &p));
    if (p.last_seen) out.recent.push_back(make(p.puid, &p));
  }
  // Friends online first, playing before idle.
  std::stable_sort(out.friends.begin(), out.friends.end(), [](const auto& a, const auto& b) {
    return std::make_tuple(!a.online, !a.in_game) < std::make_tuple(!b.online, !b.in_game);
  });
  std::sort(out.recent.begin(), out.recent.end(),
            [](const auto& a, const auto& b) { return a.last_seen > b.last_seen; });
  for (const auto& [puid, pr] : presences) {
    out.room_players.push_back(make(puid, Find(puid)));
  }
  const int64_t now = std::time(nullptr);
  g_invites.erase(std::remove_if(g_invites.begin(), g_invites.end(),
                                 [now](const PendingInvite& i) {
                                   return now - i.time > kInviteLifetime;
                                 }),
                  g_invites.end());
  for (auto it = g_invites.rbegin(); it != g_invites.rend(); ++it) {
    out.invites.push_back({it->puid, it->gamertag, it->time});
  }
  return out;
}

int xeXFriendsSendChat(const std::string& raw) {
  const std::string text = CleanChat(raw);
  if (text.empty() || !eos::Active()) return 0;
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  std::vector<uint8_t> blob(kChatMagic, kChatMagic + 3);
  char tag[16] = {};
  const std::string name = LocalGamertag();
  std::memcpy(tag, name.data(), std::min<size_t>(name.size(), 15));
  blob.insert(blob.end(), tag, tag + 16);
  blob.insert(blob.end(), text.begin(), text.end());
  int sent = 0;
  for (const auto& [puid, pr] : Presences()) {
    eos::SendDirect(puid, blob);
    ++sent;
  }
  AddChat({name, text, int64_t(std::time(nullptr)), true});
  // Debug: chat_echo.on beside the exe delivers each message back as if
  // another player ("Echo") had sent it, to try the chat on one PC.
  std::error_code ec;
  if (std::filesystem::exists("chat_echo.on", ec)) {
    std::thread([text]() {
      std::this_thread::sleep_for(std::chrono::seconds(2));
      std::lock_guard<std::recursive_mutex> echo_lock(g_mutex);
      AddChat({"Echo", text, int64_t(std::time(nullptr)), false});
      ++g_chat_unread;
      xeXamShowToast("Echo: " + (text.size() > 60 ? text.substr(0, 57) + "..." : text));
    }).detach();
    return std::max(sent, 1);
  }
  return sent;
}

std::vector<GuideChatLine> xeXFriendsChat() {
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  return {g_chat.begin(), g_chat.end()};
}

int xeXFriendsChatUnread() {
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  return g_chat_unread;
}

std::vector<std::string> xeXFriendsTalkingNames() {
  const auto talkers = eos::VoiceTalkers();
  if (talkers.empty()) return {};
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  const std::string self = eos::LocalUserId();
  auto presences = Presences();
  std::vector<std::string> names;
  for (const auto& puid : talkers) {
    if (puid == self) {
      names.push_back(LocalGamertag());
      continue;
    }
    auto it = presences.find(puid);
    if (it != presences.end() && !it->second.gamertag.empty()) {
      names.push_back(it->second.gamertag);
    } else if (const Player* p = Find(puid); p && !p->gamertag.empty()) {
      names.push_back(p->gamertag);
    } else {
      names.push_back(puid.substr(0, 8));
    }
  }
  return names;
}

void xeXFriendsChatSeen() {
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  g_chat_unread = 0;
}

bool xeXFriendsInvite(const std::string& puid, const std::string& gamertag) {
  uint8_t info[kSessionInfoSize];
  if (!eos::Active() || !xeXSessionCurrentInfo(info)) return false;
  eos::SendDirect(puid, InviteBlob(info));
  xeXamShowToast("Invite sent to " + gamertag);
  return true;
}

bool xeXFriendsJoin(const std::string& puid) {
  std::string tag;
  uint8_t info[kSessionInfoSize];
  {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    auto presences = Presences();
    auto it = presences.find(puid);
    if (it == presences.end() || !it->second.in_session) return false;
    tag = it->second.gamertag;
    std::memcpy(info, it->second.info, kSessionInfoSize);
  }
  xeXamShowToast("Joining " + tag + "'s game");
  AcceptSession(puid, info);
  return true;
}

void xeXFriendsSetFriend(const std::string& puid, const std::string& gamertag, bool is_friend) {
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  Load();
  Player* p = Find(puid);
  if (!p) {
    g_players.push_back({puid, gamertag, false, 0});
    p = &g_players.back();
  }
  if (!gamertag.empty()) p->gamertag = gamertag;
  if (p->is_friend == is_friend) return;
  p->is_friend = is_friend;
  Save();
  xeXamShowToast(gamertag + (is_friend ? " added to friends" : " removed from friends"));
}

bool xeXFriendsAnswerInvite(const std::string& puid, bool accept) {
  std::optional<PendingInvite> invite;
  {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    for (auto it = g_invites.begin(); it != g_invites.end(); ++it) {
      if (it->puid == puid) {
        invite = std::move(*it);
        g_invites.erase(it);
        break;
      }
    }
  }
  if (!invite) return false;
  REXKRNL_INFO("Invite from {} {}", invite->gamertag, accept ? "accepted" : "declined");
  if (accept) AcceptSession(invite->puid, invite->info.data());
  return true;
}

}  // namespace rex::kernel::xam
