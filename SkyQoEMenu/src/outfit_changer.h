#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace skyqoe {

struct OutfitDefinition {
  std::uint32_t id = 0;
  std::string name;
  std::string season;
  std::string list_label;
  bool in_closet = false;
  bool is_default = false;
};

struct OutfitChangerSnapshot {
  bool supported = false;
  bool catalog_ready = false;
  bool game_thread_ready = false;
  bool pending = false;
  std::uint32_t total_count = 0;
  std::array<std::uint32_t, 10> slot_counts{};
  std::uint32_t pending_slot = 0;
  std::string pending_name;
  std::uint32_t last_slot = 0;
  std::string last_name;
  std::uint64_t applied = 0;
  std::uint64_t failed = 0;
  std::string resource_path;
  std::string status = "not initialized";
};

bool InitializeOutfitChanger();
void ShutdownOutfitChanger();
void SetOutfitGameThreadReady(bool ready);
bool QueueOutfitChange(std::uint32_t slot, const std::string& resource_name,
                       std::string& error);
void ProcessPendingOutfitChange();
OutfitChangerSnapshot GetOutfitChangerSnapshot();
const std::array<std::vector<OutfitDefinition>, 10>& GetOutfitCatalog();

}  // namespace skyqoe
