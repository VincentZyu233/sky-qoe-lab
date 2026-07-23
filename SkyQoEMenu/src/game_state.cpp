#include "game_state.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <string_view>

namespace skyqoe {
namespace {

constexpr std::array<const char*, 10> kSlotNames = {
    "Body", "Wing", "Hair", "Mask", "Neck", "Feet", "Horn", "Face", "Prop", "Hat",
};
constexpr std::uint64_t kAvatarStride = 0x10B20;
constexpr std::uint64_t kAvatarArrayOffset = 0x30;
constexpr std::uint64_t kAvatarOutfitOffset = 0x58;
constexpr std::uint64_t kAvatarActiveOffset = 0xB850;
constexpr std::uint64_t kAvatarFlagsOffset = 0x109EC;
constexpr std::size_t kCoordinateScanSize = 0x3000;
constexpr std::size_t kCoordinateStride = sizeof(float);
constexpr std::size_t kDiscoveryChunkSize = 1024 * 1024;
constexpr std::size_t kDiscoveryBudgetBytes = 8 * kDiscoveryChunkSize;
constexpr std::uint64_t kGameRootVtableRva = 0x01E1F070;
constexpr std::uint64_t kGameRootManagerOffset = 0x310;
constexpr std::size_t kWorldChunkSize = 1024 * 1024;
constexpr std::size_t kWorldBudgetBytes = 8 * kWorldChunkSize;
constexpr float kNearbyRadius = 100.0F;
constexpr std::size_t kMaximumNearbyTransforms = 256;

bool IsPlausibleCoordinate(const std::array<float, 3>& value) {
  float largest = 0.0F;
  for (float component : value) {
    if (!std::isfinite(component) || std::abs(component) > 100000.0F) {
      return false;
    }
    largest = std::max(largest, std::abs(component));
  }
  return largest > 0.001F;
}

float Distance(const std::array<float, 3>& left, const std::array<float, 3>& right) {
  const float x = left[0] - right[0];
  const float y = left[1] - right[1];
  const float z = left[2] - right[2];
  return std::sqrt(x * x + y * y + z * z);
}

bool IsFiniteVector(const std::array<float, 3>& value) {
  return std::all_of(value.begin(), value.end(), [](float component) {
    return std::isfinite(component) && std::abs(component) <= 1000000.0F;
  });
}

float VectorLength(const std::array<float, 3>& value) {
  return std::sqrt(value[0] * value[0] + value[1] * value[1] + value[2] * value[2]);
}

float Dot(const std::array<float, 3>& left, const std::array<float, 3>& right) {
  return left[0] * right[0] + left[1] * right[1] + left[2] * right[2];
}

bool IsRuntimeTransform(const std::uint8_t* bytes, std::array<float, 3>& position) {
  std::array<float, 4> primary{};
  std::array<float, 4> secondary{};
  std::array<float, 4> right{};
  std::array<float, 4> up{};
  std::array<float, 4> forward{};
  std::memcpy(primary.data(), bytes, sizeof(primary));
  std::memcpy(secondary.data(), bytes + 0x10, sizeof(secondary));
  std::memcpy(right.data(), bytes + 0x50, sizeof(right));
  std::memcpy(up.data(), bytes + 0x60, sizeof(up));
  std::memcpy(forward.data(), bytes + 0x70, sizeof(forward));
  for (std::size_t index = 0; index < 3; ++index) {
    position[index] = primary[index];
    if (!std::isfinite(primary[index]) || !std::isfinite(secondary[index]) ||
        std::abs(primary[index]) > 100000.0F ||
        std::abs(primary[index] - secondary[index]) > 0.05F) {
      return false;
    }
  }
  const std::array<float, 3> right3 = {right[0], right[1], right[2]};
  const std::array<float, 3> up3 = {up[0], up[1], up[2]};
  const std::array<float, 3> forward3 = {forward[0], forward[1], forward[2]};
  const float right_length = VectorLength(right3);
  const float up_length = VectorLength(up3);
  const float forward_length = VectorLength(forward3);
  return IsFiniteVector(right3) && IsFiniteVector(up3) && IsFiniteVector(forward3) &&
         right_length > 0.5F && right_length < 1.5F && up_length > 0.5F && up_length < 1.5F &&
         forward_length > 0.5F && forward_length < 1.5F &&
         std::abs(Dot(right3, up3)) < 0.15F && std::abs(Dot(right3, forward3)) < 0.15F &&
         std::abs(Dot(up3, forward3)) < 0.15F;
}

bool IsValidLevelName(std::string_view level) {
  return !level.empty() && level.size() <= 128 &&
         std::all_of(level.begin(), level.end(), [](unsigned char character) {
           return std::isalnum(character) || character == '_' || character == '-';
         });
}

}  // namespace

GameState::GameState()
    : discovery_buffer_(kDiscoveryChunkSize), world_buffer_(kWorldChunkSize) {
  snapshot_.build = ReadBuildInfo();
  snapshot_.status = "waiting for player manager";
  world_cache_.nearby_radius = kNearbyRadius;
  world_cache_.scan_status = "waiting for player";
  world_cache_.wax_loop_status = "disabled";
}

void GameState::SetManager(std::uint64_t manager) {
  manager_.store(manager, std::memory_order_release);
  if (manager != 0) {
    discovered_avatar_.store(0, std::memory_order_release);
  }
  coordinate_avatar_ = 0;
  coordinate_initialized_ = false;
  discovery_cursor_ = 0;
  discovery_resume_at_ = {};
}

std::uint64_t GameState::Manager() const {
  return manager_.load(std::memory_order_acquire);
}

void GameState::SetCoordinateScanEnabled(bool enabled) {
  coordinate_scan_enabled_.store(enabled, std::memory_order_release);
}

template <typename T>
bool GameState::Read(std::uint64_t address, T& output) const {
  return ReadBytes(address, &output, sizeof(T));
}

bool GameState::ReadBytes(std::uint64_t address, void* output, std::size_t size) const {
  if (address < 0x10000 || output == nullptr || size == 0) {
    return false;
  }
  SIZE_T bytes_read = 0;
  return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), output, size,
                           &bytes_read) != FALSE &&
         bytes_read == size;
}

bool GameState::WriteBytes(std::uint64_t address, const void* input, std::size_t size) const {
  if (address < 0x10000 || input == nullptr || size == 0) {
    return false;
  }
  SIZE_T bytes_written = 0;
  return WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address), input, size,
                            &bytes_written) != FALSE &&
         bytes_written == size;
}

BuildInfo GameState::ReadBuildInfo() {
  BuildInfo info;
  const auto module = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
  if (!module) {
    return info;
  }
  const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
    return info;
  }
  const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(module + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) {
    return info;
  }
  info.module_base = reinterpret_cast<std::uint64_t>(module);
  info.timestamp = nt->FileHeader.TimeDateStamp;
  info.image_size = nt->OptionalHeader.SizeOfImage;
  info.supported = info.timestamp == kSupportedTimestamp && info.image_size == kSupportedImageSize;
  return info;
}

bool GameState::ReadMsvcString(std::uint64_t address, std::string& output) const {
  std::uint64_t length = 0;
  std::uint64_t capacity = 0;
  if (!Read(address + 0x10, length) || !Read(address + 0x18, capacity) || length > 4096 ||
      capacity < length) {
    return false;
  }
  std::uint64_t data_address = address;
  if (capacity >= 16 && !Read(address, data_address)) {
    return false;
  }
  std::string text(static_cast<std::size_t>(length), '\0');
  if (length != 0 && !ReadBytes(data_address, text.data(), static_cast<std::size_t>(length))) {
    return false;
  }
  output = std::move(text);
  return true;
}

std::string GameState::ResolveResourceName(std::uint64_t database, std::uint32_t slot,
                                           std::uint32_t id) const {
  if (database == 0 || id == 0) {
    return {};
  }
  std::uint32_t mask = 0;
  std::uint64_t sentinel = 0;
  std::uint64_t buckets = 0;
  if (!Read(database + 0x50A8, mask) || !Read(database + 0x5080, sentinel) ||
      !Read(database + 0x5090, buckets) || buckets == 0 || sentinel == 0) {
    return {};
  }

  std::uint32_t hash = id;
  hash = (hash ^ (hash >> 16U)) * 0x85EBCA6BU;
  hash = (hash ^ (hash >> 13U)) * 0xC2B2AE35U;
  hash ^= hash >> 16U;

  const std::uint64_t bucket = buckets + static_cast<std::uint64_t>(hash & mask) * 0x10;
  std::uint64_t first = 0;
  std::uint64_t current = 0;
  if (!Read(bucket, first) || !Read(bucket + 8, current) || current == 0 || current == sentinel) {
    return {};
  }

  for (std::uint32_t iteration = 0; iteration < 4096; ++iteration) {
    std::uint32_t node_id = 0;
    if (!Read(current + 0x10, node_id)) {
      return {};
    }
    if (node_id == id) {
      std::uint32_t record_index = 0;
      if (!Read(current + 0x14, record_index) || record_index > 0x100000) {
        return {};
      }
      const std::uint64_t record = database + 0x50C0 + static_cast<std::uint64_t>(record_index) * 0xE50;
      std::uint32_t record_id = 0;
      std::uint32_t record_slot = 0;
      if (!Read(record, record_id) || !Read(record + 8, record_slot) || record_id != id ||
          record_slot != slot) {
        return {};
      }
      std::string name;
      return ReadMsvcString(record + 0x10, name) ? name : std::string{};
    }
    if (current == first || !Read(current + 8, current) || current == 0) {
      return {};
    }
  }
  return {};
}

bool GameState::PopulateAvatar(std::uint64_t avatar, std::int32_t index,
                               GameSnapshot& snapshot) const {
  std::uint8_t active = 0;
  std::uint16_t flags = 0;
  std::uint64_t outfit = 0;
  std::uint64_t reverse_avatar = 0;
  std::uint64_t database = 0;
  std::uint32_t database_mask = 0;
  std::uint64_t database_sentinel = 0;
  std::uint64_t database_buckets = 0;
  std::uint64_t pointer_probe = 0;
  if (!Read(avatar + kAvatarActiveOffset, active) || active != 1 ||
      !Read(avatar + kAvatarFlagsOffset, flags) || (flags & 0x08U) == 0 ||
      !Read(avatar + kAvatarOutfitOffset, outfit) || outfit == 0 ||
      (outfit >= avatar && outfit < avatar + kAvatarStride) ||
      !Read(outfit + 8, reverse_avatar) || reverse_avatar != avatar ||
      !Read(outfit + 0x10, database) || database == 0 ||
      (database >= outfit && database < outfit + 0x2000) ||
      !Read(database + 0x50A8, database_mask) || database_mask > 0x00FFFFFFU ||
      (database_mask & (database_mask + 1U)) != 0 ||
      !Read(database + 0x5080, database_sentinel) || database_sentinel == 0 ||
      !Read(database + 0x5090, database_buckets) || database_buckets == 0 ||
      !Read(database_sentinel, pointer_probe) || !Read(database_buckets, pointer_probe)) {
    return false;
  }

  bool has_resolved_outfit = false;
  for (std::uint32_t slot = 0; slot < kSlotNames.size(); ++slot) {
    std::uint32_t base_id = 0;
    std::uint32_t override_id = 0;
    std::uint32_t override_flag = 0;
    if (!Read(outfit + 0x54 + slot * 4, base_id) ||
        !Read(outfit + 0x1D48 + slot * 4, override_id) ||
        !Read(outfit + 0x1D7C + slot * 4, override_flag)) {
      return false;
    }
    const std::uint32_t effective_id = override_flag != 0 ? override_id : base_id;
    if (effective_id != 0 && !ResolveResourceName(database, slot, effective_id).empty()) {
      has_resolved_outfit = true;
      break;
    }
  }
  if (!has_resolved_outfit) {
    return false;
  }
  snapshot.avatar = avatar;
  snapshot.avatar_index = index;
  snapshot.avatar_active = active;
  snapshot.avatar_flags = flags;
  snapshot.outfit = outfit;
  snapshot.outfit_database = database;
  snapshot.valid = true;
  snapshot.status = index == -2 ? "ready (auto-discovered avatar)" : "ready";
  return true;
}

bool GameState::PopulateTransform(std::uint64_t avatar, TransformSnapshot& transform) const {
  std::uint64_t address = 0;
  std::array<float, 4> position{};
  std::array<float, 4> right{};
  std::array<float, 4> up{};
  std::array<float, 4> forward{};
  if (!Read(avatar + 0x18, address) || address == 0 || !Read(address, position) ||
      !Read(address + 0x50, right) || !Read(address + 0x60, up) ||
      !Read(address + 0x70, forward)) {
    return false;
  }

  TransformSnapshot next;
  next.address = address;
  std::copy_n(position.begin(), 3, next.position.begin());
  std::copy_n(right.begin(), 3, next.right.begin());
  std::copy_n(up.begin(), 3, next.up.begin());
  std::copy_n(forward.begin(), 3, next.forward.begin());
  const float right_length = VectorLength(next.right);
  const float up_length = VectorLength(next.up);
  const float forward_length = VectorLength(next.forward);
  if (!IsFiniteVector(next.position) || !IsFiniteVector(next.right) || !IsFiniteVector(next.up) ||
      !IsFiniteVector(next.forward) || right_length < 0.5F || right_length > 1.5F ||
      up_length < 0.5F || up_length > 1.5F || forward_length < 0.5F ||
      forward_length > 1.5F) {
    return false;
  }
  next.valid = true;
  transform = next;
  return true;
}

bool GameState::AdvanceAvatarDiscovery(std::uint64_t& avatar) {
  SYSTEM_INFO system_info{};
  GetSystemInfo(&system_info);
  const std::uint64_t first_address = std::max<std::uint64_t>(
      0x10000, reinterpret_cast<std::uint64_t>(system_info.lpMinimumApplicationAddress));
  const std::uint64_t stop =
      reinterpret_cast<std::uint64_t>(system_info.lpMaximumApplicationAddress);
  const auto now = std::chrono::steady_clock::now();
  if (now < discovery_resume_at_) {
    return false;
  }
  if (discovery_cursor_ < first_address || discovery_cursor_ >= stop) {
    discovery_cursor_ = first_address;
  }

  std::size_t scanned_bytes = 0;
  while (discovery_cursor_ < stop && scanned_bytes < kDiscoveryBudgetBytes) {
    const std::uint64_t address = discovery_cursor_;
    MEMORY_BASIC_INFORMATION information{};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &information, sizeof(information)) == 0) {
      discovery_cursor_ += 0x1000;
      continue;
    }
    const std::uint64_t region_base = reinterpret_cast<std::uint64_t>(information.BaseAddress);
    const std::uint64_t region_end = region_base + information.RegionSize;
    const DWORD blocked = PAGE_GUARD | PAGE_NOACCESS;
    const bool readable = information.State == MEM_COMMIT && information.Type == MEM_PRIVATE &&
                          (information.Protect & blocked) == 0;
    if (!readable) {
      discovery_cursor_ = region_end > address ? region_end : address + 0x1000;
      continue;
    }

    const std::uint64_t chunk_base = std::max(address, region_base);
    const std::size_t size = static_cast<std::size_t>(
        std::min<std::uint64_t>(kDiscoveryChunkSize, region_end - chunk_base));
    discovery_cursor_ = chunk_base + size;
    scanned_bytes += size;

    SIZE_T bytes_read = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(chunk_base),
                           discovery_buffer_.data(), size, &bytes_read) ||
        bytes_read < sizeof(std::uint64_t)) {
      continue;
    }
    const std::uint64_t first_field = (chunk_base + 15U) & ~std::uint64_t{15U};
    const std::uint64_t first_outfit_field = first_field + 8U;
    for (std::uint64_t field_address = first_outfit_field;
         field_address + sizeof(std::uint64_t) <= chunk_base + bytes_read;
         field_address += 16U) {
      const std::size_t offset = static_cast<std::size_t>(field_address - chunk_base);
      std::uint64_t outfit = 0;
      std::memcpy(&outfit, discovery_buffer_.data() + offset, sizeof(outfit));
      if (outfit < first_address || outfit >= stop || field_address < kAvatarOutfitOffset) {
        continue;
      }
      const std::uint64_t candidate_avatar = field_address - kAvatarOutfitOffset;
      std::uint64_t reverse_avatar = 0;
      if (!Read(outfit + 8, reverse_avatar) || reverse_avatar != candidate_avatar) {
        continue;
      }
      GameSnapshot probe;
      if (PopulateAvatar(candidate_avatar, -2, probe)) {
        avatar = candidate_avatar;
        return true;
      }
    }
  }

  if (discovery_cursor_ >= stop) {
    discovery_cursor_ = first_address;
    discovery_resume_at_ = now + std::chrono::seconds(5);
  }
  return false;
}

bool GameState::ValidateManager(std::uint64_t manager, std::uint64_t avatar,
                                std::int32_t* index) const {
  if (manager < 0x10000 || avatar < 0x10000) {
    return false;
  }
  for (std::uint32_t slot_index = 0; slot_index < 60; ++slot_index) {
    const std::uint64_t slot = manager + kAvatarArrayOffset + slot_index * kAvatarStride;
    if (slot != avatar) {
      continue;
    }
    std::uint8_t active = 0;
    std::uint64_t outfit = 0;
    std::uint64_t reverse_avatar = 0;
    if (!Read(slot + kAvatarActiveOffset, active) || active != 1 ||
        !Read(slot + kAvatarOutfitOffset, outfit) || outfit < 0x10000 ||
        !Read(outfit + 8, reverse_avatar) || reverse_avatar != slot) {
      return false;
    }
    if (index) {
      *index = static_cast<std::int32_t>(slot_index);
    }
    return true;
  }
  return false;
}

void GameState::PopulateRoom(std::uint64_t manager, WorldSnapshot& world) const {
  world.avatar_capacity = 60;
  world.room_current_players = 0;
  if (manager < 0x10000) {
    return;
  }
  for (std::uint32_t index = 0; index < world.avatar_capacity; ++index) {
    const std::uint64_t avatar = manager + kAvatarArrayOffset + index * kAvatarStride;
    std::uint8_t active = 0;
    std::uint64_t outfit = 0;
    std::uint64_t reverse_avatar = 0;
    if (Read(avatar + kAvatarActiveOffset, active) && active == 1 &&
        Read(avatar + kAvatarOutfitOffset, outfit) && outfit >= 0x10000 &&
        Read(outfit + 8, reverse_avatar) && reverse_avatar == avatar) {
      ++world.room_current_players;
    }
  }
}

void GameState::UpdateLevelAssets(const std::string& level, const std::string& source) {
  if (!IsValidLevelName(level)) {
    return;
  }
  if (world_cache_.level == level && world_cache_.level_assets_valid) {
    return;
  }
  const LevelAssetSnapshot assets = LoadLevelAssets(level);
  world_cache_.level = level;
  world_cache_.level_source = source;
  world_cache_.level_assets_valid = assets.valid;
  world_cache_.level_asset_status = assets.status;
  world_cache_.level_asset_path = assets.path;
  world_cache_.level_object_count = assets.object_count;
  world_cache_.level_property_count = assets.property_count;
  world_cache_.level_source_count = assets.source_count;
  world_cache_.room_max_players = assets.room_max_players;
  world_cache_.room_max_candidates = assets.room_max_candidates;
  world_cache_.wax_spawner_count = assets.wax_spawner_count;
  world_cache_.wax_targets = assets.wax_targets;

  std::scoped_lock lock(automation_mutex_);
  wax_automation_level_.clear();
  wax_visited_.clear();
  wax_target_index_ = 0;
  wax_next_teleport_at_ = {};
  wax_automation_status_ = assets.valid ? "ready" : assets.status;
}

void GameState::PopulateAutomation(WorldSnapshot& world) const {
  world.wax_loop_enabled = wax_loop_enabled_.load(std::memory_order_acquire);
  world.wax_loop_interval_ms = wax_loop_interval_ms_.load(std::memory_order_acquire);
  std::scoped_lock lock(automation_mutex_);
  world.wax_loop_target_index = wax_target_index_;
  world.wax_loop_teleports = wax_teleport_count_;
  world.wax_loop_status = wax_automation_status_;
}

void GameState::AdvanceWorldScan(const GameSnapshot& player) {
  if (!player.valid || !player.transform.valid) {
    world_cache_.scan_status = "waiting for player transform";
    return;
  }

  SYSTEM_INFO system_info{};
  GetSystemInfo(&system_info);
  const std::uint64_t first_address = std::max<std::uint64_t>(
      0x10000, reinterpret_cast<std::uint64_t>(system_info.lpMinimumApplicationAddress));
  const std::uint64_t stop =
      reinterpret_cast<std::uint64_t>(system_info.lpMaximumApplicationAddress);
  if (world_cursor_ < first_address || world_cursor_ >= stop) {
    world_cursor_ = first_address;
  }
  world_cache_.scan_status = "scanning private world state";

  std::size_t scanned_this_refresh = 0;
  while (world_cursor_ < stop && scanned_this_refresh < kWorldBudgetBytes) {
    const std::uint64_t address = world_cursor_;
    MEMORY_BASIC_INFORMATION information{};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &information, sizeof(information)) ==
        0) {
      world_cursor_ += 0x1000;
      continue;
    }
    const std::uint64_t region_base = reinterpret_cast<std::uint64_t>(information.BaseAddress);
    const std::uint64_t region_end = region_base + information.RegionSize;
    const DWORD blocked = PAGE_GUARD | PAGE_NOACCESS;
    const bool readable = information.State == MEM_COMMIT && information.Type == MEM_PRIVATE &&
                          (information.Protect & blocked) == 0;
    if (!readable) {
      world_cursor_ = region_end > address ? region_end : address + 0x1000;
      continue;
    }

    const std::uint64_t chunk_base = std::max(address, region_base);
    const std::size_t size = static_cast<std::size_t>(
        std::min<std::uint64_t>(kWorldChunkSize, region_end - chunk_base));
    world_cursor_ = chunk_base + size;
    scanned_this_refresh += size;

    SIZE_T bytes_read = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(chunk_base),
                           world_buffer_.data(), size, &bytes_read) ||
        bytes_read < 16) {
      continue;
    }
    world_cycle_bytes_ += bytes_read;
    if (chunk_base == region_base) {
      ++world_cycle_regions_;
    }

    const std::uint64_t module_min = player.build.module_base;
    const std::uint64_t module_max = module_min + player.build.image_size;
    const std::uint64_t root_vtable = module_min + kGameRootVtableRva;
    std::uint64_t qword_address = (chunk_base + 7U) & ~std::uint64_t{7U};
    for (; qword_address + sizeof(std::uint64_t) <= chunk_base + bytes_read;
         qword_address += sizeof(std::uint64_t)) {
      std::uint64_t value = 0;
      std::memcpy(&value, world_buffer_.data() + (qword_address - chunk_base), sizeof(value));
      if (value >= module_min && value < module_max) {
        ++world_cycle_object_headers_;
      }
      if (world_cache_.root == 0 && value == root_vtable) {
        std::uint64_t manager = 0;
        if (Read(qword_address + kGameRootManagerOffset, manager) &&
            ValidateManager(manager, player.avatar)) {
          world_cache_.root = qword_address;
          world_cache_.manager_source = "game-root+0x310";
          manager_.store(manager, std::memory_order_release);
        }
      }
    }

    std::uint64_t transform_address = (chunk_base + 15U) & ~std::uint64_t{15U};
    for (; transform_address + 0x80 <= chunk_base + bytes_read; transform_address += 16U) {
      std::array<float, 3> position{};
      if (!IsRuntimeTransform(world_buffer_.data() + (transform_address - chunk_base), position)) {
        continue;
      }
      ++world_cycle_transforms_;
      const float distance = Distance(position, player.transform.position);
      if (distance <= kNearbyRadius) {
        ++world_cycle_nearby_count_;
        if (world_cycle_nearby_.size() < kMaximumNearbyTransforms * 4) {
          world_cycle_nearby_.push_back({transform_address, position, distance});
        }
      }
    }

    const auto scan_text = [&](std::string_view pattern, const auto& callback) {
      const auto begin = world_buffer_.begin();
      const auto end = begin + static_cast<std::ptrdiff_t>(bytes_read);
      auto current = begin;
      while (current != end) {
        const auto found = std::search(current, end, pattern.begin(), pattern.end());
        if (found == end) {
          break;
        }
        callback(static_cast<std::size_t>(found - begin) + pattern.size());
        current = found + 1;
      }
    };

    constexpr std::string_view kTelemetry = "\"k\":\"cur_level\",\"v\":\"";
    scan_text(kTelemetry, [&](std::size_t value_offset) {
      std::string level;
      for (std::size_t index = value_offset; index < bytes_read && level.size() <= 128; ++index) {
        const char character = static_cast<char>(world_buffer_[index]);
        if (character == '"') {
          break;
        }
        level.push_back(character);
      }
      if (IsValidLevelName(level)) {
        world_level_candidates_[level] += 10;
      }
    });

    constexpr std::string_view kLevelPath = "Data/Levels/";
    scan_text(kLevelPath, [&](std::size_t value_offset) {
      std::string level;
      std::size_t index = value_offset;
      for (; index < bytes_read && level.size() <= 128; ++index) {
        const char character = static_cast<char>(world_buffer_[index]);
        if (character == '/') {
          break;
        }
        level.push_back(character);
      }
      constexpr std::string_view kSuffix = "/Objects.level.bin";
      if (IsValidLevelName(level) && index + kSuffix.size() <= bytes_read &&
          std::memcmp(world_buffer_.data() + index, kSuffix.data(), kSuffix.size()) == 0) {
        world_level_candidates_[level] += 5;
      }
    });
  }

  if (world_cursor_ < stop) {
    return;
  }

  world_cache_.scan_cycle += 1;
  world_cache_.scanned_private_bytes = world_cycle_bytes_;
  world_cache_.scanned_private_regions = world_cycle_regions_;
  world_cache_.object_header_candidates = world_cycle_object_headers_;
  world_cache_.transform_candidates = world_cycle_transforms_;
  world_cache_.nearby_transform_count = world_cycle_nearby_count_;
  std::sort(world_cycle_nearby_.begin(), world_cycle_nearby_.end(),
            [](const auto& left, const auto& right) { return left.distance < right.distance; });
  if (world_cycle_nearby_.size() > kMaximumNearbyTransforms) {
    world_cycle_nearby_.resize(kMaximumNearbyTransforms);
  }
  world_cache_.nearby_transforms = world_cycle_nearby_;
  world_cache_.scan_status = "ready; starting next scan";

  if (!world_level_candidates_.empty()) {
    const auto selected = std::max_element(
        world_level_candidates_.begin(), world_level_candidates_.end(),
        [](const auto& left, const auto& right) { return left.second < right.second; });
    if (selected != world_level_candidates_.end()) {
      UpdateLevelAssets(selected->first, "world scan consensus");
    }
  }

  world_cursor_ = first_address;
  world_cycle_bytes_ = 0;
  world_cycle_regions_ = 0;
  world_cycle_object_headers_ = 0;
  world_cycle_transforms_ = 0;
  world_cycle_nearby_count_ = 0;
  world_cycle_nearby_.clear();
  world_level_candidates_.clear();
}

void GameState::UpdateCoordinateCandidates(std::uint64_t avatar, GameSnapshot& snapshot) {
  if (!coordinate_scan_enabled_.load(std::memory_order_acquire)) {
    return;
  }
  std::array<std::uint8_t, kCoordinateScanSize> bytes{};
  if (!ReadBytes(avatar, bytes.data(), bytes.size())) {
    return;
  }

  const std::size_t count = (bytes.size() - sizeof(float) * 3) / kCoordinateStride;
  if (coordinate_avatar_ != avatar || coordinate_previous_.size() != count) {
    coordinate_avatar_ = avatar;
    coordinate_initialized_ = false;
    coordinate_previous_.assign(count, {});
    coordinate_scores_.assign(count, 0.0F);
  }

  std::vector<CoordinateCandidate> candidates;
  candidates.reserve(64);
  for (std::size_t index = 0; index < count; ++index) {
    const std::size_t offset = index * kCoordinateStride;
    std::array<float, 3> value{};
    std::memcpy(value.data(), bytes.data() + offset, sizeof(float) * value.size());
    if (!IsPlausibleCoordinate(value)) {
      coordinate_scores_[index] *= 0.95F;
      coordinate_previous_[index] = value;
      continue;
    }

    if (coordinate_initialized_ && IsPlausibleCoordinate(coordinate_previous_[index])) {
      const float delta = Distance(value, coordinate_previous_[index]);
      if (delta > 0.0005F && delta < 50.0F) {
        coordinate_scores_[index] = std::min(100.0F, coordinate_scores_[index] + 1.5F);
      } else if (delta >= 50.0F) {
        coordinate_scores_[index] *= 0.25F;
      } else {
        coordinate_scores_[index] *= 0.997F;
      }
      const float largest = std::max({std::abs(value[0]), std::abs(value[1]), std::abs(value[2])});
      if (largest <= 1.25F) {
        coordinate_scores_[index] *= 0.98F;
      }
    }
    coordinate_previous_[index] = value;
    if (coordinate_scores_[index] > 0.25F) {
      candidates.push_back({static_cast<std::uint32_t>(offset), value, coordinate_scores_[index]});
    }
  }
  coordinate_initialized_ = true;

  std::sort(candidates.begin(), candidates.end(), [](const auto& left, const auto& right) {
    return left.score > right.score;
  });
  if (candidates.size() > 16) {
    candidates.resize(16);
  }
  snapshot.coordinate_candidates = std::move(candidates);
}

void GameState::Refresh() {
  GameSnapshot next;
  next.build = ReadBuildInfo();
  next.manager = Manager();
  next.status = next.manager == 0 ? "waiting for player manager" : "local avatar not found";
  for (std::size_t index = 0; index < next.slots.size(); ++index) {
    next.slots[index].index = static_cast<std::uint32_t>(index);
    next.slots[index].type = kSlotNames[index];
  }

  if (next.manager != 0 && next.build.supported) {
    for (std::uint32_t index = 0; index < 60; ++index) {
      const std::uint64_t avatar = next.manager + kAvatarArrayOffset + index * kAvatarStride;
      if (!PopulateAvatar(avatar, static_cast<std::int32_t>(index), next)) {
        continue;
      }
      break;
    }
  } else if (next.build.supported) {
    std::uint64_t avatar = discovered_avatar_.load(std::memory_order_acquire);
    if (avatar != 0 && !PopulateAvatar(avatar, -2, next)) {
      discovered_avatar_.store(0, std::memory_order_release);
      avatar = 0;
    }
    if (avatar == 0) {
      next.status = "scanning for local avatar";
      if (AdvanceAvatarDiscovery(avatar)) {
        discovered_avatar_.store(avatar, std::memory_order_release);
        PopulateAvatar(avatar, -2, next);
      }
    }
  } else if (!next.build.supported) {
    next.status = "unsupported Sky build";
  }

  if (next.valid) {
    PopulateTransform(next.avatar, next.transform);
    if (next.manager == 0 && next.avatar >= kAvatarArrayOffset) {
      const std::uint64_t slot_zero_manager = next.avatar - kAvatarArrayOffset;
      if (ValidateManager(slot_zero_manager, next.avatar)) {
        manager_.store(slot_zero_manager, std::memory_order_release);
        next.manager = slot_zero_manager;
        next.avatar_index = 0;
        next.status = "ready (manager inferred from local slot 0)";
        if (world_cache_.manager_source.empty()) {
          world_cache_.manager_source = "local-avatar slot-0 candidate";
        }
      }
    }
    for (std::uint32_t slot = 0; slot < next.slots.size(); ++slot) {
      auto& item = next.slots[slot];
      Read(next.outfit + 0x54 + slot * 4, item.base_id);
      Read(next.outfit + 0x1D48 + slot * 4, item.override_id);
      Read(next.outfit + 0x1D7C + slot * 4, item.override_flag);
      item.effective_id = item.override_flag != 0 ? item.override_id : item.base_id;
      item.resource_name = ResolveResourceName(next.outfit_database, slot, item.effective_id);
    }
    UpdateCoordinateCandidates(next.avatar, next);
    if (next.transform.valid) {
      AdvanceWorldScan(next);
    }
  }

  next.manager = Manager();
  PopulateRoom(next.manager, world_cache_);
  PopulateAutomation(world_cache_);
  next.world = world_cache_;
  next.local_effects = GetLocalEffectSnapshot();

  std::scoped_lock lock(snapshot_mutex_);
  snapshot_ = std::move(next);
}

GameSnapshot GameState::Snapshot() const {
  std::scoped_lock lock(snapshot_mutex_);
  return snapshot_;
}

bool GameState::TryGetPlayerPose(std::array<float, 3>& position,
                                 std::array<float, 3>& up) const {
  std::scoped_lock lock(snapshot_mutex_);
  if (!snapshot_.valid || !snapshot_.transform.valid) {
    return false;
  }
  position = snapshot_.transform.position;
  up = snapshot_.transform.up;
  return true;
}

bool GameState::ReadFloat4(std::uint64_t base, std::uint32_t offset,
                           std::array<float, 4>& output) const {
  return base != 0 && ReadBytes(base + offset, output.data(), sizeof(float) * output.size());
}

bool GameState::TeleportRelative(MoveDirection direction, float distance, std::string& error) {
  if (!std::isfinite(distance) || distance <= 0.0F || distance > 10000.0F) {
    error = u8"距离必须在 0 到 10000 之间";
    return false;
  }

  const GameSnapshot snapshot = Snapshot();
  TransformSnapshot transform;
  if (!snapshot.valid || snapshot.avatar == 0 || !PopulateTransform(snapshot.avatar, transform)) {
    error = u8"玩家位置尚未就绪";
    return false;
  }

  std::array<float, 3> axis{};
  float sign = 1.0F;
  switch (direction) {
    case MoveDirection::Forward:
      axis = transform.forward;
      break;
    case MoveDirection::Backward:
      axis = transform.forward;
      sign = -1.0F;
      break;
    case MoveDirection::Left:
      axis = transform.right;
      sign = -1.0F;
      break;
    case MoveDirection::Right:
      axis = transform.right;
      break;
    case MoveDirection::Up:
      axis = {0.0F, 1.0F, 0.0F};
      break;
    case MoveDirection::Down:
      axis = {0.0F, -1.0F, 0.0F};
      break;
  }

  if (direction == MoveDirection::Forward || direction == MoveDirection::Backward ||
      direction == MoveDirection::Left || direction == MoveDirection::Right) {
    axis[1] = 0.0F;
    const float horizontal_length = VectorLength(axis);
    if (!std::isfinite(horizontal_length) || horizontal_length < 0.001F) {
      error = u8"角色朝向无效";
      return false;
    }
    for (float& component : axis) {
      component /= horizontal_length;
    }
  }

  std::array<float, 3> target = transform.position;
  for (std::size_t index = 0; index < 3; ++index) {
    const float delta = axis[index] * distance * sign;
    target[index] += delta;
  }
  return WritePosition(transform, target, error);
}

bool GameState::WritePosition(const TransformSnapshot& transform,
                              const std::array<float, 3>& position, std::string& error) {
  if (!IsFiniteVector(position)) {
    error = u8"目标坐标无效";
    return false;
  }

  std::array<float, 4> primary{};
  std::array<float, 4> secondary{};
  if (!Read(transform.address, primary) || !Read(transform.address + 0x10, secondary)) {
    error = u8"读取位置失败";
    return false;
  }
  const auto original_primary = primary;
  const auto original_secondary = secondary;
  std::copy(position.begin(), position.end(), primary.begin());
  std::copy(position.begin(), position.end(), secondary.begin());

  if (!WriteBytes(transform.address, primary.data(), sizeof(primary)) ||
      !WriteBytes(transform.address + 0x10, secondary.data(), sizeof(secondary))) {
    WriteBytes(transform.address, original_primary.data(), sizeof(original_primary));
    WriteBytes(transform.address + 0x10, original_secondary.data(), sizeof(original_secondary));
    error = u8"写入位置失败，已恢复原坐标";
    return false;
  }

  {
    std::scoped_lock lock(snapshot_mutex_);
    if (snapshot_.transform.address == transform.address) {
      snapshot_.transform.position = position;
    }
  }
  error.clear();
  return true;
}

bool GameState::TeleportAbsolute(const std::array<float, 3>& position, std::string& error) {
  const GameSnapshot snapshot = Snapshot();
  TransformSnapshot transform;
  if (!snapshot.valid || snapshot.avatar == 0 || !PopulateTransform(snapshot.avatar, transform)) {
    error = u8"玩家位置尚未就绪";
    return false;
  }
  return WritePosition(transform, position, error);
}

void GameState::SetWaxLoopEnabled(bool enabled) {
  const bool previous = wax_loop_enabled_.exchange(enabled, std::memory_order_acq_rel);
  if (previous == enabled) {
    return;
  }
  std::scoped_lock lock(automation_mutex_);
  wax_visited_.clear();
  wax_automation_level_.clear();
  wax_target_index_ = 0;
  wax_next_teleport_at_ = std::chrono::steady_clock::now();
  wax_automation_status_ = enabled ? "waiting for level wax targets" : "disabled";
}

bool GameState::WaxLoopEnabled() const {
  return wax_loop_enabled_.load(std::memory_order_acquire);
}

void GameState::SetWaxLoopInterval(std::uint32_t interval_ms) {
  wax_loop_interval_ms_.store(std::clamp<std::uint32_t>(interval_ms, 100, 10000),
                              std::memory_order_release);
}

std::uint32_t GameState::WaxLoopInterval() const {
  return wax_loop_interval_ms_.load(std::memory_order_acquire);
}

void GameState::TickAutomation() {
  if (!WaxLoopEnabled()) {
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  const GameSnapshot snapshot = Snapshot();
  std::scoped_lock lock(automation_mutex_);
  if (now < wax_next_teleport_at_) {
    return;
  }
  if (!snapshot.valid || !snapshot.transform.valid) {
    wax_automation_status_ = "waiting for player transform";
    wax_next_teleport_at_ = now + std::chrono::seconds(1);
    return;
  }
  if (!snapshot.world.level_assets_valid || snapshot.world.wax_targets.empty()) {
    wax_automation_status_ = snapshot.world.level.empty() ? "waiting for current level"
                                                           : "no parsed wax targets";
    wax_next_teleport_at_ = now + std::chrono::seconds(1);
    return;
  }
  if (wax_automation_level_ != snapshot.world.level ||
      wax_visited_.size() != snapshot.world.wax_targets.size()) {
    wax_automation_level_ = snapshot.world.level;
    wax_visited_.assign(snapshot.world.wax_targets.size(), false);
    wax_target_index_ = 0;
  }

  bool has_unvisited = false;
  for (std::size_t index = 0; index < snapshot.world.wax_targets.size(); ++index) {
    if (snapshot.world.wax_targets[index].usable && !wax_visited_[index]) {
      has_unvisited = true;
      break;
    }
  }
  if (!has_unvisited) {
    std::fill(wax_visited_.begin(), wax_visited_.end(), false);
    wax_automation_status_ = "cycle complete; rescanning in 10 seconds";
    wax_next_teleport_at_ = now + std::chrono::seconds(10);
    return;
  }

  std::size_t selected = snapshot.world.wax_targets.size();
  float selected_distance = std::numeric_limits<float>::max();
  for (std::size_t index = 0; index < snapshot.world.wax_targets.size(); ++index) {
    const auto& target = snapshot.world.wax_targets[index];
    if (!target.usable || wax_visited_[index]) {
      continue;
    }
    const float distance = Distance(snapshot.transform.position, target.position);
    if (distance < selected_distance) {
      selected = index;
      selected_distance = distance;
    }
  }
  if (selected >= snapshot.world.wax_targets.size()) {
    wax_automation_status_ = "no usable wax targets";
    wax_next_teleport_at_ = now + std::chrono::seconds(1);
    return;
  }

  std::array<float, 3> destination = snapshot.world.wax_targets[selected].position;
  destination[1] += 0.75F;
  std::string error;
  if (TeleportAbsolute(destination, error)) {
    wax_visited_[selected] = true;
    wax_target_index_ = static_cast<std::uint32_t>(selected);
    ++wax_teleport_count_;
    char buffer[160]{};
    std::snprintf(buffer, sizeof(buffer), "target %zu/%zu at %.2f m", selected + 1,
                  snapshot.world.wax_targets.size(), static_cast<double>(selected_distance));
    wax_automation_status_ = buffer;
  } else {
    wax_automation_status_ = error;
  }
  wax_next_teleport_at_ =
      now + std::chrono::milliseconds(WaxLoopInterval());
}

GameState& GetGameState() {
  static GameState state;
  return state;
}

}  // namespace skyqoe
