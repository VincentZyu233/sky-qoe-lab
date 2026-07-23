#include "game_state.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

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

}  // namespace

GameState::GameState() : discovery_buffer_(kDiscoveryChunkSize) {
  snapshot_.build = ReadBuildInfo();
  snapshot_.status = "waiting for player manager";
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
    for (std::uint32_t slot = 0; slot < next.slots.size(); ++slot) {
      auto& item = next.slots[slot];
      Read(next.outfit + 0x54 + slot * 4, item.base_id);
      Read(next.outfit + 0x1D48 + slot * 4, item.override_id);
      Read(next.outfit + 0x1D7C + slot * 4, item.override_flag);
      item.effective_id = item.override_flag != 0 ? item.override_id : item.base_id;
      item.resource_name = ResolveResourceName(next.outfit_database, slot, item.effective_id);
    }
    UpdateCoordinateCandidates(next.avatar, next);
  }

  std::scoped_lock lock(snapshot_mutex_);
  snapshot_ = std::move(next);
}

GameSnapshot GameState::Snapshot() const {
  std::scoped_lock lock(snapshot_mutex_);
  return snapshot_;
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

  std::array<float, 4> primary{};
  std::array<float, 4> secondary{};
  if (!Read(transform.address, primary) || !Read(transform.address + 0x10, secondary)) {
    error = u8"读取位置失败";
    return false;
  }
  const auto original_primary = primary;
  const auto original_secondary = secondary;
  for (std::size_t index = 0; index < 3; ++index) {
    const float delta = axis[index] * distance * sign;
    primary[index] += delta;
    secondary[index] += delta;
  }
  if (!IsFiniteVector({primary[0], primary[1], primary[2]})) {
    error = u8"目标坐标无效";
    return false;
  }

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
      snapshot_.transform.position = {primary[0], primary[1], primary[2]};
    }
  }
  error.clear();
  return true;
}

GameState& GetGameState() {
  static GameState state;
  return state;
}

}  // namespace skyqoe
