#include "local_effects.h"

#include "game_state.h"
#include "outfit_changer.h"

#include <windows.h>

#include <MinHook.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace skyqoe {
namespace {

constexpr std::uint32_t kEmitterUpdateRva = 0x012320B0;
constexpr std::uint32_t kCreateEmitterRva = 0x01231FD0;
constexpr std::uint32_t kPoolCapacity = 3000;
constexpr std::uint32_t kPoolPauseThreshold = 2800;
constexpr std::uint32_t kPoolResumeThreshold = 2400;
constexpr std::array<std::uint8_t, 12> kEmitterUpdatePrefix = {
    0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x56, 0x57, 0x53,
};

// Static EmitterDef* slots used by this Sky build's verified local CreateEmitter call sites.
constexpr std::array<std::uint32_t, 104> kEmitterDefinitionSlots = {
    0x275CE50, 0x275CEA0, 0x275CEB0, 0x275CEC8, 0x275CF20, 0x275CF30, 0x275CF70,
    0x275CF80, 0x275CFD0, 0x275CFE0, 0x275CFE8, 0x275CFF8, 0x275D040, 0x275D0A8,
    0x275D0B0, 0x275D0B8, 0x275D0C0, 0x275D0F8, 0x275D100, 0x275D118, 0x275D128,
    0x275D130, 0x275D160, 0x275D1D8, 0x275D210, 0x275D238, 0x275D250, 0x275D268,
    0x275D300, 0x275D348, 0x275D350, 0x275D378, 0x275D380, 0x275D388, 0x275D390,
    0x275D398, 0x275D3A0, 0x275D3A8, 0x275D3C8, 0x275D3D0, 0x275D3E0, 0x275D448,
    0x275D460, 0x275D468, 0x275D478, 0x275D480, 0x275D4A0, 0x275D4A8, 0x275D4D0,
    0x275D5D0, 0x275D688, 0x275D6E0, 0x275D720, 0x275D728, 0x275D738, 0x275D768,
    0x275D7D0, 0x275D7D8, 0x275D878, 0x275D8A8, 0x275D8D0, 0x275D8E0, 0x275D8E8,
    0x275D8F0, 0x275D900, 0x275D958, 0x275D968, 0x275D978, 0x275D988, 0x275D990,
    0x275D9A8, 0x275DA38, 0x275DA48, 0x275DA50, 0x275DA70, 0x275DAC8, 0x275DAD0,
    0x275DB68, 0x275DB78, 0x275DBB8, 0x275DC18, 0x275DC20, 0x275DC28, 0x275DC30,
    0x275DC68, 0x275DC70, 0x275DC80, 0x275DCC8, 0x275DCD0, 0x275DCE0, 0x275DCE8,
    0x275DD00, 0x275DD80, 0x275DF20, 0x275DF30, 0x275DF48, 0x275DF50, 0x275DF68,
    0x275DFD8, 0x275DFE8, 0x275DFF0, 0x275E000, 0x275E018, 0x275E070,
};

struct alignas(16) Float4 {
  float x;
  float y;
  float z;
  float w;
};

using EmitterUpdateFn = void (*)(void*, void*, void*, void*, std::uint64_t, std::uint64_t,
                                 std::uint64_t, std::uint64_t);
using CreateEmitterFn = void* (*)(void*, const Float4*, const Float4*, void*, bool);

std::atomic<bool> g_enabled{false};
std::atomic<bool> g_shutting_down{false};
std::atomic<std::uint32_t> g_interval_ms{35};
std::mutex g_state_mutex;
LocalEffectSnapshot g_state;
std::uint8_t* g_module_base = nullptr;
EmitterUpdateFn g_original_update = nullptr;
CreateEmitterFn g_create_emitter = nullptr;
void* g_update_target = nullptr;
bool g_minhook_initialized = false;
bool g_hook_created = false;
std::uint64_t g_next_emit_ms = 0;
bool g_pool_paused = false;

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

bool SupportedBuild(std::uint8_t* module) {
  if (!module) {
    return false;
  }
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
    return false;
  }
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(module + dos->e_lfanew);
  return nt->Signature == IMAGE_NT_SIGNATURE &&
         nt->FileHeader.TimeDateStamp == kSupportedTimestamp &&
         nt->OptionalHeader.SizeOfImage == kSupportedImageSize;
}

std::uint32_t CountLoadedDefinitions() {
  if (!g_module_base) {
    return 0;
  }
  std::uint32_t count = 0;
  for (const std::uint32_t rva : kEmitterDefinitionSlots) {
    void* definition = nullptr;
    if (ReadCurrent(g_module_base + rva, definition) && definition != nullptr) {
      ++count;
    }
  }
  return count;
}

void SetStatusLocked(const char* text) {
  g_state.status = text ? text : "";
}

void ProcessEffect(void* emitter_barn) {
  if (!g_enabled.load(std::memory_order_acquire) ||
      g_shutting_down.load(std::memory_order_acquire) || !emitter_barn || !g_create_emitter) {
    return;
  }

  const std::uint64_t now = GetTickCount64();
  std::uint32_t definition_index = 0;
  {
    std::scoped_lock lock(g_state_mutex);
    g_state.emitter_barn = reinterpret_cast<std::uint64_t>(emitter_barn);
    if (now < g_next_emit_ms) {
      return;
    }
    g_next_emit_ms = now + g_interval_ms.load(std::memory_order_acquire);

    std::uint32_t pool_active = 0;
    void* free_head = nullptr;
    if (!ReadCurrent(static_cast<std::uint8_t*>(emitter_barn) + 0x2BBAB0, pool_active) ||
        !ReadCurrent(static_cast<std::uint8_t*>(emitter_barn) + 0x2BBA90, free_head)) {
      SetStatusLocked("unable to read EmitterBarn pool state");
      return;
    }
    g_state.pool_active = pool_active;
    if (g_pool_paused && pool_active <= kPoolResumeThreshold) {
      g_pool_paused = false;
    }
    if (pool_active >= kPoolPauseThreshold || !free_head || g_pool_paused) {
      g_pool_paused = true;
      SetStatusLocked("paused while the local emitter pool drains");
      return;
    }

    definition_index = g_state.next_index;
    g_state.next_index = (g_state.next_index + 1) % kEmitterDefinitionSlots.size();
    if (g_state.next_index == 0) {
      ++g_state.cycles;
    }
  }

  void* definition = nullptr;
  const std::uint32_t slot_rva = kEmitterDefinitionSlots[definition_index];
  if (!ReadCurrent(g_module_base + slot_rva, definition) || !definition) {
    std::scoped_lock lock(g_state_mutex);
    ++g_state.skipped;
    SetStatusLocked("skipped an unloaded local emitter definition");
    return;
  }

  std::array<float, 3> position{};
  std::array<float, 3> up{};
  if (!GetGameState().TryGetPlayerPose(position, up)) {
    std::scoped_lock lock(g_state_mutex);
    SetStatusLocked("waiting for the local player transform");
    return;
  }
  const float up_length =
      std::sqrt(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
  if (!std::isfinite(up_length) || up_length < 0.001F) {
    up = {0.0F, 1.0F, 0.0F};
  } else {
    for (float& component : up) {
      component /= up_length;
    }
  }
  const Float4 effect_position = {position[0], position[1] + 0.15F, position[2], 1.0F};
  const Float4 effect_direction = {up[0], up[1], up[2], 0.0F};
  void* emitter = g_create_emitter(emitter_barn, &effect_position, &effect_direction, definition,
                                   false);

  std::scoped_lock lock(g_state_mutex);
  g_state.last_definition = reinterpret_cast<std::uint64_t>(definition);
  g_state.last_emitter = reinterpret_cast<std::uint64_t>(emitter);
  if (emitter) {
    ++g_state.generated;
    char status[160]{};
    std::snprintf(status, sizeof(status), "definition %u/%zu (slot Sky.exe+0x%X)",
                  definition_index + 1, kEmitterDefinitionSlots.size(), slot_rva);
    SetStatusLocked(status);
  } else {
    ++g_state.skipped;
    SetStatusLocked("CreateEmitter returned null; waiting for pool capacity");
  }
}

void HookedEmitterUpdate(void* emitter_barn, void* argument2, void* argument3, void* argument4,
                         std::uint64_t argument5, std::uint64_t argument6,
                         std::uint64_t argument7, std::uint64_t argument8) {
  g_original_update(emitter_barn, argument2, argument3, argument4, argument5, argument6,
                    argument7, argument8);
  ProcessPendingOutfitChange();
  ProcessEffect(emitter_barn);
}

}  // namespace

bool InitializeLocalEffects() {
  std::scoped_lock lock(g_state_mutex);
  if (g_state.hook_installed) {
    return true;
  }
  g_state.catalog_count = static_cast<std::uint32_t>(kEmitterDefinitionSlots.size());
  g_state.pool_capacity = kPoolCapacity;
  g_module_base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
  g_state.supported = SupportedBuild(g_module_base);
  if (!g_state.supported) {
    SetStatusLocked("unsupported Sky build; local effects disabled");
    return false;
  }

  g_update_target = g_module_base + kEmitterUpdateRva;
  g_create_emitter = reinterpret_cast<CreateEmitterFn>(g_module_base + kCreateEmitterRva);
  std::array<std::uint8_t, kEmitterUpdatePrefix.size()> prefix{};
  if (!ReadCurrent(g_update_target, prefix) || prefix != kEmitterUpdatePrefix) {
    SetStatusLocked("EmitterBarn update signature mismatch");
    return false;
  }

  MH_STATUS status = MH_Initialize();
  if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
    SetStatusLocked(MH_StatusToString(status));
    return false;
  }
  g_minhook_initialized = status == MH_OK;
  status = MH_CreateHook(g_update_target, reinterpret_cast<void*>(&HookedEmitterUpdate),
                         reinterpret_cast<void**>(&g_original_update));
  if (status != MH_OK) {
    SetStatusLocked(MH_StatusToString(status));
    if (g_minhook_initialized) {
      MH_Uninitialize();
      g_minhook_initialized = false;
    }
    return false;
  }
  g_hook_created = true;
  status = MH_EnableHook(g_update_target);
  if (status != MH_OK) {
    SetStatusLocked(MH_StatusToString(status));
    MH_RemoveHook(g_update_target);
    g_hook_created = false;
    if (g_minhook_initialized) {
      MH_Uninitialize();
      g_minhook_initialized = false;
    }
    return false;
  }

  g_state.loaded_count = CountLoadedDefinitions();
  g_state.hook_installed = true;
  SetOutfitGameThreadReady(true);
  char status_text[128]{};
  std::snprintf(status_text, sizeof(status_text), "ready; %u/%zu local definitions loaded",
                g_state.loaded_count, kEmitterDefinitionSlots.size());
  SetStatusLocked(status_text);
  return true;
}

void ShutdownLocalEffects() {
  g_shutting_down.store(true, std::memory_order_release);
  g_enabled.store(false, std::memory_order_release);
  SetOutfitGameThreadReady(false);
  if (g_hook_created && g_update_target) {
    MH_DisableHook(g_update_target);
    MH_RemoveHook(g_update_target);
  }
  if (g_minhook_initialized) {
    MH_Uninitialize();
  }
  std::scoped_lock lock(g_state_mutex);
  g_state.enabled = false;
  g_state.hook_installed = false;
  SetStatusLocked("shut down");
  g_original_update = nullptr;
  g_create_emitter = nullptr;
  g_update_target = nullptr;
  g_minhook_initialized = false;
  g_hook_created = false;
}

void SetLocalEffectLoopEnabled(bool enabled) {
  std::scoped_lock lock(g_state_mutex);
  if (enabled && !g_state.hook_installed) {
    g_enabled.store(false, std::memory_order_release);
    g_state.enabled = false;
    SetStatusLocked("local effect hook is not ready");
    return;
  }
  g_pool_paused = false;
  g_next_emit_ms = GetTickCount64();
  g_state.next_index = 0;
  g_state.enabled = enabled;
  g_enabled.store(enabled, std::memory_order_release);
  if (!enabled) {
    SetStatusLocked("disabled");
  } else {
    SetStatusLocked("starting local effect cycle");
  }
}

bool LocalEffectLoopEnabled() {
  return g_enabled.load(std::memory_order_acquire);
}

void SetLocalEffectInterval(std::uint32_t interval_ms) {
  g_interval_ms.store(std::clamp<std::uint32_t>(interval_ms, 16, 5000),
                      std::memory_order_release);
}

std::uint32_t LocalEffectInterval() {
  return g_interval_ms.load(std::memory_order_acquire);
}

LocalEffectSnapshot GetLocalEffectSnapshot() {
  std::scoped_lock lock(g_state_mutex);
  LocalEffectSnapshot result = g_state;
  result.enabled = g_enabled.load(std::memory_order_acquire);
  result.interval_ms = g_interval_ms.load(std::memory_order_acquire);
  return result;
}

}  // namespace skyqoe
