#include "level_assets.h"

#include <windows.h>
#include <shellapi.h>

#include <iostream>
#include <string>
#include <string_view>

namespace {

std::string WideToUtf8(std::wstring_view value) {
  if (value.empty()) {
    return {};
  }
  const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                           static_cast<int>(value.size()), nullptr, 0,
                                           nullptr, nullptr);
  if (required <= 0) {
    return {};
  }
  std::string output(static_cast<std::size_t>(required), '\0');
  if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                          static_cast<int>(value.size()), output.data(), required,
                          nullptr, nullptr) != required) {
    return {};
  }
  return output;
}

}  // namespace

int wmain() {
  int argument_count = 0;
  LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
  if (!arguments || argument_count != 3) {
    std::cerr << "Usage: SkyLevelAssetInspect <level> <Objects.level.bin>\n";
    if (arguments) {
      LocalFree(arguments);
    }
    return 1;
  }
  const std::wstring level_wide = arguments[1];
  const std::wstring path = arguments[2];
  LocalFree(arguments);
  const std::string level = WideToUtf8(level_wide);
  if (level.empty() && !level_wide.empty()) {
    std::cerr << "Level name is not valid UTF-16\n";
    return 1;
  }
  const skyqoe::LevelAssetSnapshot snapshot = skyqoe::ParseLevelAssetFile(level, path);
  if (!snapshot.valid) {
    std::cerr << snapshot.status << '\n';
    return 2;
  }
  std::cout << "level=" << snapshot.level << " objects=" << snapshot.object_count
            << " properties=" << snapshot.property_count << " sources=" << snapshot.source_count
            << " roomMax=" << snapshot.room_max_players
            << " waxSpawners=" << snapshot.wax_spawner_count << '\n';
  std::uint32_t usable = 0;
  for (const auto& target : snapshot.wax_targets) {
    if (target.usable) {
      ++usable;
    }
    std::cout << target.object_index << ' ' << target.object_name << ' ' << target.position[0] << ' '
              << target.position[1] << ' ' << target.position[2] << " usable=" << target.usable
              << " networked=" << target.networked << '\n';
  }
  std::cout << "usable=" << usable << '\n';
  return snapshot.object_count == 9618 && snapshot.property_count == 25929 &&
                 snapshot.wax_spawner_count == 32 && usable == 30 &&
                 snapshot.room_max_players == 8
             ? 0
             : 3;
}
