#include "game_state.h"
#include "local_effects.h"
#include "overlay.h"
#include "snapshot_json.h"

#include <windows.h>

#include <cstdint>
#include <string>

extern "C" __declspec(dllexport) std::uint64_t __stdcall SkyQoE_SetManager(
    std::uint64_t manager) {
  skyqoe::GetGameState().SetManager(manager);
  return manager;
}

extern "C" __declspec(dllexport) std::uint64_t __stdcall SkyQoE_GetManager(std::uint64_t) {
  return skyqoe::GetGameState().Manager();
}

extern "C" __declspec(dllexport) std::uint64_t __stdcall SkyQoE_GetAvatar(std::uint64_t) {
  return skyqoe::GetGameState().Snapshot().avatar;
}

extern "C" __declspec(dllexport) std::uint64_t __stdcall SkyQoE_GetOutfit(std::uint64_t) {
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

extern "C" __declspec(dllexport) std::uint64_t __stdcall SkyQoE_SetWaxLoopEnabled(
    std::uint64_t enabled) {
  skyqoe::GetGameState().SetWaxLoopEnabled(enabled != 0);
  return skyqoe::GetGameState().WaxLoopEnabled() ? 1 : 0;
}

extern "C" __declspec(dllexport) std::uint64_t __stdcall SkyQoE_SetWaxLoopInterval(
    std::uint64_t interval_ms) {
  skyqoe::GetGameState().SetWaxLoopInterval(static_cast<std::uint32_t>(interval_ms));
  return skyqoe::GetGameState().WaxLoopInterval();
}

extern "C" __declspec(dllexport) std::uint64_t __stdcall SkyQoE_SetLocalEffectLoopEnabled(
    std::uint64_t enabled) {
  skyqoe::SetLocalEffectLoopEnabled(enabled != 0);
  return skyqoe::LocalEffectLoopEnabled() ? 1 : 0;
}

extern "C" __declspec(dllexport) std::uint64_t __stdcall SkyQoE_SetLocalEffectInterval(
    std::uint64_t interval_ms) {
  skyqoe::SetLocalEffectInterval(static_cast<std::uint32_t>(interval_ms));
  return skyqoe::LocalEffectInterval();
}

extern "C" __declspec(dllexport) std::uint64_t __stdcall SkyQoE_CopySnapshotJson(
    char* output, std::uint64_t capacity) {
  try {
    const std::string json = skyqoe::BuildSnapshotJson();
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

extern "C" __declspec(dllexport) std::uint64_t __stdcall SkyQoE_ToggleMenu(std::uint64_t) {
  skyqoe::ToggleMenu();
  return skyqoe::IsMenuVisible() ? 1 : 0;
}

extern "C" __declspec(dllexport) std::uint64_t __stdcall SkyQoE_RequestShutdown(std::uint64_t) {
  skyqoe::RequestShutdown();
  return 1;
}

extern "C" __declspec(dllexport) std::uint64_t __stdcall SkyQoE_GetVersion(std::uint64_t) {
  return 0x00030000;
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
