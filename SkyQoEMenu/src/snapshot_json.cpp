#include "snapshot_json.h"

#include "game_state.h"
#include "outfit_changer.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace skyqoe {
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

void AppendAddress(std::string& output, std::uint64_t value) {
  char buffer[24]{};
  std::snprintf(buffer, sizeof(buffer), "0x%llX", static_cast<unsigned long long>(value));
  AppendJsonString(output, buffer);
}

void AppendFloat(std::string& output, float value) {
  if (!std::isfinite(value)) {
    output += "null";
    return;
  }
  char buffer[32]{};
  std::snprintf(buffer, sizeof(buffer), "%.7g", static_cast<double>(value));
  output += buffer;
}

void AppendVector3(std::string& output, const std::array<float, 3>& value) {
  output.push_back('[');
  AppendFloat(output, value[0]);
  output.push_back(',');
  AppendFloat(output, value[1]);
  output.push_back(',');
  AppendFloat(output, value[2]);
  output.push_back(']');
}

void AppendBuild(std::string& output, const BuildInfo& build) {
  output += "{\"supported\":";
  output += build.supported ? "true" : "false";
  output += ",\"moduleBase\":";
  AppendAddress(output, build.module_base);
  output += ",\"timestamp\":" + std::to_string(build.timestamp);
  output += ",\"imageSize\":" + std::to_string(build.image_size) + "}";
}

void AppendTransform(std::string& output, const TransformSnapshot& transform) {
  output += "{\"valid\":";
  output += transform.valid ? "true" : "false";
  output += ",\"address\":";
  AppendAddress(output, transform.address);
  output += ",\"position\":";
  AppendVector3(output, transform.position);
  output += ",\"right\":";
  AppendVector3(output, transform.right);
  output += ",\"up\":";
  AppendVector3(output, transform.up);
  output += ",\"forward\":";
  AppendVector3(output, transform.forward);
  output.push_back('}');
}

void AppendSlots(std::string& output, const GameSnapshot& snapshot) {
  output.push_back('[');
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
  output.push_back(']');
}

void AppendCoordinateCandidates(std::string& output, const GameSnapshot& snapshot) {
  output.push_back('[');
  for (std::size_t index = 0; index < snapshot.coordinate_candidates.size(); ++index) {
    const auto& candidate = snapshot.coordinate_candidates[index];
    if (index != 0) {
      output.push_back(',');
    }
    output += "{\"offset\":" + std::to_string(candidate.offset) + ",\"value\":";
    AppendVector3(output, candidate.value);
    output += ",\"score\":";
    AppendFloat(output, candidate.score);
    output.push_back('}');
  }
  output.push_back(']');
}

void AppendPlayer(std::string& output, const GameSnapshot& snapshot) {
  output += "{\"valid\":";
  output += snapshot.valid ? "true" : "false";
  output += ",\"status\":";
  AppendJsonString(output, snapshot.status);
  output += ",\"manager\":";
  AppendAddress(output, snapshot.manager);
  output += ",\"avatar\":";
  AppendAddress(output, snapshot.avatar);
  output += ",\"outfit\":";
  AppendAddress(output, snapshot.outfit);
  output += ",\"database\":";
  AppendAddress(output, snapshot.outfit_database);
  output += ",\"avatarIndex\":" + std::to_string(snapshot.avatar_index);
  output += ",\"avatarActive\":" + std::to_string(snapshot.avatar_active);
  output += ",\"avatarFlags\":" + std::to_string(snapshot.avatar_flags);
  output += ",\"transform\":";
  AppendTransform(output, snapshot.transform);
  output += ",\"slots\":";
  AppendSlots(output, snapshot);
  output += ",\"coordinateCandidates\":";
  AppendCoordinateCandidates(output, snapshot);
  output.push_back('}');
}

void AppendLocalEffects(std::string& output, const LocalEffectSnapshot& effects) {
  output += "{\"supported\":";
  output += effects.supported ? "true" : "false";
  output += ",\"hookInstalled\":";
  output += effects.hook_installed ? "true" : "false";
  output += ",\"enabled\":";
  output += effects.enabled ? "true" : "false";
  output += ",\"intervalMs\":" + std::to_string(effects.interval_ms);
  output += ",\"catalogCount\":" + std::to_string(effects.catalog_count);
  output += ",\"loadedCount\":" + std::to_string(effects.loaded_count);
  output += ",\"nextIndex\":" + std::to_string(effects.next_index);
  output += ",\"poolActive\":" + std::to_string(effects.pool_active);
  output += ",\"poolCapacity\":" + std::to_string(effects.pool_capacity);
  output += ",\"generated\":" + std::to_string(effects.generated);
  output += ",\"cycles\":" + std::to_string(effects.cycles);
  output += ",\"skipped\":" + std::to_string(effects.skipped);
  output += ",\"emitterBarn\":";
  AppendAddress(output, effects.emitter_barn);
  output += ",\"lastDefinition\":";
  AppendAddress(output, effects.last_definition);
  output += ",\"lastEmitter\":";
  AppendAddress(output, effects.last_emitter);
  output += ",\"status\":";
  AppendJsonString(output, effects.status);
  output.push_back('}');
}

void AppendOutfitChanger(std::string& output, const OutfitChangerSnapshot& changer) {
  output += "{\"supported\":";
  output += changer.supported ? "true" : "false";
  output += ",\"catalogReady\":";
  output += changer.catalog_ready ? "true" : "false";
  output += ",\"gameThreadReady\":";
  output += changer.game_thread_ready ? "true" : "false";
  output += ",\"pending\":";
  output += changer.pending ? "true" : "false";
  output += ",\"totalCount\":" + std::to_string(changer.total_count);
  output += ",\"slotCounts\":[";
  for (std::size_t slot = 0; slot < changer.slot_counts.size(); ++slot) {
    if (slot != 0) {
      output.push_back(',');
    }
    output += std::to_string(changer.slot_counts[slot]);
  }
  output += "],\"pendingSlot\":" + std::to_string(changer.pending_slot);
  output += ",\"pendingName\":";
  AppendJsonString(output, changer.pending_name);
  output += ",\"lastSlot\":" + std::to_string(changer.last_slot);
  output += ",\"lastName\":";
  AppendJsonString(output, changer.last_name);
  output += ",\"applied\":" + std::to_string(changer.applied);
  output += ",\"failed\":" + std::to_string(changer.failed);
  output += ",\"resourcePath\":";
  AppendJsonString(output, changer.resource_path);
  output += ",\"status\":";
  AppendJsonString(output, changer.status);
  output.push_back('}');
}

void AppendWorld(std::string& output, const GameSnapshot& snapshot) {
  const WorldSnapshot& world = snapshot.world;
  output += "{\"root\":";
  AppendAddress(output, world.root);
  output += ",\"managerSource\":";
  AppendJsonString(output, world.manager_source);
  output += ",\"level\":";
  AppendJsonString(output, world.level);
  output += ",\"levelSource\":";
  AppendJsonString(output, world.level_source);
  output += ",\"scanStatus\":";
  AppendJsonString(output, world.scan_status);
  output += ",\"scanCycle\":" + std::to_string(world.scan_cycle);
  output += ",\"scannedPrivateBytes\":" + std::to_string(world.scanned_private_bytes);
  output += ",\"scannedPrivateRegions\":" + std::to_string(world.scanned_private_regions);
  output += ",\"objectHeaderCandidates\":" + std::to_string(world.object_header_candidates);
  output += ",\"transformCandidates\":" + std::to_string(world.transform_candidates);
  output += ",\"nearbyRadius\":";
  AppendFloat(output, world.nearby_radius);
  output += ",\"nearbyTransformCount\":" + std::to_string(world.nearby_transform_count);
  output += ",\"nearbyTransforms\":[";
  for (std::size_t index = 0; index < world.nearby_transforms.size(); ++index) {
    const auto& transform = world.nearby_transforms[index];
    if (index != 0) {
      output.push_back(',');
    }
    output += "{\"address\":";
    AppendAddress(output, transform.address);
    output += ",\"position\":";
    AppendVector3(output, transform.position);
    output += ",\"distance\":";
    AppendFloat(output, transform.distance);
    output.push_back('}');
  }
  output += "],\"room\":{\"current\":" + std::to_string(world.room_current_players);
  output += ",\"max\":" + std::to_string(world.room_max_players);
  output += ",\"avatarCapacity\":" + std::to_string(world.avatar_capacity);
  output += ",\"maxCandidates\":[";
  for (std::size_t index = 0; index < world.room_max_candidates.size(); ++index) {
    if (index != 0) {
      output.push_back(',');
    }
    output += std::to_string(world.room_max_candidates[index]);
  }
  output += "]},\"levelAssets\":{\"valid\":";
  output += world.level_assets_valid ? "true" : "false";
  output += ",\"status\":";
  AppendJsonString(output, world.level_asset_status);
  output += ",\"path\":";
  AppendJsonString(output, world.level_asset_path);
  output += ",\"objects\":" + std::to_string(world.level_object_count);
  output += ",\"properties\":" + std::to_string(world.level_property_count);
  output += ",\"sources\":" + std::to_string(world.level_source_count);
  output += ",\"waxSpawners\":" + std::to_string(world.wax_spawner_count);
  output += ",\"waxTargets\":[";
  for (std::size_t index = 0; index < world.wax_targets.size(); ++index) {
    const auto& target = world.wax_targets[index];
    if (index != 0) {
      output.push_back(',');
    }
    output += "{\"objectIndex\":" + std::to_string(target.object_index);
    output += ",\"objectName\":";
    AppendJsonString(output, target.object_name);
    output += ",\"position\":";
    AppendVector3(output, target.position);
    output += ",\"spawnCountMin\":" + std::to_string(target.spawn_count_min);
    output += ",\"spawnCountMax\":" + std::to_string(target.spawn_count_max);
    output += ",\"networked\":";
    output += target.networked ? "true" : "false";
    output += ",\"alwaysSpawn\":";
    output += target.always_spawn ? "true" : "false";
    output += ",\"usable\":";
    output += target.usable ? "true" : "false";
    output.push_back('}');
  }
  output += "]},\"waxLoop\":{\"enabled\":";
  output += world.wax_loop_enabled ? "true" : "false";
  output += ",\"intervalMs\":" + std::to_string(world.wax_loop_interval_ms);
  output += ",\"targetIndex\":" + std::to_string(world.wax_loop_target_index);
  output += ",\"teleports\":" + std::to_string(world.wax_loop_teleports);
  output += ",\"status\":";
  AppendJsonString(output, world.wax_loop_status);
  output += "},\"localEffects\":";
  AppendLocalEffects(output, snapshot.local_effects);
  output.push_back('}');
}

}  // namespace

std::string BuildSnapshotJson() {
  const GameSnapshot snapshot = GetGameState().Snapshot();
  std::string output;
  output.reserve(64 * 1024);
  output += "{\"version\":\"0.4.1\",\"build\":";
  AppendBuild(output, snapshot.build);
  output += ",\"player\":";
  AppendPlayer(output, snapshot);
  output += ",\"world\":";
  AppendWorld(output, snapshot);
  output += ",\"outfitChanger\":";
  AppendOutfitChanger(output, GetOutfitChangerSnapshot());

  // Preserve v0.2 top-level fields used by existing CE Bridge consumers.
  output += ",\"valid\":";
  output += snapshot.valid ? "true" : "false";
  output += ",\"status\":";
  AppendJsonString(output, snapshot.status);
  output += ",\"manager\":";
  AppendAddress(output, snapshot.manager);
  output += ",\"avatar\":";
  AppendAddress(output, snapshot.avatar);
  output += ",\"outfit\":";
  AppendAddress(output, snapshot.outfit);
  output += ",\"database\":";
  AppendAddress(output, snapshot.outfit_database);
  output += ",\"avatarIndex\":" + std::to_string(snapshot.avatar_index);
  output += ",\"avatarActive\":" + std::to_string(snapshot.avatar_active);
  output += ",\"avatarFlags\":" + std::to_string(snapshot.avatar_flags);
  output += ",\"transform\":";
  AppendTransform(output, snapshot.transform);
  output += ",\"slots\":";
  AppendSlots(output, snapshot);
  output += ",\"coordinateCandidates\":";
  AppendCoordinateCandidates(output, snapshot);
  output.push_back('}');
  return output;
}

std::string BuildPlayerJson() {
  const GameSnapshot snapshot = GetGameState().Snapshot();
  std::string output = "{\"version\":\"0.4.1\",\"build\":";
  AppendBuild(output, snapshot.build);
  output += ",\"player\":";
  AppendPlayer(output, snapshot);
  output.push_back('}');
  return output;
}

std::string BuildWorldJson() {
  const GameSnapshot snapshot = GetGameState().Snapshot();
  std::string output = "{\"version\":\"0.4.1\",\"build\":";
  AppendBuild(output, snapshot.build);
  output += ",\"world\":";
  AppendWorld(output, snapshot);
  output.push_back('}');
  return output;
}

std::string BuildOutfitCatalogJson() {
  constexpr std::array<const char*, 10> kTypes = {
      "Body", "Wing", "Hair", "Mask", "Neck",
      "Feet", "Horn", "Face", "Prop", "Hat",
  };
  const OutfitChangerSnapshot changer = GetOutfitChangerSnapshot();
  const auto& catalog = GetOutfitCatalog();
  std::string output;
  output.reserve(384 * 1024);
  output += "{\"version\":\"0.4.1\",\"state\":";
  AppendOutfitChanger(output, changer);
  output += ",\"slots\":[";
  for (std::size_t slot = 0; slot < catalog.size(); ++slot) {
    if (slot != 0) {
      output.push_back(',');
    }
    output += "{\"index\":" + std::to_string(slot) + ",\"type\":";
    AppendJsonString(output, kTypes[slot]);
    output += ",\"definitions\":[";
    for (std::size_t index = 0; index < catalog[slot].size(); ++index) {
      if (index != 0) {
        output.push_back(',');
      }
      const OutfitDefinition& definition = catalog[slot][index];
      output += "{\"index\":" + std::to_string(index) + ",\"id\":" +
                std::to_string(definition.id) + ",\"name\":";
      AppendJsonString(output, definition.name);
      output += ",\"season\":";
      AppendJsonString(output, definition.season);
      output += ",\"inCloset\":";
      output += definition.in_closet ? "true" : "false";
      output += ",\"isDefault\":";
      output += definition.is_default ? "true" : "false";
      output.push_back('}');
    }
    output += "]}";
  }
  output += "]}";
  return output;
}

std::string BuildHealthJson() {
  const GameSnapshot snapshot = GetGameState().Snapshot();
  const OutfitChangerSnapshot changer = GetOutfitChangerSnapshot();
  std::string output = "{\"ok\":true,\"version\":\"0.4.1\",\"buildSupported\":";
  output += snapshot.build.supported ? "true" : "false";
  output += ",\"stateValid\":";
  output += snapshot.valid ? "true" : "false";
  output += ",\"outfitCatalogReady\":";
  output += changer.catalog_ready ? "true" : "false";
  output += "}";
  return output;
}

std::string BuildSchemaJson() {
  return R"({"version":"0.4.1","readOnly":true,"bind":"127.0.0.1:27891","endpoints":{"/health":"service and build readiness","/v1/state":"complete player, outfit, world, changer and local-effect snapshot","/v1/player":"player transform and outfit slots","/v1/world":"room, level assets, nearby transforms, wax and local effects","/v1/outfits":"all outfit definitions, IDs, seasons and closet flags","/v1/schema":"this endpoint map"},"addressEncoding":"hexadecimal strings","units":{"position":"game world units","distance":"game world units","interval":"milliseconds"}})";
}

}  // namespace skyqoe
