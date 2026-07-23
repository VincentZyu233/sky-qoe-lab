#pragma once

#include <cstdint>
#include <string>

namespace skyqoe {

struct LocalEffectSnapshot {
  bool supported = false;
  bool hook_installed = false;
  bool enabled = false;
  std::uint32_t interval_ms = 35;
  std::uint32_t catalog_count = 0;
  std::uint32_t loaded_count = 0;
  std::uint32_t next_index = 0;
  std::uint32_t pool_active = 0;
  std::uint32_t pool_capacity = 3000;
  std::uint64_t generated = 0;
  std::uint64_t cycles = 0;
  std::uint64_t skipped = 0;
  std::uint64_t emitter_barn = 0;
  std::uint64_t last_definition = 0;
  std::uint64_t last_emitter = 0;
  std::string status = "not initialized";
};

bool InitializeLocalEffects();
void ShutdownLocalEffects();
void SetLocalEffectLoopEnabled(bool enabled);
bool LocalEffectLoopEnabled();
void SetLocalEffectInterval(std::uint32_t interval_ms);
std::uint32_t LocalEffectInterval();
LocalEffectSnapshot GetLocalEffectSnapshot();

}  // namespace skyqoe
