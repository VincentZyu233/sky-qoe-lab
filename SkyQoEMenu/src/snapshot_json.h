#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace skyqoe {

std::string BuildSnapshotJson();
std::string BuildPlayerJson();
std::string BuildWorldJson();
std::string BuildEnvironmentJson();
std::string BuildEntitiesJson();
std::string BuildRoomJson();
std::string BuildObjectsJson(std::size_t offset, std::size_t limit,
                             const std::string& search);
std::string BuildOutfitCatalogJson();
std::string BuildChatStatusJson();
std::string BuildChatMessagesJson(std::uint64_t after, std::size_t limit);
std::optional<std::string> BuildChatTaskJson(std::uint64_t id);
std::string BuildHealthJson();
std::string BuildSchemaJson();

}  // namespace skyqoe
