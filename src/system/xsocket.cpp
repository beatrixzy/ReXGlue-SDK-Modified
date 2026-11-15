/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <atomic>
#include <cstring>
#include <string>
#include <string_view>

#include <rex/cvar.h>
#include <rex/kernel/xam/module.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/eos_service.h>
#include <rex/system/xsocket.h>
// #include <rex/system/xnet.h>

#include <rex/net/socket.h>

// Standard socket types used by Xbox API emulation
#if REX_PLATFORM_WIN32
#include <WinSock2.h>

#include <WS2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <sys/socket.h>
#endif

REXCVAR_DEFINE_STRING(net_bind_address, "", "Network",
                      "IPv4 this PC uses for System Link (XNADDR and socket binds), e.g. its "
                      "Radmin VPN / ZeroTier / Hamachi address; empty = default-route interface");
REXCVAR_DEFINE_STRING(net_broadcast_targets, "", "Network",
                      "Comma-separated IPv4s that System Link broadcasts are sent to instead of "
                      "255.255.255.255 (for VPNs that drop broadcasts, or other PCs by address)");

namespace rex::system {

namespace net_link {

namespace {

uint32_t ParseAddress(std::string_view text) {
  while (!text.empty() && text.front() == ' ') text.remove_prefix(1);
  while (!text.empty() && text.back() == ' ') text.remove_suffix(1);
  if (text.empty()) {
    return 0;
  }
  in_addr addr = {};
  std::string str(text);
  if (inet_pton(AF_INET, str.c_str(), &addr) != 1) {
    REXSYS_WARN("net: '{}' is not an IPv4 address", str);
    return 0;
  }
  return addr.s_addr;
}

uint32_t DefaultRouteAddress() {
  // A connected UDP socket sends nothing but makes the OS pick the source
  // address of its default route.
  auto s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == static_cast<decltype(s)>(-1)) {
    return 0;
  }
  sockaddr_in to = {};
  to.sin_family = AF_INET;
  to.sin_port = htons(53);
  inet_pton(AF_INET, "8.8.8.8", &to.sin_addr);
  uint32_t result = 0;
  if (connect(s, reinterpret_cast<sockaddr*>(&to), sizeof(to)) == 0) {
    sockaddr_in local = {};
    socklen_t len = sizeof(local);
    if (getsockname(s, reinterpret_cast<sockaddr*>(&local), &len) == 0) {
      result = local.sin_addr.s_addr;
    }
  }
  rex::net::socket_close(s);
  return result;
}

}  // namespace

// True if some adapter on this PC owns addr (a UDP bind to it succeeds).
bool IsLocalAddress(uint32_t addr) {
  auto s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == static_cast<decltype(s)>(-1)) {
    return false;
  }
  sockaddr_in local = {};
  local.sin_family = AF_INET;
  local.sin_addr.s_addr = addr;
  bool ok = bind(s, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == 0;
  rex::net::socket_close(s);
  return ok;
}

uint32_t PinnedAddress() {
  static uint32_t cached = [] {
    const std::string& text = REXCVAR_GET(net_bind_address);
    uint32_t addr = ParseAddress(text);
    if (addr && !IsLocalAddress(addr)) {
      // Typically Hamachi/ZeroTier is disconnected; binding to it would make
      // every game socket fail (WSAEADDRNOTAVAIL), so use the default network.
      REXSYS_WARN(
          "net: net_bind_address {} is not on any network adapter of this PC (VPN not "
          "connected?); using the default network instead",
          text);
      addr = 0;
    }
    return addr;
  }();
  return cached;
}

uint32_t LocalAddress() {
  static uint32_t cached = [] {
    uint32_t addr = PinnedAddress();
    if (!addr) {
      addr = DefaultRouteAddress();
    }
    if (!addr) {
      addr = htonl(INADDR_LOOPBACK);
    }
    char text[INET_ADDRSTRLEN] = {};
    in_addr ia = {};
    ia.s_addr = addr;
    inet_ntop(AF_INET, &ia, text, sizeof(text));
    REXSYS_INFO("net: System Link address {}", text);
    return addr;
  }();
  return cached;
}

std::vector<uint32_t> BroadcastTargets() {
  static std::vector<uint32_t> cached = [] {
    std::vector<uint32_t> targets;
    std::string_view list = REXCVAR_GET(net_broadcast_targets);
    while (!list.empty()) {
      auto comma = list.find(',');
      if (uint32_t addr = ParseAddress(list.substr(0, comma))) {
        targets.push_back(addr);
      }
      if (comma == std::string_view::npos) {
        break;
      }
      list.remove_prefix(comma + 1);
    }
    if (targets.empty()) {
      targets.push_back(htonl(INADDR_BROADCAST));
    }
    return targets;
  }();
  return cached;
}

}  // namespace net_link

XSocket::XSocket(KernelState* kernel_state) : XObject(kernel_state, kObjectType) {}

XSocket::XSocket(KernelState* kernel_state, uint64_t native_handle)
    : XObject(kernel_state, kObjectType), native_handle_(native_handle) {}

XSocket::~XSocket() {
  Close();
}

X_STATUS XSocket::Initialize(AddressFamily af, Type type, Protocol proto) {
  af_ = af;
  type_ = type;
  proto_ = proto;

  if (proto == Protocol::X_IPPROTO_VDP) {
    // VDP is a layer on top of UDP.
    proto = Protocol::X_IPPROTO_UDP;
  }

  native_handle_ = socket(af, type, proto);
  if (native_handle_ == -1) {
    return X_STATUS_UNSUCCESSFUL;
  }

  // Online System Link: datagram sockets ride the EOS virtual LAN. The native
  // socket stays unbound and unused.
  virtual_ = type == Type::X_SOCK_DGRAM && xam::eos::Active();

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::Close() {
  if (virtual_ && bound_) {
    xam::eos::UnbindPort(bound_port_);
    bound_ = false;
  }
  int ret = rex::net::socket_close(native_handle_);
  if (ret != 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::SetOption(uint32_t level, uint32_t optname, void* optval_ptr, uint32_t optlen) {
  if (level == 0xFFFF && (optname == 0x5801 || optname == 0x5802)) {
    // Disable socket encryption
    secure_ = false;
    return X_STATUS_SUCCESS;
  }
  if (virtual_) {
    // Broadcast is a fan-out and blocking mode is queue semantics.
    return X_STATUS_SUCCESS;
  }

  int ret = setsockopt(native_handle_, level, optname, (char*)optval_ptr, optlen);
  if (ret < 0) {
    // TODO: WSAGetLastError()
    return X_STATUS_UNSUCCESSFUL;
  }

  // SO_BROADCAST
  if (level == 0xFFFF && optname == 0x0020) {
    broadcast_socket_ = true;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::IOControl(uint32_t cmd, uint8_t* arg_ptr) {
  if (virtual_) {
    return X_STATUS_SUCCESS;  // FIONBIO etc.: virtual sockets never block
  }
  int ret = rex::net::socket_ioctl(native_handle_, cmd, arg_ptr);
  if (ret < 0) {
    // TODO: Get last error
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::Connect(N_XSOCKADDR* name, int name_len) {
  int ret = connect(native_handle_, (sockaddr*)name, name_len);
  if (ret < 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::Bind(N_XSOCKADDR_IN* name, int name_len) {
  if (virtual_) {
    uint16_t port = name->sin_port;
    if (!port) {
      static std::atomic<uint16_t> next_ephemeral{49152};
      port = next_ephemeral++;
    }
    xam::eos::BindPort(port);
    bound_ = true;
    bound_port_ = port;
    return X_STATUS_SUCCESS;
  }

  // N_XSOCKADDR_IN keeps port and address in network order, as sockaddr_in.
  N_XSOCKADDR_IN local = *name;
  if (uint32_t pinned = net_link::PinnedAddress();
      pinned && uint32_t(local.sin_addr) == INADDR_ANY) {
    // Pin to the chosen adapter: a broadcast then leaves through it (Windows
    // otherwise uses the default route) and its traffic is all we see.
    local.sin_addr = ntohl(pinned);
  }
  int ret = bind(native_handle_, (sockaddr*)&local, name_len);
  if (ret < 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  bound_ = true;
  bound_port_ = name->sin_port;

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::Listen(int backlog) {
  int ret = listen(native_handle_, backlog);
  if (ret < 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

object_ref<XSocket> XSocket::Accept(N_XSOCKADDR* name, int* name_len) {
  sockaddr n_sockaddr;
  socklen_t n_name_len = sizeof(sockaddr);
  uintptr_t ret = accept(native_handle_, &n_sockaddr, &n_name_len);
  if (ret == -1) {
    std::memset(name, 0, *name_len);
    *name_len = 0;
    return nullptr;
  }

  std::memcpy(name, &n_sockaddr, n_name_len);
  *name_len = n_name_len;

  // Create a kernel object to represent the new socket, and copy parameters
  // over.
  auto socket = object_ref<XSocket>(new XSocket(kernel_state_, ret));
  socket->af_ = af_;
  socket->type_ = type_;
  socket->proto_ = proto_;

  return socket;
}

int XSocket::Shutdown(int how) {
  return shutdown(native_handle_, how);
}

int XSocket::Recv(uint8_t* buf, uint32_t buf_len, uint32_t flags) {
  return recv(native_handle_, reinterpret_cast<char*>(buf), buf_len, flags);
}

int XSocket::RecvFrom(uint8_t* buf, uint32_t buf_len, uint32_t flags, N_XSOCKADDR_IN* from,
                      uint32_t* from_len) {
  // Pop from secure packets first
  // TODO(DrChat): Enable when I commit XNet
  /*
  {
    std::lock_guard<std::mutex> lock(incoming_packet_mutex_);
    if (incoming_packets_.size()) {
      packet* pkt = (packet*)incoming_packets_.front();
      int data_len = pkt->data_len;
      std::memcpy(buf, pkt->data, std::min((uint32_t)pkt->data_len, buf_len));

      from->sin_family = 2;
      from->sin_addr = pkt->src_ip;
      from->sin_port = pkt->src_port;

      incoming_packets_.pop();
      uint8_t* pkt_ui8 = (uint8_t*)pkt;
      delete[] pkt_ui8;

      return data_len;
    }
  }
  */

  if (virtual_) {
    int err = 0;
    uint32_t src_ip = 0;
    uint16_t src_port = 0;
    int ret = bound_ ? xam::eos::RecvFrom(bound_port_, buf, buf_len, &src_ip, &src_port, &err)
                     : -1;
    if (!bound_) {
      err = 10022;  // WSAEINVAL: recvfrom on an unbound socket
    }
    if (ret < 0) {
#if REX_PLATFORM_WIN32
      WSASetLastError(err);
#endif
      return -1;
    }
    if (from) {
      from->sin_family = AF_INET;
      from->sin_addr = src_ip;
      from->sin_port = src_port;
      std::memset(from->x_sin_zero, 0, sizeof(from->x_sin_zero));
    }
    if (from_len) {
      *from_len = sizeof(sockaddr_in);
    }
    return ret;
  }

  sockaddr_in nfrom;
  socklen_t nfromlen = sizeof(sockaddr_in);
  int ret = recvfrom(native_handle_, reinterpret_cast<char*>(buf), buf_len, flags,
                     (sockaddr*)&nfrom, &nfromlen);
  if (from) {
    from->sin_family = nfrom.sin_family;
    from->sin_addr = ntohl(nfrom.sin_addr.s_addr);  // BE <- BE
    from->sin_port = ntohs(nfrom.sin_port);
    std::memset(from->x_sin_zero, 0, sizeof(from->x_sin_zero));
  }

  if (from_len) {
    *from_len = nfromlen;
  }

  return ret;
}

int XSocket::Send(const uint8_t* buf, uint32_t buf_len, uint32_t flags) {
  return send(native_handle_, reinterpret_cast<const char*>(buf), buf_len, flags);
}

int XSocket::SendTo(uint8_t* buf, uint32_t buf_len, uint32_t flags, N_XSOCKADDR_IN* to,
                    uint32_t to_len) {
  // Send 2 copies of the packet: One to XNet (for network security) and an
  // unencrypted copy for other Xenia hosts.
  // TODO(DrChat): Enable when I commit XNet.
  /*
  auto xam = kernel_state()->GetKernelModule<xam::XamModule>("xam.xex");
  auto xnet = xam->xnet();
  if (xnet) {
    xnet->SendPacket(this, to, buf, buf_len);
  }
  */

  if (virtual_) {
    int err = 10022;  // WSAEINVAL: a datagram needs a destination
    int ret = -1;
    if (to) {
      ret = xam::eos::SendTo(uint32_t(to->sin_addr), uint16_t(to->sin_port), bound_port_, buf,
                             buf_len, &err);
    }
    if (ret < 0) {
#if REX_PLATFORM_WIN32
      WSASetLastError(err);
#endif
    }
    return ret;
  }

  // to's fields read back as host values (be<> loads), so convert to network
  // order; copying them straight swapped the port and address bytes.
  sockaddr_in nto = {};
  if (to) {
    nto.sin_addr.s_addr = htonl(to->sin_addr);
    nto.sin_family = to->sin_family;
    nto.sin_port = htons(to->sin_port);
  }

  if (to && nto.sin_addr.s_addr == htonl(INADDR_BROADCAST)) {
    // System Link discovery: fan out to the configured targets.
    int ret = -1;
    for (uint32_t target : net_link::BroadcastTargets()) {
      nto.sin_addr.s_addr = target;
      int r = sendto(native_handle_, reinterpret_cast<char*>(buf), buf_len, flags,
                     (sockaddr*)&nto, sizeof(nto));
      if (r >= 0) {
        ret = r;
      }
    }
    return ret;
  }

  return sendto(native_handle_, reinterpret_cast<char*>(buf), buf_len, flags,
                to ? (sockaddr*)&nto : nullptr, to ? sizeof(nto) : 0);
}

bool XSocket::QueuePacket(uint32_t src_ip, uint16_t src_port, const uint8_t* buf, size_t len) {
  packet* pkt = reinterpret_cast<packet*>(new uint8_t[sizeof(packet) + len]);
  pkt->src_ip = src_ip;
  pkt->src_port = src_port;

  pkt->data_len = (uint16_t)len;
  std::memcpy(pkt->data, buf, len);

  std::lock_guard<std::mutex> lock(incoming_packet_mutex_);
  incoming_packets_.push((uint8_t*)pkt);

  // TODO: Limit on number of incoming packets?
  return true;
}

}  // namespace rex::system
