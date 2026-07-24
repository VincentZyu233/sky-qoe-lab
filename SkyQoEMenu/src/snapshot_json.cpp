#include "snapshot_json.h"

#include "chat_bridge.h"
#include "game_state.h"
#include "outfit_changer.h"

#include <algorithm>
#include <cmath>
#include <cctype>
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

void AppendSlots(std::string& output,
                 const std::array<OutfitSlotSnapshot, 10>& slots) {
  output.push_back('[');
  for (std::size_t index = 0; index < slots.size(); ++index) {
    const auto& slot = slots[index];
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

void AppendSlots(std::string& output, const GameSnapshot& snapshot) {
  AppendSlots(output, snapshot.slots);
}

void AppendUuid(std::string& output, const std::array<std::uint8_t, 16>& uuid,
                bool valid) {
  if (!valid) {
    output += "null";
    return;
  }
  static constexpr char kHex[] = "0123456789abcdef";
  std::string text;
  text.reserve(36);
  for (std::size_t index = 0; index < uuid.size(); ++index) {
    if (index == 4 || index == 6 || index == 8 || index == 10) {
      text.push_back('-');
    }
    text.push_back(kHex[uuid[index] >> 4U]);
    text.push_back(kHex[uuid[index] & 0x0FU]);
  }
  AppendJsonString(output, text);
}

void AppendRoomPlayers(std::string& output, const WorldSnapshot& world) {
  output.push_back('[');
  for (std::size_t index = 0; index < world.room_players.size(); ++index) {
    const auto& player = world.room_players[index];
    if (index != 0) {
      output.push_back(',');
    }
    output += "{\"index\":" + std::to_string(player.index) + ",\"avatar\":";
    AppendAddress(output, player.avatar);
    output += ",\"outfit\":";
    AppendAddress(output, player.outfit);
    output += ",\"database\":";
    AppendAddress(output, player.outfit_database);
    output += ",\"active\":" + std::to_string(player.active);
    output += ",\"flags\":" + std::to_string(player.flags);
    output += ",\"local\":";
    output += player.local ? "true" : "false";
    output += ",\"uuid\":";
    AppendUuid(output, player.uuid, player.uuid_valid);
    output += ",\"distance\":";
    AppendFloat(output, player.distance);
    output += ",\"transform\":";
    AppendTransform(output, player.transform);
    output += ",\"slots\":";
    AppendSlots(output, player.slots);
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

void AppendChatStatus(std::string& output, const ChatStatusSnapshot& chat) {
  output += "{\"captureSupported\":";
  output += chat.capture_supported ? "true" : "false";
  output += ",\"captureHookInstalled\":";
  output += chat.capture_hook_installed ? "true" : "false";
  output += ",\"sendSupported\":";
  output += chat.send_supported ? "true" : "false";
  output += ",\"gameThreadReady\":";
  output += chat.game_thread_ready ? "true" : "false";
  output += ",\"shuttingDown\":";
  output += chat.shutting_down ? "true" : "false";
  output += ",\"queueDepth\":" + std::to_string(chat.queue_depth);
  output += ",\"queueCapacity\":" + std::to_string(chat.queue_capacity);
  output += ",\"messagesStored\":" + std::to_string(chat.messages_stored);
  output += ",\"messageCapacity\":" + std::to_string(chat.message_capacity);
  output += ",\"oldestSequence\":" + std::to_string(chat.oldest_sequence);
  output += ",\"newestSequence\":" + std::to_string(chat.newest_sequence);
  output += ",\"captured\":" + std::to_string(chat.captured);
  output += ",\"captureDropped\":" + std::to_string(chat.capture_dropped);
  output += ",\"submitted\":" + std::to_string(chat.submitted);
  output += ",\"failed\":" + std::to_string(chat.failed);
  output += ",\"status\":";
  AppendJsonString(output, chat.status);
  output.push_back('}');
}

void AppendRoom(std::string& output, const WorldSnapshot& world, bool include_players) {
  output += "{\"current\":" + std::to_string(world.room_current_players);
  output += ",\"max\":" + std::to_string(world.room_max_players);
  output += ",\"avatarCapacity\":" + std::to_string(world.avatar_capacity);
  output += ",\"maxCandidates\":[";
  for (std::size_t index = 0; index < world.room_max_candidates.size(); ++index) {
    if (index != 0) {
      output.push_back(',');
    }
    output += std::to_string(world.room_max_candidates[index]);
  }
  output.push_back(']');
  if (include_players) {
    output += ",\"players\":";
    AppendRoomPlayers(output, world);
  }
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
  output += "],\"room\":";
  AppendRoom(output, world, true);
  output += ",\"levelAssets\":{\"valid\":";
  output += world.level_assets_valid ? "true" : "false";
  output += ",\"status\":";
  AppendJsonString(output, world.level_asset_status);
  output += ",\"path\":";
  AppendJsonString(output, world.level_asset_path);
  output += ",\"types\":" + std::to_string(world.level_type_count);
  output += ",\"symbols\":" + std::to_string(world.level_symbol_count);
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
  output += ",\"chat\":";
  AppendChatStatus(output, GetChatStatusSnapshot());
  output.push_back('}');
}

bool ContainsInsensitive(const std::string& value, const std::string& search) {
  if (search.empty()) {
    return true;
  }
  return std::search(value.begin(), value.end(), search.begin(), search.end(),
                     [](unsigned char left, unsigned char right) {
                       return std::tolower(left) == std::tolower(right);
                     }) != value.end();
}

}  // namespace

std::string BuildSnapshotJson() {
  const GameSnapshot snapshot = GetGameState().Snapshot();
  std::string output;
  output.reserve(64 * 1024);
  output += "{\"version\":\"0.6.0\",\"build\":";
  AppendBuild(output, snapshot.build);
  output += ",\"player\":";
  AppendPlayer(output, snapshot);
  output += ",\"world\":";
  AppendWorld(output, snapshot);
  output += ",\"outfitChanger\":";
  AppendOutfitChanger(output, GetOutfitChangerSnapshot());
  output += ",\"chat\":";
  AppendChatStatus(output, GetChatStatusSnapshot());

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
  std::string output = "{\"version\":\"0.6.0\",\"build\":";
  AppendBuild(output, snapshot.build);
  output += ",\"player\":";
  AppendPlayer(output, snapshot);
  output.push_back('}');
  return output;
}

std::string BuildWorldJson() {
  const GameSnapshot snapshot = GetGameState().Snapshot();
  std::string output = "{\"version\":\"0.6.0\",\"build\":";
  AppendBuild(output, snapshot.build);
  output += ",\"world\":";
  AppendWorld(output, snapshot);
  output.push_back('}');
  return output;
}

std::string BuildEnvironmentJson() {
  const GameSnapshot snapshot = GetGameState().Snapshot();
  const WorldSnapshot& world = snapshot.world;
  std::string output = "{\"version\":\"0.6.0\",\"build\":";
  AppendBuild(output, snapshot.build);
  output += ",\"environment\":{\"root\":";
  AppendAddress(output, world.root);
  output += ",\"managerSource\":";
  AppendJsonString(output, world.manager_source);
  output += ",\"level\":";
  AppendJsonString(output, world.level);
  output += ",\"levelSource\":";
  AppendJsonString(output, world.level_source);
  output += ",\"scan\":{\"status\":";
  AppendJsonString(output, world.scan_status);
  output += ",\"cycle\":" + std::to_string(world.scan_cycle);
  output += ",\"privateBytes\":" + std::to_string(world.scanned_private_bytes);
  output += ",\"privateRegions\":" + std::to_string(world.scanned_private_regions);
  output += ",\"objectHeaderCandidates\":" +
            std::to_string(world.object_header_candidates);
  output += ",\"transformCandidates\":" + std::to_string(world.transform_candidates);
  output += "},\"levelAssets\":{\"valid\":";
  output += world.level_assets_valid ? "true" : "false";
  output += ",\"status\":";
  AppendJsonString(output, world.level_asset_status);
  output += ",\"path\":";
  AppendJsonString(output, world.level_asset_path);
  output += ",\"types\":" + std::to_string(world.level_type_count);
  output += ",\"symbols\":" + std::to_string(world.level_symbol_count);
  output += ",\"objects\":" + std::to_string(world.level_object_count);
  output += ",\"properties\":" + std::to_string(world.level_property_count);
  output += ",\"sources\":" + std::to_string(world.level_source_count);
  output += ",\"waxSpawners\":" + std::to_string(world.wax_spawner_count);
  output += "},\"room\":";
  AppendRoom(output, world, false);
  output += "}}";
  return output;
}

std::string BuildEntitiesJson() {
  const GameSnapshot snapshot = GetGameState().Snapshot();
  const WorldSnapshot& world = snapshot.world;
  std::string output = "{\"version\":\"0.6.0\",\"entities\":{\"roomPlayers\":";
  AppendRoomPlayers(output, world);
  output += ",\"nearbyRadius\":";
  AppendFloat(output, world.nearby_radius);
  output += ",\"nearbyTransformTotal\":" + std::to_string(world.nearby_transform_count);
  output += ",\"nearbyTransforms\":[";
  for (std::size_t index = 0; index < world.nearby_transforms.size(); ++index) {
    if (index != 0) {
      output.push_back(',');
    }
    const auto& transform = world.nearby_transforms[index];
    output += "{\"address\":";
    AppendAddress(output, transform.address);
    output += ",\"position\":";
    AppendVector3(output, transform.position);
    output += ",\"distance\":";
    AppendFloat(output, transform.distance);
    output.push_back('}');
  }
  output += "],\"waxTargets\":[";
  for (std::size_t index = 0; index < world.wax_targets.size(); ++index) {
    if (index != 0) {
      output.push_back(',');
    }
    const auto& target = world.wax_targets[index];
    output += "{\"objectIndex\":" + std::to_string(target.object_index);
    output += ",\"name\":";
    AppendJsonString(output, target.object_name);
    output += ",\"position\":";
    AppendVector3(output, target.position);
    output += ",\"usable\":";
    output += target.usable ? "true" : "false";
    output += ",\"networked\":";
    output += target.networked ? "true" : "false";
    output.push_back('}');
  }
  output += "]}}";
  return output;
}

std::string BuildRoomJson() {
  const GameSnapshot snapshot = GetGameState().Snapshot();
  std::string output = "{\"version\":\"0.6.0\",\"room\":";
  AppendRoom(output, snapshot.world, true);
  output.push_back('}');
  return output;
}

std::string BuildObjectsJson(std::size_t offset, std::size_t limit,
                             const std::string& search) {
  const GameSnapshot snapshot = GetGameState().Snapshot();
  const WorldSnapshot& world = snapshot.world;
  limit = std::clamp<std::size_t>(limit, 1, 256);
  const auto objects = world.level_objects;
  std::size_t matched = 0;
  std::size_t returned = 0;
  std::string items;
  items.reserve(limit * 160);
  if (objects) {
    for (const auto& object : *objects) {
      if (!ContainsInsensitive(object.name, search)) {
        continue;
      }
      const std::size_t match_index = matched++;
      if (match_index < offset || returned >= limit) {
        continue;
      }
      if (returned++ != 0) {
        items.push_back(',');
      }
      items += "{\"index\":" + std::to_string(object.index);
      items += ",\"typeIndex\":" + std::to_string(object.type_index);
      items += ",\"typeId\":" + std::to_string(object.type_id);
      items += ",\"name\":";
      AppendJsonString(items, object.name);
      items += ",\"hasOwnTransform\":";
      items += object.has_transform ? "true" : "false";
      items += ",\"transform\":";
      if (object.has_transform) {
        AppendVector3(items, object.transform);
      } else {
        items += "null";
      }
      items += ",\"anchorPosition\":";
      if (object.has_anchor_position) {
        AppendVector3(items, object.anchor_position);
      } else {
        items += "null";
      }
      items.push_back('}');
    }
  }
  std::string output = "{\"version\":\"0.6.0\",\"level\":";
  AppendJsonString(output, world.level);
  output += ",\"catalogReady\":";
  output += world.level_assets_valid ? "true" : "false";
  output += ",\"total\":" + std::to_string(objects ? objects->size() : 0);
  output += ",\"matched\":" + std::to_string(matched);
  output += ",\"offset\":" + std::to_string(offset);
  output += ",\"limit\":" + std::to_string(limit);
  output += ",\"returned\":" + std::to_string(returned);
  output += ",\"search\":";
  AppendJsonString(output, search);
  output += ",\"objects\":[" + items + "]}";
  return output;
}

std::string BuildChatStatusJson() {
  std::string output = "{\"version\":\"0.6.0\",\"chat\":";
  AppendChatStatus(output, GetChatStatusSnapshot());
  output.push_back('}');
  return output;
}

std::string BuildChatMessagesJson(std::uint64_t after, std::size_t limit) {
  limit = std::clamp<std::size_t>(limit, 1, 100);
  const ChatStatusSnapshot status = GetChatStatusSnapshot();
  const auto messages = GetChatMessages(after, limit);
  std::string output = "{\"version\":\"0.6.0\",\"after\":" + std::to_string(after);
  output += ",\"limit\":" + std::to_string(limit);
  output += ",\"oldestAvailable\":" + std::to_string(status.oldest_sequence);
  output += ",\"newestAvailable\":" + std::to_string(status.newest_sequence);
  output += ",\"cursorExpired\":";
  output += status.oldest_sequence != 0 && after < status.oldest_sequence - 1 ? "true" : "false";
  output += ",\"messages\":[";
  for (std::size_t index = 0; index < messages.size(); ++index) {
    if (index != 0) {
      output.push_back(',');
    }
    const auto& message = messages[index];
    output += "{\"sequence\":" + std::to_string(message.sequence);
    output += ",\"receivedAtMs\":" + std::to_string(message.received_at_ms);
    output += ",\"text\":";
    AppendJsonString(output, message.text);
    output += ",\"sourceUuid\":";
    AppendJsonString(output, message.source_uuid);
    output += ",\"channel\":" + std::to_string(message.channel);
    output += ",\"channelName\":";
    AppendJsonString(output, message.channel_name);
    output += ",\"direction\":";
    AppendJsonString(output, message.direction);
    output += ",\"senderAvatar\":";
    AppendAddress(output, message.sender_avatar);
    output += ",\"senderAvatarIndex\":" + std::to_string(message.sender_avatar_index);
    output.push_back('}');
  }
  output += "],\"chat\":";
  AppendChatStatus(output, status);
  output.push_back('}');
  return output;
}

std::optional<std::string> BuildChatTaskJson(std::uint64_t id) {
  const auto task = GetChatTask(id);
  if (!task) {
    return std::nullopt;
  }
  std::string output = "{\"version\":\"0.6.0\",\"task\":{\"id\":" +
                       std::to_string(task->id);
  output += ",\"state\":";
  AppendJsonString(output, task->state);
  output += ",\"detail\":";
  AppendJsonString(output, task->detail);
  output += ",\"createdAtMs\":" + std::to_string(task->created_at_ms);
  output += ",\"updatedAtMs\":" + std::to_string(task->updated_at_ms);
  output += "}}";
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
  output += "{\"version\":\"0.6.0\",\"state\":";
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
  const ChatStatusSnapshot chat = GetChatStatusSnapshot();
  std::string output = "{\"ok\":true,\"version\":\"0.6.0\",\"buildSupported\":";
  output += snapshot.build.supported ? "true" : "false";
  output += ",\"stateValid\":";
  output += snapshot.valid ? "true" : "false";
  output += ",\"outfitCatalogReady\":";
  output += changer.catalog_ready ? "true" : "false";
  output += ",\"chatCaptureReady\":";
  output += chat.capture_hook_installed ? "true" : "false";
  output += ",\"chatSendReady\":";
  output += chat.game_thread_ready ? "true" : "false";
  output += "}";
  return output;
}

std::string BuildSchemaJson() {
  return R"({"version":"0.6.0","readOnly":false,"bind":"127.0.0.1:27891","endpoints":{"GET /health":"service, build and chat readiness","GET /v1/state":"complete local player, world, outfit, effect and chat status snapshot","GET /v1/player":"local player transform and outfit slots","GET /v1/world":"world scan, room, level assets, nearby transforms and automation","GET /v1/environment":"level and scan environment summary","GET /v1/entities":"room avatars, nearby transform candidates and wax targets","GET /v1/room":"room capacity and validated avatar members","GET /v1/objects":"paginated TGCL object catalog; offset, limit and search query parameters","GET /v1/outfits":"all outfit definitions, IDs, seasons and closet flags","GET /v1/chat/status":"chat hook, queue and ring-buffer status","GET /v1/chat/messages":"captured chat; after and limit query parameters","POST /v1/chat/send":"queue one UTF-8 chat message and return 202 with a task ID","GET /v1/tasks/{id}":"queued chat task state","GET /v1/schema":"this endpoint map"},"limits":{"requestHeadersBytes":16384,"requestBodyBytes":4096,"chatMessageUtf8Bytes":256,"chatQueue":8,"chatMessages":256,"chatTasks":128,"objectPage":256},"addressEncoding":"hexadecimal strings","units":{"position":"game world units","distance":"game world units","interval":"milliseconds","timestamps":"Unix milliseconds"}})";
}

}  // namespace skyqoe
