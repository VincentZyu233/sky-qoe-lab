#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "level_assets.h"
#include "local_effects.h"

namespace skyqoe {

constexpr std::uint32_t kSupportedTimestamp = 0x6A582C8E;
constexpr std::uint32_t kSupportedImageSize = 0x02FB2000;

struct BuildInfo {
  std::uint64_t module_base = 0;
  std::uint32_t timestamp = 0;
  std::uint32_t image_size = 0;
  bool supported = false;
};

struct OutfitSlotSnapshot {
  std::uint32_t index = 0;
  std::string type;
  std::uint32_t base_id = 0;
  std::uint32_t override_id = 0;
  std::uint32_t override_flag = 0;
  std::uint32_t effective_id = 0;
  std::string resource_name;
};

struct CoordinateCandidate {
  std::uint32_t offset = 0;
  std::array<float, 3> value{};
  float score = 0.0F;
};

struct TransformSnapshot {
  std::uint64_t address = 0;
  std::array<float, 3> position{};
  std::array<float, 3> right{};
  std::array<float, 3> up{};
  std::array<float, 3> forward{};
  bool valid = false;
};

struct NearbyTransformSnapshot {
  std::uint64_t address = 0;
  std::array<float, 3> position{};
  float distance = 0.0F;
};

struct WorldSnapshot {
  std::uint64_t root = 0;
  std::string manager_source;
  std::string level;
  std::string level_source;
  std::string scan_status;
  std::uint64_t scan_cycle = 0;
  std::uint64_t scanned_private_bytes = 0;
  std::uint32_t scanned_private_regions = 0;
  std::uint64_t object_header_candidates = 0;
  std::uint64_t transform_candidates = 0;
  std::uint64_t nearby_transform_count = 0;
  float nearby_radius = 100.0F;
  std::vector<NearbyTransformSnapshot> nearby_transforms;

  std::uint32_t room_current_players = 0;
  std::uint32_t room_max_players = 0;
  std::uint32_t avatar_capacity = 60;
  std::vector<std::uint32_t> room_max_candidates;

  bool level_assets_valid = false;
  std::string level_asset_status;
  std::string level_asset_path;
  std::uint32_t level_object_count = 0;
  std::uint32_t level_property_count = 0;
  std::uint32_t level_source_count = 0;
  std::uint32_t wax_spawner_count = 0;
  std::vector<LevelWaxTarget> wax_targets;

  bool wax_loop_enabled = false;
  std::uint32_t wax_loop_interval_ms = 900;
  std::uint32_t wax_loop_target_index = 0;
  std::uint64_t wax_loop_teleports = 0;
  std::string wax_loop_status;
};

enum class MoveDirection : std::uint32_t {
  Forward,
  Backward,
  Left,
  Right,
  Up,
  Down,
};

struct GameSnapshot {
  BuildInfo build;
  std::uint64_t manager = 0;
  std::uint64_t avatar = 0;
  std::uint64_t outfit = 0;
  std::uint64_t outfit_database = 0;
  std::int32_t avatar_index = -1;
  std::uint16_t avatar_flags = 0;
  std::uint8_t avatar_active = 0;
  bool valid = false;
  std::string status;
  std::array<OutfitSlotSnapshot, 10> slots{};
  std::vector<CoordinateCandidate> coordinate_candidates;
  TransformSnapshot transform;
  WorldSnapshot world;
  LocalEffectSnapshot local_effects;
};

class GameState {
 public:
  GameState();

  void SetManager(std::uint64_t manager);
  std::uint64_t Manager() const;
  void SetCoordinateScanEnabled(bool enabled);
  void Refresh();
  GameSnapshot Snapshot() const;
  bool ReadFloat4(std::uint64_t base, std::uint32_t offset, std::array<float, 4>& output) const;
  bool TeleportRelative(MoveDirection direction, float distance, std::string& error);
  bool TeleportAbsolute(const std::array<float, 3>& position, std::string& error);
  bool TryGetPlayerPose(std::array<float, 3>& position, std::array<float, 3>& up) const;
  void SetWaxLoopEnabled(bool enabled);
  bool WaxLoopEnabled() const;
  void SetWaxLoopInterval(std::uint32_t interval_ms);
  std::uint32_t WaxLoopInterval() const;
  void TickAutomation();

 private:
  template <typename T>
  bool Read(std::uint64_t address, T& output) const;

  bool ReadBytes(std::uint64_t address, void* output, std::size_t size) const;
  bool WriteBytes(std::uint64_t address, const void* input, std::size_t size) const;
  bool ReadMsvcString(std::uint64_t address, std::string& output) const;
  std::string ResolveResourceName(std::uint64_t database, std::uint32_t slot, std::uint32_t id) const;
  bool AdvanceAvatarDiscovery(std::uint64_t& avatar);
  void AdvanceWorldScan(const GameSnapshot& player);
  bool PopulateAvatar(std::uint64_t avatar, std::int32_t index, GameSnapshot& snapshot) const;
  bool PopulateTransform(std::uint64_t avatar, TransformSnapshot& transform) const;
  void PopulateRoom(std::uint64_t manager, WorldSnapshot& world) const;
  bool ValidateManager(std::uint64_t manager, std::uint64_t avatar,
                       std::int32_t* index = nullptr) const;
  void UpdateLevelAssets(const std::string& level, const std::string& source);
  void PopulateAutomation(WorldSnapshot& world) const;
  bool WritePosition(const TransformSnapshot& transform, const std::array<float, 3>& position,
                     std::string& error);
  void UpdateCoordinateCandidates(std::uint64_t avatar, GameSnapshot& snapshot);
  static BuildInfo ReadBuildInfo();

  std::atomic<std::uint64_t> manager_{0};
  std::atomic<std::uint64_t> discovered_avatar_{0};
  std::atomic<bool> coordinate_scan_enabled_{true};
  mutable std::mutex snapshot_mutex_;
  GameSnapshot snapshot_;
  std::uint64_t coordinate_avatar_ = 0;
  bool coordinate_initialized_ = false;
  std::vector<std::array<float, 3>> coordinate_previous_;
  std::vector<float> coordinate_scores_;
  std::uint64_t discovery_cursor_ = 0;
  std::chrono::steady_clock::time_point discovery_resume_at_{};
  std::vector<std::uint8_t> discovery_buffer_;

  std::uint64_t world_cursor_ = 0;
  std::vector<std::uint8_t> world_buffer_;
  WorldSnapshot world_cache_;
  std::uint64_t world_cycle_bytes_ = 0;
  std::uint32_t world_cycle_regions_ = 0;
  std::uint64_t world_cycle_object_headers_ = 0;
  std::uint64_t world_cycle_transforms_ = 0;
  std::uint64_t world_cycle_nearby_count_ = 0;
  std::vector<NearbyTransformSnapshot> world_cycle_nearby_;
  std::unordered_map<std::string, std::uint32_t> world_level_candidates_;

  std::atomic<bool> wax_loop_enabled_{false};
  std::atomic<std::uint32_t> wax_loop_interval_ms_{900};
  mutable std::mutex automation_mutex_;
  std::vector<bool> wax_visited_;
  std::string wax_automation_level_;
  std::chrono::steady_clock::time_point wax_next_teleport_at_{};
  std::uint32_t wax_target_index_ = 0;
  std::uint64_t wax_teleport_count_ = 0;
  std::string wax_automation_status_ = "disabled";
};

GameState& GetGameState();

}  // namespace skyqoe
