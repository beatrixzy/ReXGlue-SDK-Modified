/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <mutex>
#include <unordered_set>

#include <rex/kernel/xam/apps/xlivebase_app.h>
#include <rex/kernel/xam/private.h>
#include <rex/logging.h>
#include <rex/thread.h>

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;
using namespace rex::system::xam;
namespace apps {
using namespace rex::system;

XLiveBaseApp::XLiveBaseApp(KernelState* kernel_state) : App(kernel_state, 0xFC) {}

// http://mb.mirage.org/bugzilla/xliveless/main.c

X_HRESULT XLiveBaseApp::DispatchMessageSync(uint32_t message, uint32_t buffer_ptr,
                                            uint32_t buffer_length) {
  // NOTE: buffer_length may be zero or valid.
  auto buffer = memory_->TranslateVirtual(buffer_ptr);
  switch (message) {
    case 0x00058004: {
      // Called on startup, seems to just return a bool in the buffer.
      assert_true(!buffer_length || buffer_length == 4);
      REXKRNL_DEBUG("XLiveBaseGetLogonId({:08X})", buffer_ptr);
      memory::store_and_swap<uint32_t>(buffer + 0, 1);  // ?
      return X_E_SUCCESS;
    }
    case 0x00058006: {
      assert_true(!buffer_length || buffer_length == 4);
      REXKRNL_DEBUG("XLiveBaseGetNatType({:08X})", buffer_ptr);
      memory::store_and_swap<uint32_t>(buffer + 0, 1);  // XONLINE_NAT_OPEN
      return X_E_SUCCESS;
    }
    case 0x00058007: {
      // Occurs if title calls XOnlineGetServiceInfo, expects dwServiceId
      // and pServiceInfo. pServiceInfo should contain pointer to
      // XONLINE_SERVICE_INFO structure.
      REXKRNL_DEBUG("CXLiveLogon::GetServiceInfo({:08X}, {:08X})", buffer_ptr, buffer_length);
      return 0x80151802;  // ERROR_CONNECTION_INVALID
    }
    case 0x00058020: {
      // CXLiveFriends::Enumerate: buffer_length is an X_ARGUMENT_LIST pointer
      // (user index, start, count, out buffer size, out handle).
      return xeXFriendsCreateEnumerator(buffer_length);
    }
    case 0x00058023: {
      // CXLiveMessaging::XMessageGameInviteGetAcceptedInfo: X_ARGUMENT_LIST
      // (user index, out X_INVITE_INFO).
      return xeXInviteGetAcceptedInfo(buffer_length);
    }
    case 0x00058009: {
      // XContentGetMarketplaceCounts: nothing on offer.
      return X_E_SUCCESS;
    }
    case 0x0005800E: {
      // XUserMuteListQuery (titles poll it every frame): nobody is muted.
      return X_E_SUCCESS;
    }
    case 0x00058046: {
      // Required to be successful for 4D530910 to detect signed-in profile
      // Doesn't seem to set anything in the given buffer, probably only takes
      // input
      REXKRNL_DEBUG("XLiveBaseUnk58046({:08X}, {:08X}) unimplemented", buffer_ptr, buffer_length);
      return X_E_SUCCESS;
    }
    case 0x00058037: {
      REXKRNL_DEBUG("XPresenceInitialize({:08X}, {:08X})", buffer_ptr, buffer_length);
      return X_E_SUCCESS;
    }
  }
  // Titles poll some of these every frame (PGR4: 0x5800E), so log each once.
  static std::mutex logged_mutex;
  static std::unordered_set<uint32_t> logged;
  {
    std::lock_guard<std::mutex> lock(logged_mutex);
    if (logged.insert(message).second) {
      REXKRNL_ERROR(
          "Unimplemented XLIVEBASE message app={:08X}, msg={:08X}, arg1={:08X}, "
          "arg2={:08X} (logged once)",
          app_id(), message, buffer_ptr, buffer_length);
    }
  }
  return X_E_FAIL;
}

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace rex
