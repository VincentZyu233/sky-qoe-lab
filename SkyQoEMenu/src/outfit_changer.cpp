#include "outfit_changer.h"

#include "game_state.h"

#include <windows.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <limits>
#include <mutex>
#include <string_view>
#include <utility>
#include <vector>

namespace skyqoe {
namespace {

constexpr std::uint32_t kSetOutfitByNameRva = 0x015FD3C0;
constexpr std::array<std::uint8_t, 13> kSetOutfitByNamePrefix = {
    0x55, 0x41, 0x56, 0x56, 0x57, 0x53, 0x48,
    0x81, 0xEC, 0x90, 0x09, 0x00, 0x00,
};
constexpr std::array<std::string_view, 10> kJsonTypes = {
    "body", "wing", "hair", "mask", "neck",
    "feet", "horn", "face", "prop", "hat",
};

using SetOutfitByNameFn = void (*)(void*, std::uint32_t, const char*, std::uint32_t, bool);

struct PendingOutfitChange {
  std::uint32_t slot = 0;
  std::string name;
  std::uint32_t expected_id = 0;
};

std::array<std::vector<OutfitDefinition>, 10> g_catalog;
std::mutex g_state_mutex;
OutfitChangerSnapshot g_state;
PendingOutfitChange g_pending;
SetOutfitByNameFn g_set_outfit_by_name = nullptr;
bool g_shutting_down = false;

bool ReadCurrent(const void* address, void* output, std::size_t size) {
  if (!address || !output || size == 0) {
    return false;
  }
  SIZE_T read = 0;
  return ReadProcessMemory(GetCurrentProcess(), address, output, size, &read) != FALSE &&
         read == size;
}

template <typename T>
bool ReadCurrent(const void* address, T& output) {
  return ReadCurrent(address, &output, sizeof(output));
}

std::uint32_t OutfitId(std::string_view name) {
  std::uint32_t hash = 0x811C9DC5U;
  for (const unsigned char character : name) {
    hash ^= character;
    hash *= 0x01000193U;
  }
  return hash;
}

std::string Utf8(const std::wstring& value) {
  if (value.empty()) {
    return {};
  }
  const int required = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                           static_cast<int>(value.size()), nullptr, 0,
                                           nullptr, nullptr);
  if (required <= 0) {
    return {};
  }
  std::string output(static_cast<std::size_t>(required), '\0');
  WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                      output.data(), required, nullptr, nullptr);
  return output;
}

bool BuildResourcePath(std::wstring& path) {
  std::vector<wchar_t> buffer(32768);
  const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                          static_cast<DWORD>(buffer.size()));
  if (length == 0 || length >= buffer.size()) {
    return false;
  }
  path.assign(buffer.data(), length);
  const std::size_t separator = path.find_last_of(L"\\/");
  if (separator == std::wstring::npos) {
    return false;
  }
  path.resize(separator);
  path += L"\\data\\assets\\initial\\Data\\Resources\\OutfitDefs.json";
  return true;
}

bool ReadFileBytes(const std::wstring& path, std::vector<char>& bytes, std::string& error) {
  const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    error = "unable to open OutfitDefs.json (Windows error " +
            std::to_string(GetLastError()) + ")";
    return false;
  }

  LARGE_INTEGER size{};
  if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
      size.QuadPart > 32LL * 1024LL * 1024LL) {
    const DWORD code = GetLastError();
    CloseHandle(file);
    error = "invalid OutfitDefs.json size (Windows error " + std::to_string(code) + ")";
    return false;
  }

  bytes.resize(static_cast<std::size_t>(size.QuadPart));
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const DWORD chunk = static_cast<DWORD>(
        std::min<std::size_t>(bytes.size() - offset, std::numeric_limits<DWORD>::max()));
    DWORD read = 0;
    if (!ReadFile(file, bytes.data() + offset, chunk, &read, nullptr) || read == 0) {
      const DWORD code = GetLastError();
      CloseHandle(file);
      error = "unable to read OutfitDefs.json (Windows error " + std::to_string(code) + ")";
      return false;
    }
    offset += read;
  }
  CloseHandle(file);
  return true;
}

std::size_t SlotForType(std::string_view type) {
  const auto found = std::find(kJsonTypes.begin(), kJsonTypes.end(), type);
  return found == kJsonTypes.end() ? kJsonTypes.size()
                                  : static_cast<std::size_t>(found - kJsonTypes.begin());
}

bool ParseCatalog(const std::vector<char>& bytes, std::string& error) {
  const nlohmann::json root = nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, false);
  if (root.is_discarded() || !root.is_array()) {
    error = "OutfitDefs.json is not a valid JSON array";
    return false;
  }

  std::array<std::vector<OutfitDefinition>, 10> catalog;
  for (const auto& item : root) {
    if (!item.is_object()) {
      continue;
    }
    const auto name_item = item.find("name");
    const auto type_item = item.find("type");
    if (name_item == item.end() || !name_item->is_string() ||
        type_item == item.end() || !type_item->is_string()) {
      continue;
    }
    const std::string name = name_item->get<std::string>();
    const std::size_t slot = SlotForType(type_item->get_ref<const std::string&>());
    if (name.empty() || slot >= catalog.size()) {
      continue;
    }

    OutfitDefinition definition;
    definition.id = OutfitId(name);
    definition.name = name;
    if (const auto season = item.find("season");
        season != item.end() && season->is_string()) {
      definition.season = season->get<std::string>();
    }
    if (const auto in_closet = item.find("inCloset");
        in_closet != item.end() && in_closet->is_boolean()) {
      definition.in_closet = in_closet->get<bool>();
    }
    if (const auto is_default = item.find("isDefault");
        is_default != item.end() && is_default->is_boolean()) {
      definition.is_default = is_default->get<bool>();
    }

    definition.list_label = "#" + std::to_string(catalog[slot].size() + 1) + "  " +
                            definition.name + "  |  ID " + std::to_string(definition.id) +
                            "  |  season " +
                            (definition.season.empty() ? "-" : definition.season);
    if (!definition.in_closet) {
      definition.list_label += "  |  outside closet";
    }
    if (definition.is_default) {
      definition.list_label += "  |  default";
    }
    catalog[slot].push_back(std::move(definition));
  }

  const bool complete = std::all_of(catalog.begin(), catalog.end(),
                                    [](const auto& slot) { return !slot.empty(); });
  if (!complete) {
    error = "OutfitDefs.json did not contain all ten outfit categories";
    return false;
  }
  g_catalog = std::move(catalog);
  return true;
}

void CompleteWithFailure(const PendingOutfitChange& request, const std::string& status) {
  std::scoped_lock lock(g_state_mutex);
  ++g_state.failed;
  g_state.last_slot = request.slot;
  g_state.last_name = request.name;
  g_state.status = status;
}

}  // namespace

bool InitializeOutfitChanger() {
  std::scoped_lock lock(g_state_mutex);
  if (g_state.catalog_ready) {
    return true;
  }
  g_shutting_down = false;

  const auto* module = reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr));
  const BuildInfo build = GetGameState().Snapshot().build;
  g_state.supported = module != nullptr && build.supported;
  if (!g_state.supported) {
    g_state.status = "unsupported Sky build; outfit changer disabled";
    return false;
  }

  std::array<std::uint8_t, kSetOutfitByNamePrefix.size()> prefix{};
  const void* target = module + kSetOutfitByNameRva;
  if (!ReadCurrent(target, prefix) || prefix != kSetOutfitByNamePrefix) {
    g_state.status = "SetOutfitByName signature mismatch";
    return false;
  }

  std::wstring resource_path;
  if (!BuildResourcePath(resource_path)) {
    g_state.status = "unable to locate the Sky installation directory";
    return false;
  }
  g_state.resource_path = Utf8(resource_path);

  std::vector<char> bytes;
  std::string error;
  if (!ReadFileBytes(resource_path, bytes, error) || !ParseCatalog(bytes, error)) {
    g_state.status = error;
    return false;
  }

  g_state.total_count = 0;
  for (std::size_t slot = 0; slot < g_catalog.size(); ++slot) {
    g_state.slot_counts[slot] = static_cast<std::uint32_t>(g_catalog[slot].size());
    g_state.total_count += g_state.slot_counts[slot];
  }
  g_set_outfit_by_name = reinterpret_cast<SetOutfitByNameFn>(
      const_cast<std::uint8_t*>(module) + kSetOutfitByNameRva);
  g_state.catalog_ready = true;
  g_state.status = "catalog ready; waiting for the game-thread hook";
  return true;
}

void ShutdownOutfitChanger() {
  std::scoped_lock lock(g_state_mutex);
  g_shutting_down = true;
  g_state.game_thread_ready = false;
  g_state.pending = false;
  g_pending = {};
  g_set_outfit_by_name = nullptr;
  g_state.status = "shut down";
}

void SetOutfitGameThreadReady(bool ready) {
  std::scoped_lock lock(g_state_mutex);
  g_state.game_thread_ready = ready && g_state.catalog_ready && !g_shutting_down;
  if (g_state.game_thread_ready) {
    g_state.status = "ready";
  } else if (!g_shutting_down && g_state.catalog_ready) {
    g_state.status = "game-thread hook unavailable";
  }
}

bool QueueOutfitChange(std::uint32_t slot, const std::string& resource_name,
                       std::string& error) {
  const GameSnapshot snapshot = GetGameState().Snapshot();
  std::scoped_lock lock(g_state_mutex);
  if (g_shutting_down || !g_state.catalog_ready || !g_state.game_thread_ready ||
      !g_set_outfit_by_name) {
    error = "更衣功能尚未就绪";
    return false;
  }
  if (!snapshot.valid || snapshot.outfit == 0) {
    error = "本地玩家穿搭尚未就绪";
    return false;
  }
  if (slot >= g_catalog.size()) {
    error = "无效的服饰槽位";
    return false;
  }
  const auto found = std::find_if(g_catalog[slot].begin(), g_catalog[slot].end(),
                                  [&](const OutfitDefinition& definition) {
                                    return definition.name == resource_name;
                                  });
  if (found == g_catalog[slot].end()) {
    error = "资源不属于所选服饰槽位";
    return false;
  }
  if (g_state.pending) {
    error = "上一项换装请求仍在等待游戏线程";
    return false;
  }

  g_pending = PendingOutfitChange{slot, resource_name, found->id};
  g_state.pending = true;
  g_state.pending_slot = slot;
  g_state.pending_name = resource_name;
  g_state.status = "change queued for the game thread";
  return true;
}

void ProcessPendingOutfitChange() {
  PendingOutfitChange request;
  {
    std::scoped_lock lock(g_state_mutex);
    if (g_shutting_down || !g_state.pending || !g_state.game_thread_ready ||
        !g_set_outfit_by_name) {
      return;
    }
    request = g_pending;
    g_state.pending = false;
    g_state.pending_name.clear();
  }

  const GameSnapshot snapshot = GetGameState().Snapshot();
  if (!snapshot.build.supported || !snapshot.valid || snapshot.outfit == 0) {
    CompleteWithFailure(request, "player outfit became unavailable before the request ran");
    return;
  }

  std::uint64_t reverse_avatar = 0;
  if (!ReadCurrent(reinterpret_cast<const std::uint8_t*>(snapshot.outfit) + 8,
                   reverse_avatar) ||
      reverse_avatar != snapshot.avatar) {
    CompleteWithFailure(request, "outfit pointer validation failed on the game thread");
    return;
  }

  g_set_outfit_by_name(reinterpret_cast<void*>(snapshot.outfit), request.slot,
                       request.name.c_str(), 0, false);

  std::uint32_t base_id = 0;
  const bool applied = ReadCurrent(
                           reinterpret_cast<const std::uint8_t*>(snapshot.outfit) +
                               0x54 + request.slot * sizeof(std::uint32_t),
                           base_id) &&
                       base_id == request.expected_id;
  std::scoped_lock lock(g_state_mutex);
  g_state.last_slot = request.slot;
  g_state.last_name = request.name;
  if (applied) {
    ++g_state.applied;
    g_state.status = "applied on the game thread";
  } else {
    ++g_state.failed;
    char status[128]{};
    std::snprintf(status, sizeof(status),
                  "SetOutfitByName returned but base ID is %u (expected %u)", base_id,
                  request.expected_id);
    g_state.status = status;
  }
}

OutfitChangerSnapshot GetOutfitChangerSnapshot() {
  std::scoped_lock lock(g_state_mutex);
  return g_state;
}

const std::array<std::vector<OutfitDefinition>, 10>& GetOutfitCatalog() {
  return g_catalog;
}

}  // namespace skyqoe
