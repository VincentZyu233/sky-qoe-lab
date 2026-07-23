#include "level_assets.h"

#include <windows.h>
#include <shellapi.h>

#include <iostream>

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
  const std::string level(level_wide.begin(), level_wide.end());
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
