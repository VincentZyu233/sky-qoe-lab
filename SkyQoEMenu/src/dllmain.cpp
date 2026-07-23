#include "game_state.h"
#include "overlay.h"

#include <windows.h>

#include <cstdio>
#include <cstdint>
#include <string>

namespace {

void AppendJsonString(std::string& output, const std::string& value) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  output.push_back('"');
  for (const unsigned char character : value) {
    switch (character) {
      case '"':
        output += "\\\"";
        break;
      case '\\':
        output += "\\\\";
        break;
      case '\b':
        output += "\\b";
        break;
      case '\f':
        output += "\\f";
        break;
      case '\n':
        output += "\\n";
        break;
      case '\r':
        output += "\\r";
        break;
      case '\t':
        output += "\\t";
        break;
      default:
        if (character < 0x20U) {
          output += "\\u00";
          output.push_back(kHex[character >> 4U]);
          output.push_back(kHex[character & 0x0FU]);
        } else {
          output.push_back(static_cast<char>(character));
        }
        break;
    }
  }
  output.push_back('"');
}

void AppendHexAddress(std::string& output, std::uint64_t value) {
  char buffer[24]{};
  std::snprintf(buffer, sizeof(buffer), "0x%llX", static_cast<unsigned long long>(value));
  AppendJsonString(output, buffer);
}

void AppendFloat(std::string& output, float value) {
  char buffer[32]{};
  std::snprintf(buffer, sizeof(buffer), "%.7g", static_cast<double>(value));
  output += buffer;
}

std::string BuildSnapshotJson() {
  const skyqoe::GameSnapshot snapshot = skyqoe::GetGameState().Snapshot();
  std::string output;
  output.reserve(4096);
  output += "{\"version\":\"0.2.0\",\"valid\":";
  output += snapshot.valid ? "true" : "false";
  output += ",\"status\":";
  AppendJsonString(output, snapshot.status);
  output += ",\"build\":{\"supported\":";
  output += snapshot.build.supported ? "true" : "false";
  output += ",\"moduleBase\":";
  AppendHexAddress(output, snapshot.build.module_base);
  output += ",\"timestamp\":" + std::to_string(snapshot.build.timestamp);
  output += ",\"imageSize\":" + std::to_string(snapshot.build.image_size) + "}";
  output += ",\"manager\":";
  AppendHexAddress(output, snapshot.manager);
  output += ",\"avatar\":";
  AppendHexAddress(output, snapshot.avatar);
  output += ",\"outfit\":";
  AppendHexAddress(output, snapshot.outfit);
  output += ",\"database\":";
  AppendHexAddress(output, snapshot.outfit_database);
  output += ",\"avatarIndex\":" + std::to_string(snapshot.avatar_index);
  output += ",\"avatarActive\":" + std::to_string(snapshot.avatar_active);
  output += ",\"avatarFlags\":" + std::to_string(snapshot.avatar_flags);
  output += ",\"transform\":{\"valid\":";
  output += snapshot.transform.valid ? "true" : "false";
  output += ",\"address\":";
  AppendHexAddress(output, snapshot.transform.address);
  output += ",\"position\":[";
  AppendFloat(output, snapshot.transform.position[0]);
  output.push_back(',');
  AppendFloat(output, snapshot.transform.position[1]);
  output.push_back(',');
  AppendFloat(output, snapshot.transform.position[2]);
  output += "],\"right\":[";
  AppendFloat(output, snapshot.transform.right[0]);
  output.push_back(',');
  AppendFloat(output, snapshot.transform.right[1]);
  output.push_back(',');
  AppendFloat(output, snapshot.transform.right[2]);
  output += "],\"up\":[";
  AppendFloat(output, snapshot.transform.up[0]);
  output.push_back(',');
  AppendFloat(output, snapshot.transform.up[1]);
  output.push_back(',');
  AppendFloat(output, snapshot.transform.up[2]);
  output += "],\"forward\":[";
  AppendFloat(output, snapshot.transform.forward[0]);
  output.push_back(',');
  AppendFloat(output, snapshot.transform.forward[1]);
  output.push_back(',');
  AppendFloat(output, snapshot.transform.forward[2]);
  output += "]}";
  output += ",\"slots\":[";
  for (std::size_t index = 0; index < snapshot.slots.size(); ++index) {
    const auto& slot = snapshot.slots[index];
    if (index != 0) {
      output.push_back(',');
    }
    output += "{\"index\":" + std::to_string(slot.index) + ",\"type\":";
    AppendJsonString(output, slot.type);
    output += ",\"baseId\":" + std::to_string(slot.base_id);
    output += ",\"overrideId\":" + std::to_string(slot.override_id);
    output += ",\"overrideFlag\":" + std::to_string(slot.override_flag);
    output += ",\"effectiveId\":" + std::to_string(slot.effective_id);
    output += ",\"resourceName\":";
    AppendJsonString(output, slot.resource_name);
    output.push_back('}');
  }
  output += "],\"coordinateCandidates\":[";
  for (std::size_t index = 0; index < snapshot.coordinate_candidates.size(); ++index) {
    const auto& candidate = snapshot.coordinate_candidates[index];
    if (index != 0) {
      output.push_back(',');
    }
    output += "{\"offset\":" + std::to_string(candidate.offset) + ",\"value\":[";
    AppendFloat(output, candidate.value[0]);
    output.push_back(',');
    AppendFloat(output, candidate.value[1]);
    output.push_back(',');
    AppendFloat(output, candidate.value[2]);
    output += "],\"score\":";
    AppendFloat(output, candidate.score);
    output.push_back('}');
  }
  output += "]}";
  return output;
}

}  // namespace

extern "C" __declspec(dllexport) std::uint64_t __stdcall SkyQoE_SetManager(
    std::uint64_t manager) {
  skyqoe::GetGameState().SetManager(manager);
  return manager;
}

extern "C" __declspec(dllexport) std::uint64_t __stdcall SkyQoE_GetManager(
    std::uint64_t) {
  return skyqoe::GetGameState().Manager();
}

extern "C" __declspec(dllexport) std::uint64_t __stdcall SkyQoE_GetAvatar(
    std::uint64_t) {
  return skyqoe::GetGameState().Snapshot().avatar;
}

extern "C" __declspec(dllexport) std::uint64_t __stdcall SkyQoE_GetOutfit(
    std::uint64_t) {
  return skyqoe::GetGameState().Snapshot().outfit;
}

extern "C" __declspec(dllexport) std::uint64_t __stdcall SkyQoE_GetOutfitDatabase(
    std::uint64_t) {
  return skyqoe::GetGameState().Snapshot().outfit_database;
}

extern "C" __declspec(dllexport) std::uint64_t __stdcall SkyQoE_GetEffectiveOutfitId(
    std::uint64_t slot) {
  const skyqoe::GameSnapshot snapshot = skyqoe::GetGameState().Snapshot();
  return slot < snapshot.slots.size() ? snapshot.slots[static_cast<std::size_t>(slot)].effective_id
                                      : 0;
}

extern "C" __declspec(dllexport) std::uint64_t __stdcall SkyQoE_TeleportRelative(
    std::uint64_t direction, std::uint64_t distance_millimeters) {
  if (direction > static_cast<std::uint64_t>(skyqoe::MoveDirection::Down) ||
      distance_millimeters == 0 || distance_millimeters > 10000000) {
    return 0;
  }
  std::string error;
  const float distance = static_cast<float>(distance_millimeters) / 1000.0F;
  return skyqoe::GetGameState().TeleportRelative(
             static_cast<skyqoe::MoveDirection>(direction), distance, error)
             ? 1
             : 0;
}

extern "C" __declspec(dllexport) std::uint64_t __stdcall SkyQoE_CopySnapshotJson(
    char* output, std::uint64_t capacity) {
  try {
    const std::string json = BuildSnapshotJson();
    const std::uint64_t required = json.size() + 1;
    if (output == nullptr || capacity < required) {
      return required;
    }
    SIZE_T written = 0;
    if (!WriteProcessMemory(GetCurrentProcess(), output, json.c_str(), required, &written) ||
        written != required) {
      return 0;
    }
    return required;
  } catch (...) {
    return 0;
  }
}

extern "C" __declspec(dllexport) std::uint64_t __stdcall SkyQoE_ToggleMenu(
    std::uint64_t) {
  skyqoe::ToggleMenu();
  return skyqoe::IsMenuVisible() ? 1 : 0;
}

extern "C" __declspec(dllexport) std::uint64_t __stdcall SkyQoE_RequestShutdown(
    std::uint64_t) {
  skyqoe::RequestShutdown();
  return 1;
}

extern "C" __declspec(dllexport) std::uint64_t __stdcall SkyQoE_GetVersion(
    std::uint64_t) {
  return 0x00020000;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(module);
    const HANDLE thread = CreateThread(nullptr, 0, skyqoe::OverlayThread, module, 0, nullptr);
    if (thread) {
      CloseHandle(thread);
    }
  } else if (reason == DLL_PROCESS_DETACH) {
    skyqoe::RequestShutdown();
  }
  return TRUE;
}
