#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2019 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <functional>
#include <string>
#include <vector>

#include <rex/system/export_resolver.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/user_profile.h>

namespace rex {
namespace kernel {
namespace xam {

bool xeXamIsUIActive();

// Local profile of a signed-in guest user, or null. User 0 is always signed
// in; users 1-3 are signed in while a physical pad sits on their slot.
system::xam::UserProfile* xeXamSignedInProfile(uint32_t user_index);
// Bit n set = user n signed in.
uint32_t xeXamSignedInMask();
// Broadcasts XN_SYS_SIGNINCHANGED when the mask moved since the last check.
// Cheap: rescans pads at most every 250 ms.
void xeXamPollSigninChanges();

// This PC's XNADDR (System Link address) into a 36-byte guest XNADDR.
void xeXNetFillLocalXnAddr(void* xnaddr);
// Cryptographically unimportant but per-PC-unique random bytes.
void xeXNetRandom(uint8_t* out, uint32_t length);

// Xbox Live sessions over the EOS room (xam_session.cpp). Objects are guest
// addresses from xeXSessionObject; IDs and values are host-order numerics.
uint32_t xeXSessionCreateHandle();
uint32_t xeXSessionObject(uint32_t handle);
void xeXSessionCreate(uint32_t obj, uint32_t flags, uint32_t max_public, uint32_t max_private,
                      uint32_t user_index, const uint8_t* info);
void xeXSessionJoin(uint32_t obj, uint32_t count, bool private_slots);
void xeXSessionLeave(uint32_t obj, uint32_t count);
void xeXSessionModify(uint32_t obj, uint32_t flags, uint32_t max_public, uint32_t max_private);
void xeXSessionDelete(uint32_t obj);
void xeXUserSetContext(uint32_t user_index, uint32_t id, uint32_t value);
void xeXUserSetProperty(uint32_t user_index, uint32_t id, uint32_t size, uint32_t value_ptr);
// Fill an XSESSION_SEARCHRESULT_HEADER at results_ptr; returns the count.
uint32_t xeXSessionSearch(uint32_t max_results, uint32_t num_contexts, uint32_t contexts_ptr,
                          uint32_t buffer_size, uint32_t results_ptr);
uint32_t xeXSessionSearchById(const uint8_t* kid, uint32_t buffer_size, uint32_t results_ptr);
void xeXNetQosSetListenData(const uint8_t* data, uint32_t size, bool clear);
// XSESSION_INFO (60 bytes) of the game session we host or are in; false if none.
bool xeXSessionCurrentInfo(uint8_t* out);

// Friends, recent players, presence and invites over the EOS room
// (xam_friends.cpp).
void xeXFriendsTick();  // cheap; call often (throttled inside)
X_HRESULT xeXFriendsCreateEnumerator(uint32_t arg_list_ptr);
X_HRESULT xeXInviteGetAcceptedInfo(uint32_t arg_list_ptr);
void xeXInviteShowUI(uint32_t user_index, const uint64_t* recipients, uint32_t count);
void xeXFriendsShowPlayers();  // opens the Guide on Recent Players
// Gamertag of a friend or recent player by online XUID; empty if unknown.
std::string xeXFriendsGamertag(uint64_t xuid);

// Leaderboards over stats_service (xam_stats.cpp).
void xeXSessionWriteStats(uint64_t xuid, uint32_t num_views, uint32_t views_ptr);
// Blocks on the network: run it through xeDeferMessageWork.
X_RESULT xeXUserReadStats(uint32_t title_id, uint32_t xuid_count, uint32_t xuids_ptr,
                          uint32_t spec_count, uint32_t specs_ptr, uint32_t results_size,
                          uint32_t results_ptr);

// An app message handler can hand slow work (network) back from
// DispatchMessageSync: XMsgStartIORequest(Ex) then completes its overlapped
// deferred with the work's result, or runs it inline without one (xam_msg.cpp).
void xeDeferMessageWork(std::function<X_RESULT()> work);

// What the Guide shows, gathered at once (thread-safe).
struct GuidePlayer {
  std::string puid, gamertag;
  bool is_friend = false, online = false, in_game = false;
  int64_t last_seen = 0;
};
struct GuideInvite {
  std::string puid, gamertag;
  int64_t time = 0;
};
struct GuideSnapshot {
  bool online_enabled = false;  // net_transport is eos
  bool connected = false;       // in the room, with a virtual IP
  bool in_game = false;         // we host or are in a Live session
  std::string room;             // room code; empty = public room
  std::vector<GuidePlayer> friends, recent, room_players;
  std::vector<GuideInvite> invites;  // newest first
};
GuideSnapshot xeXFriendsSnapshot();
// Guide actions; false when no longer possible (we or they left the game).
bool xeXFriendsInvite(const std::string& puid, const std::string& gamertag);
bool xeXFriendsJoin(const std::string& puid);
void xeXFriendsSetFriend(const std::string& puid, const std::string& gamertag, bool is_friend);
bool xeXFriendsAnswerInvite(const std::string& puid, bool accept);

// Room chat: text to everyone in the online room, shown as a notice and kept
// in the Guide (Online Room > Room Chat).
struct GuideChatLine {
  std::string gamertag, text;
  int64_t time = 0;  // unix seconds
  bool self = false;
};
// Sends to every player in the room; returns how many it went to.
int xeXFriendsSendChat(const std::string& text);
std::vector<GuideChatLine> xeXFriendsChat();  // oldest first
int xeXFriendsChatUnread();
void xeXFriendsChatSeen();
// Gamertags of everyone speaking in voice chat now (ours included).
std::vector<std::string> xeXFriendsTalkingNames();

// The Xbox Guide: opened by the Guide button (or Back+Start); returns at
// once. Blade to open on:
constexpr int kGuideBladeFriends = 0, kGuideBladeHome = 1, kGuideBladeRecent = 2,
              kGuideBladeRoom = 3;
// open_message: an announcement id to open straight away (0 = none).
void xeXamShowGuide(int blade = kGuideBladeHome, int64_t open_message = 0);

// Menu auto-repeat (menu_repeat_delay): a title can say when it is in its
// menus, where the left stick moves through lists too. Without one, only the
// D-pad is held back.
void xeXamSetMenuContext(bool (*in_menus)());
// Called with each user's raw pad buttons from XamInputGetState: opens the
// Guide on a Guide press, and says whether the game must see an idle pad
// (a system UI is up, or the button that closed it is still held).
bool xeXamFilterGameInput(uint32_t user_index, uint16_t buttons);
// Enter (not Alt+Enter) is held while the game window has focus: the
// keyboard's Guide key, whether or not keyboard-as-pad (mnk_mode) is on.
bool xeXamEnterKeyDown();

// Modal list picker over the game (blocks the caller); returns the chosen
// index, or -1 for cancel / no UI available.
int xeXamShowListDialog(const std::string& title, const std::string& description,
                        const std::vector<std::string>& items);
// Corner notice for a few seconds; the game keeps running. Also logged.
void xeXamShowToast(const std::string& text);
// Builds a completed XNQOS in guest memory for xnaddr_ptrs[count]; returns it.
uint32_t xeXNetQosLookup(uint32_t count, uint32_t xnaddr_ptrs, uint32_t probes);

rex::runtime::Export* RegisterExport_xam(rex::runtime::Export* export_entry);

// Registration functions, one per file.
#define XE_MODULE_EXPORT_GROUP(m, n)                                       \
  void Register##n##Exports(rex::runtime::ExportResolver* export_resolver, \
                            system::KernelState* kernel_state);
#include "module_export_groups.inc"
#undef XE_MODULE_EXPORT_GROUP

}  // namespace xam
}  // namespace kernel
}  // namespace rex
