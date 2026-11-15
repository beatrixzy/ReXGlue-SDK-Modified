/**
 * @file        rex/system/xam/eos_service.h
 * @brief       Epic Online Services: online identity and an internet
 *              virtual LAN for System Link play.
 *
 * With net_transport = "eos", players who set the same net_room meet in an
 * EOS lobby (the lobby only introduces peers), each claims a virtual IP
 * 10.77.0.x as a lobby member attribute, and the title's own System Link
 * traffic runs over EOS P2P (NAT punch-through, Epic relay as fallback):
 * XSocket routes bind/sendto/recvfrom here and the title XNADDR carries the
 * virtual IP. Broadcasts fan out to every peer. Datagrams carry a 4-byte
 * {u16 src_port, u16 dst_port} big-endian header so every guest socket
 * shares one EOS connection.
 *
 * Adapted from the acre (Ace Combat) rexglue port's eos_transport.
 *
 * Everything is optional at run time: without EOSSDK-Win64-Shipping.dll or
 * the credentials file beside the executable, or in a build without the EOS
 * headers, nothing starts and Active() stays false. IPs here are host-order
 * numerics, ports host-order; error codes are WSA numerics.
 */

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace rex::system::xam::eos {

// Starts EOS (platform, device-ID login, lobby by room code) on its own
// thread when net_transport is "eos". Returns at once.
void Startup(const std::string& display_name);
void Shutdown();

// net_transport is "eos" and EOS is starting or running: sockets and the
// XNADDR go through the virtual LAN.
bool Active();
// Logged in, in the room's lobby, with a virtual IP claimed.
bool Ready();
// This PC's virtual IP (host order), 0 until Ready().
uint32_t SelfVirtualIp();
// Remote lobby members with a virtual IP.
int PeerCount();
// vip (host order) belongs to someone still in the room.
bool IsPeer(uint32_t vip);

// Guest bind/close bookkeeping: only bound ports get receive queues.
void BindPort(uint16_t guest_port);
void UnbindPort(uint16_t guest_port);

// Sends len bytes from src_port to dst_ip:dst_port; broadcasts
// (255.255.255.255, x.x.x.255) fan out to every peer. Returns len, or -1 with
// *out_wsa_err set.
int SendTo(uint32_t dst_ip, uint16_t dst_port, uint16_t src_port, const uint8_t* data,
           uint32_t len, int* out_wsa_err);
// Non-blocking receive on a bound guest port: byte count with the source
// virtual IP/port, or -1 with *out_wsa_err = WSAEWOULDBLOCK when empty.
int RecvFrom(uint16_t guest_port, uint8_t* buf, uint32_t buf_len, uint32_t* out_src_ip,
             uint16_t* out_src_port, int* out_wsa_err);
// A datagram is waiting on guest_port (for select()).
bool HasPending(uint16_t guest_port);

// Xbox Live session announcements. A host sets an opaque blob (its
// advertised session, built by xam_session); the EOS thread sends it to every
// room member on P2P channel 1 when it changes and once a second, and
// receivers keep the latest one per peer for a few seconds. Empty = withdraw.
void SetAnnouncement(std::vector<uint8_t> blob);
struct Announcement {
  uint32_t vip = 0;  // sender's virtual IP (host order)
  std::vector<uint8_t> blob;
};
std::vector<Announcement> Announcements();
// Largest blob that fits one P2P packet.
constexpr uint32_t kMaxAnnouncementBytes = 1100;

// Generic per-peer broadcasts on a control channel (1 = Live sessions, which
// SetAnnouncement wraps; 2 = presence): same refresh/expiry rules.
void SetBroadcast(uint8_t channel, std::vector<uint8_t> blob);
struct PeerBlob {
  uint32_t vip = 0;  // sender's virtual IP (host order)
  std::string puid;  // sender's EOS ProductUserId
  std::vector<uint8_t> blob;
};
std::vector<PeerBlob> Broadcasts(uint8_t channel);
constexpr uint8_t kPresenceChannel = 2;

// One-off reliable message to a room member (invites); received ones queue
// up until taken.
void SendDirect(const std::string& puid, std::vector<uint8_t> blob);
std::vector<PeerBlob> TakeDirectMessages();

// Room members we exchanged game traffic with since the last call (recent
// players).
std::vector<std::string> TakeTrafficPeers();

// This PC's EOS ProductUserId, empty until logged in.
std::string LocalUserId();

// The room we are in (or moving to); empty = the public room.
std::string RoomCode();
// Moves to another room (empty = public) while running: leaves the current
// lobby and joins the new room's (creating it if nobody is there). With
// create_new, the code must be unused: it is checked first (staying put if
// taken) and a fresh lobby is created. note hears how it went, on the EOS
// thread. Codes ignore case.
void SwitchRoom(const std::string& code, bool create_new = false,
                std::function<void(const std::string&)> note = nullptr);

// Voice chat in the online room (EOS lobby RTC). Off until the player turns
// it on in the Guide; remembered in voice.txt beside the exe with the players
// they muted. Needs xaudio2_9redist.dll beside the exe and everyone in the
// room on a build with voice (older clients' rooms have no voice room).
bool VoiceAvailable();  // in a room that has a voice channel
bool VoiceEnabled();
void SetVoiceEnabled(bool enabled);
// Mic mute: with voice on, stop sending but keep hearing the room.
// Voice is only heard from players in the same game lobby: the room members
// (ProductUserIds) whose current session ID matches ours.
void SetVoiceLobby(const std::vector<std::string>& puids);
bool VoiceMicMuted();
void SetVoiceMicMuted(bool muted);
bool VoiceMuted(const std::string& puid);
void SetVoiceMuted(const std::string& puid, bool muted);
bool VoiceTalking(const std::string& puid);  // that player is speaking now
std::vector<std::string> VoiceTalkers();     // everyone speaking now (us too)

// Microphones for voice chat. VoiceMics() also asks for a fresh list, which
// arrives a moment later (the list may be empty until then).
struct MicDevice {
  std::string id;
  std::string name;
  bool is_default = false;
};
std::vector<MicDevice> VoiceMics();
std::string VoiceMic();                    // the chosen device's id; "" = the system default
void SetVoiceMic(const std::string& id);   // "" = the system default

}  // namespace rex::system::xam::eos
