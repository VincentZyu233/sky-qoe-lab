#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace skyqoe {

struct LevelWaxTarget {
  std::uint32_t object_index = 0;
  std::string object_name;
  std::array<float, 3> position{};
  std::uint32_t spawn_count_min = 0;
  std::uint32_t spawn_count_max = 0;
  bool networked = false;
  bool always_spawn = false;
  bool usable = false;
};

struct LevelAssetSnapshot {
  bool valid = false;
  std::string status;
  std::string level;
  std::string path;
  std::uint32_t type_count = 0;
  std::uint32_t symbol_count = 0;
  std::uint32_t object_count = 0;
  std::uint32_t property_count = 0;
  std::uint32_t source_count = 0;
  std::uint32_t room_max_players = 0;
  std::vector<std::uint32_t> room_max_candidates;
  std::uint32_t wax_spawner_count = 0;
  std::vector<LevelWaxTarget> wax_targets;
};

LevelAssetSnapshot LoadLevelAssets(const std::string& level);
LevelAssetSnapshot ParseLevelAssetFile(const std::string& level, const std::wstring& path);

}  // namespace skyqoe
