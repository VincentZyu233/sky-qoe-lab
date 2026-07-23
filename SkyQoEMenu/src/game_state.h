#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

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

 private:
  template <typename T>
  bool Read(std::uint64_t address, T& output) const;

  bool ReadBytes(std::uint64_t address, void* output, std::size_t size) const;
  bool ReadMsvcString(std::uint64_t address, std::string& output) const;
  std::string ResolveResourceName(std::uint64_t database, std::uint32_t slot, std::uint32_t id) const;
  bool AdvanceAvatarDiscovery(std::uint64_t& avatar);
  bool PopulateAvatar(std::uint64_t avatar, std::int32_t index, GameSnapshot& snapshot) const;
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
};

GameState& GetGameState();

}  // namespace skyqoe
