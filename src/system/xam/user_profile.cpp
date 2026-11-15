/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <sstream>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <filesystem>
#include <fstream>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/user_profile.h>

REXCVAR_DEFINE_STRING(user_name, "User", "Kernel",
                      "Profile gamertag (up to 15 characters); the XUID, and so the saves, stay the "
                      "same. Left at User, the name last set in the Guide (gamertag.txt) is used");
REXCVAR_DEFINE_BOOL(signin_live, true, "Kernel",
                    "Report signed-in profiles as signed in to Xbox Live (state 2); PGR4 needs "
                    "it to host or join System Link races. Off = signed in locally (state 1)");
REXCVAR_DEFINE_STRING(user_name_extra, "Player", "Kernel",
                      "Gamertag prefix for players 2-4 (a pad on their slot signs them in); "
                      "the player number is appended");

namespace rex {
namespace system {
namespace xam {

namespace {

constexpr char kDefaultName[] = "User";

// Player 1's gamertag as last set in the Guide or the launcher.
std::filesystem::path GamertagFile() {
  return rex::filesystem::GetExecutableFolder() / "gamertag.txt";
}

std::string LoadSavedGamertag() {
  std::ifstream in(GamertagFile());
  std::string name;
  std::getline(in, name);
  while (!name.empty() && (name.back() == '\r' || name.back() == ' ')) name.pop_back();
  return name;
}

}  // namespace

UserProfile::UserProfile(uint32_t user_index) : user_index_(user_index) {
  // 58410A1F checks the user XUID against a mask of 0x00C0000000000000 (3<<54),
  // if non-zero, it prevents the user from playing the game.
  // "You do not have permissions to perform this operation."
  // Players 2-4 (signed in by a pad on their slot) get their own XUID and
  // gamertag, so titles see them as separate profiles.
  xuid_ = 0xB13EBABEBABEBABE + user_index;
  if (user_index == 0) {
    // A name given on the command line (the launcher's) wins; otherwise the
    // one saved from the Guide.
    name_ = REXCVAR_GET(user_name);
    if (name_ == kDefaultName) {
      if (std::string saved = LoadSavedGamertag(); !saved.empty()) name_ = saved;
    }
  } else {
    name_ = REXCVAR_GET(user_name_extra);
    if (name_.empty()) {
      name_ = "Player";
    }
    if (name_.size() > 13) {
      name_.resize(13);
    }
    name_ += " " + std::to_string(user_index + 1);
  }
  if (name_.empty()) {
    name_ = "User";
  }
  if (name_.size() > 15) {
    name_.resize(15);
  }

  // https://cs.rin.ru/forum/viewtopic.php?f=38&t=60668&hilit=gfwl+live&start=195
  // https://github.com/arkem/py360/blob/master/py360/constants.py
  // XPROFILE_GAMER_YAXIS_INVERSION
  AddSetting(std::make_unique<Int32Setting>(0x10040002, 0));
  // XPROFILE_OPTION_CONTROLLER_VIBRATION
  AddSetting(std::make_unique<Int32Setting>(0x10040003, 3));
  // XPROFILE_GAMERCARD_ZONE
  AddSetting(std::make_unique<Int32Setting>(0x10040004, 0));
  // XPROFILE_GAMERCARD_REGION
  AddSetting(std::make_unique<Int32Setting>(0x10040005, 0));
  // XPROFILE_GAMERCARD_CRED
  AddSetting(std::make_unique<Int32Setting>(0x10040006, 0xFA));
  // XPROFILE_GAMERCARD_REP
  AddSetting(std::make_unique<FloatSetting>(0x5004000B, 0.0f));
  // XPROFILE_OPTION_VOICE_MUTED
  AddSetting(std::make_unique<Int32Setting>(0x1004000C, 0));
  // XPROFILE_OPTION_VOICE_THRU_SPEAKERS
  AddSetting(std::make_unique<Int32Setting>(0x1004000D, 0));
  // XPROFILE_OPTION_VOICE_VOLUME
  AddSetting(std::make_unique<Int32Setting>(0x1004000E, 0x64));
  // XPROFILE_GAMERCARD_MOTTO
  AddSetting(std::make_unique<UnicodeSetting>(0x402C0011, u""));
  // XPROFILE_GAMERCARD_TITLES_PLAYED
  AddSetting(std::make_unique<Int32Setting>(0x10040012, 1));
  // XPROFILE_GAMERCARD_ACHIEVEMENTS_EARNED
  AddSetting(std::make_unique<Int32Setting>(0x10040013, 0));
  // XPROFILE_GAMER_DIFFICULTY
  AddSetting(std::make_unique<Int32Setting>(0x10040015, 0));
  // XPROFILE_GAMER_CONTROL_SENSITIVITY
  AddSetting(std::make_unique<Int32Setting>(0x10040018, 0));
  // Preferred color 1
  AddSetting(std::make_unique<Int32Setting>(0x1004001D, 0xFFFF0000u));
  // Preferred color 2
  AddSetting(std::make_unique<Int32Setting>(0x1004001E, 0xFF00FF00u));
  // XPROFILE_GAMER_ACTION_AUTO_AIM
  AddSetting(std::make_unique<Int32Setting>(0x10040022, 1));
  // XPROFILE_GAMER_ACTION_AUTO_CENTER
  AddSetting(std::make_unique<Int32Setting>(0x10040023, 0));
  // XPROFILE_GAMER_ACTION_MOVEMENT_CONTROL
  AddSetting(std::make_unique<Int32Setting>(0x10040024, 0));
  // XPROFILE_GAMER_RACE_TRANSMISSION
  AddSetting(std::make_unique<Int32Setting>(0x10040026, 0));
  // XPROFILE_GAMER_RACE_CAMERA_LOCATION
  AddSetting(std::make_unique<Int32Setting>(0x10040027, 0));
  // XPROFILE_GAMER_RACE_BRAKE_CONTROL
  AddSetting(std::make_unique<Int32Setting>(0x10040028, 0));
  // XPROFILE_GAMER_RACE_ACCELERATOR_CONTROL
  AddSetting(std::make_unique<Int32Setting>(0x10040029, 0));
  // XPROFILE_GAMERCARD_TITLE_CRED_EARNED
  AddSetting(std::make_unique<Int32Setting>(0x10040038, 0));
  // XPROFILE_GAMERCARD_TITLE_ACHIEVEMENTS_EARNED
  AddSetting(std::make_unique<Int32Setting>(0x10040039, 0));

  // If we set this, games will try to get it.
  // XPROFILE_GAMERCARD_PICTURE_KEY
  AddSetting(std::make_unique<UnicodeSetting>(0x4064000F, u"gamercard_picture_key"));

  // XPROFILE_TITLE_SPECIFIC1
  AddSetting(std::make_unique<BinarySetting>(0x63E83FFF));
  // XPROFILE_TITLE_SPECIFIC2
  AddSetting(std::make_unique<BinarySetting>(0x63E83FFE));
  // XPROFILE_TITLE_SPECIFIC3
  AddSetting(std::make_unique<BinarySetting>(0x63E83FFD));
}

uint32_t UserProfile::signin_state() const {
  return REXCVAR_GET(signin_live) ? 2 : 1;
}

void UserProfile::AddSetting(std::unique_ptr<Setting> setting) {
  Setting* previous_setting = setting.get();
  std::swap(settings_[setting->setting_id], previous_setting);

  if (setting->is_set && setting->is_title_specific()) {
    SaveSetting(setting.get());
  }

  if (previous_setting) {
    // replace: swap out the old setting from the owning list
    for (auto vec_it = setting_list_.begin(); vec_it != setting_list_.end(); ++vec_it) {
      if (vec_it->get() == previous_setting) {
        vec_it->swap(setting);
        break;
      }
    }
  } else {
    // new setting: add to the owning list
    setting_list_.push_back(std::move(setting));
  }
}

UserProfile::Setting* UserProfile::GetSetting(uint32_t setting_id) {
  const auto& it = settings_.find(setting_id);
  if (it == settings_.end()) {
    return nullptr;
  }
  UserProfile::Setting* setting = it->second;
  if (setting->is_title_specific()) {
    // If what we have loaded in memory isn't for the title that is running
    // right now, then load it from disk.
    if (kernel_state_->title_id() != setting->loaded_title_id) {
      LoadSetting(setting);
    }
  }
  return setting;
}

std::filesystem::path UserProfile::ContentDir() const {
  // content_root/title_id/profile/<XUID>, so each player keeps their own and
  // a new gamertag keeps the settings. Folders from before (named after the
  // gamertag) move over once.
  const auto profiles = kernel_state_->content_manager()->ResolveGameUserContentPath().parent_path();
  const auto dir = profiles / fmt::format("{:016X}", xuid_);
  std::error_code ec;
  if (!std::filesystem::exists(dir, ec)) {
    const auto old_dir = profiles / rex::to_path(name_);
    if (std::filesystem::is_directory(old_dir, ec)) {
      std::filesystem::rename(old_dir, dir, ec);
      if (ec) {
        REXSYS_WARN("profile: couldn't move {} to {}: {}", old_dir.string(), dir.string(),
                    ec.message());
        return old_dir;
      }
      REXSYS_INFO("profile: moved {}'s settings to {}", name_, dir.string());
    }
  }
  return dir;
}

bool UserProfile::Rename(std::string name) {
  while (!name.empty() && name.back() == ' ') name.pop_back();
  while (!name.empty() && name.front() == ' ') name.erase(name.begin());
  if (name.empty()) {
    return false;
  }
  if (name.size() > 15) {
    name.resize(15);
  }
  // Settings live under the XUID, so nothing moves.
  ContentDir();
  name_ = name;
  if (user_index_ == 0) {
    std::ofstream out(GamertagFile(), std::ios::trunc);
    out << name_ << '\n';
    if (!out) {
      REXSYS_WARN("profile: couldn't save the gamertag to {}", GamertagFile().string());
    }
  }
  REXSYS_INFO("profile: player {} is now {}", user_index_ + 1, name_);
  return true;
}

void UserProfile::LoadSetting(UserProfile::Setting* setting) {
  if (setting->is_title_specific()) {
    auto content_dir = ContentDir();
    auto setting_id = fmt::format("{:08X}", setting->setting_id);
    auto file_path = content_dir / setting_id;
    auto file = rex::filesystem::OpenFile(file_path, "rb");
    if (file) {
      fseek(file, 0, SEEK_END);
      uint32_t input_file_size = static_cast<uint32_t>(ftell(file));
      fseek(file, 0, SEEK_SET);

      std::vector<uint8_t> serialized_data(input_file_size);
      fread(serialized_data.data(), 1, serialized_data.size(), file);
      fclose(file);
      setting->Deserialize(serialized_data);
      setting->loaded_title_id = kernel_state_->title_id();
    }
  } else {
    // Unsupported for now.  Other settings aren't per-game and need to be
    // stored some other way.
    REXSYS_WARN("Attempting to load unsupported profile setting from disk");
  }
}

void UserProfile::SaveSetting(UserProfile::Setting* setting) {
  if (setting->is_title_specific()) {
    auto serialized_setting = setting->Serialize();
    auto content_dir = ContentDir();
    std::filesystem::create_directories(content_dir);
    auto setting_id = fmt::format("{:08X}", setting->setting_id);
    auto file_path = content_dir / setting_id;
    auto file = rex::filesystem::OpenFile(file_path, "wb");
    fwrite(serialized_setting.data(), 1, serialized_setting.size(), file);
    fclose(file);
  } else {
    // Unsupported for now.  Other settings aren't per-game and need to be
    // stored some other way.
    REXSYS_WARN("Attempting to save unsupported profile setting to disk");
  }
}

}  // namespace xam
}  // namespace system
}  // namespace rex
