#include "level_assets.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>

namespace skyqoe {
namespace {

struct FieldDefinition {
  std::uint32_t kind = 0;
  std::string name;
  std::uint32_t size = 0;
  std::int32_t auxiliary = 0;
};

struct TypeDefinition {
  std::uint32_t type_id = 0;
  std::vector<FieldDefinition> fields;
};

struct ParsedFields {
  std::optional<std::array<float, 3>> transform;
  std::uint32_t max_players = 0;
  std::uint32_t spawn_count_min = 0;
  std::uint32_t spawn_count_max = 0;
  bool networked = false;
  bool always_spawn = false;
};

bool ReadU32(const std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t& value) {
  if (offset > bytes.size() || bytes.size() - offset < sizeof(value)) {
    return false;
  }
  std::memcpy(&value, bytes.data() + offset, sizeof(value));
  return true;
}

bool ReadI32(const std::vector<std::uint8_t>& bytes, std::size_t offset, std::int32_t& value) {
  if (offset > bytes.size() || bytes.size() - offset < sizeof(value)) {
    return false;
  }
  std::memcpy(&value, bytes.data() + offset, sizeof(value));
  return true;
}

bool ReadCString(const std::vector<std::uint8_t>& bytes, std::size_t& offset, std::size_t limit,
                 std::string& value) {
  if (offset >= bytes.size() || limit > bytes.size()) {
    return false;
  }
  const std::size_t end_limit = std::min(limit, bytes.size());
  const auto begin = bytes.begin() + static_cast<std::ptrdiff_t>(offset);
  const auto end = std::find(begin, bytes.begin() + static_cast<std::ptrdiff_t>(end_limit), 0);
  if (end == bytes.begin() + static_cast<std::ptrdiff_t>(end_limit)) {
    return false;
  }
  value.assign(reinterpret_cast<const char*>(bytes.data() + offset),
               static_cast<std::size_t>(end - begin));
  offset = static_cast<std::size_t>(end - bytes.begin()) + 1;
  return true;
}

bool ReadTransform(const std::vector<std::uint8_t>& bytes, std::size_t offset,
                   std::array<float, 3>& position) {
  std::array<float, 16> matrix{};
  if (offset > bytes.size() || bytes.size() - offset < sizeof(matrix)) {
    return false;
  }
  std::memcpy(matrix.data(), bytes.data() + offset, sizeof(matrix));
  for (float value : matrix) {
    if (!std::isfinite(value) || std::abs(value) > 1000000.0F) {
      return false;
    }
  }
  if (std::abs(matrix[3]) > 0.001F || std::abs(matrix[7]) > 0.001F ||
      std::abs(matrix[11]) > 0.001F || std::abs(matrix[15] - 1.0F) > 0.001F) {
    return false;
  }
  position = {matrix[12], matrix[13], matrix[14]};
  return true;
}

bool ParseFields(const std::vector<std::uint8_t>& bytes,
                 const std::vector<TypeDefinition>& types,
                 const std::vector<FieldDefinition>& fields, std::size_t& offset,
                 ParsedFields* parsed, std::uint32_t depth) {
  if (depth > 8) {
    return false;
  }
  for (const auto& field : fields) {
    const std::size_t field_offset = offset;
    switch (field.kind) {
      case 0: {
        if (offset > bytes.size() || bytes.size() - offset < field.size) {
          return false;
        }
        if (parsed && field.name == "transform" && field.size == sizeof(float) * 16) {
          std::array<float, 3> position{};
          if (ReadTransform(bytes, offset, position)) {
            parsed->transform = position;
          }
        } else if (parsed && field.size == sizeof(std::uint32_t)) {
          std::uint32_t value = 0;
          std::memcpy(&value, bytes.data() + offset, sizeof(value));
          if (field.name == "maxPlayers") {
            parsed->max_players = value;
          } else if (field.name == "spawnCountMin") {
            parsed->spawn_count_min = value;
          } else if (field.name == "spawnCountMax") {
            parsed->spawn_count_max = value;
          }
        } else if (parsed && field.size == 1) {
          if (field.name == "networkedPickups") {
            parsed->networked = bytes[offset] != 0;
          } else if (field.name == "alwaysSpawn") {
            parsed->always_spawn = bytes[offset] != 0;
          }
        }
        offset += field.size;
        break;
      }
      case 1: {
        std::string ignored;
        if (!ReadCString(bytes, offset, bytes.size(), ignored)) {
          return false;
        }
        break;
      }
      case 2:
        if (offset > bytes.size() || bytes.size() - offset < sizeof(std::uint32_t)) {
          return false;
        }
        offset += sizeof(std::uint32_t);
        break;
      case 3: {
        std::uint32_t count = 0;
        if (!ReadU32(bytes, offset, count)) {
          return false;
        }
        offset += sizeof(std::uint32_t);
        if (count > 1000000) {
          return false;
        }
        if (field.auxiliary == -1) {
          const std::size_t size = static_cast<std::size_t>(count) * sizeof(std::uint32_t);
          if (offset > bytes.size() || bytes.size() - offset < size) {
            return false;
          }
          offset += size;
        } else {
          if (field.auxiliary < 0 || static_cast<std::size_t>(field.auxiliary) >= types.size()) {
            return false;
          }
          const auto& nested = types[static_cast<std::size_t>(field.auxiliary)].fields;
          for (std::uint32_t index = 0; index < count; ++index) {
            if (!ParseFields(bytes, types, nested, offset, nullptr, depth + 1)) {
              return false;
            }
          }
        }
        break;
      }
      default:
        return false;
    }
    if (offset < field_offset || offset > bytes.size()) {
      return false;
    }
  }
  return true;
}

std::string WideToUtf8(const std::wstring& value) {
  if (value.empty()) {
    return {};
  }
  const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                       nullptr, 0, nullptr, nullptr);
  if (size <= 0) {
    return {};
  }
  std::string output(static_cast<std::size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), output.data(), size,
                      nullptr, nullptr);
  return output;
}

bool IsUsableWaxPosition(const std::array<float, 3>& position) {
  return std::all_of(position.begin(), position.end(), [](float component) {
           return std::isfinite(component) && std::abs(component) < 10000.0F;
         }) &&
         position[1] > 10.0F;
}

}  // namespace

LevelAssetSnapshot ParseLevelAssetFile(const std::string& level, const std::wstring& path) {
  LevelAssetSnapshot result;
  result.level = level;
  result.path = WideToUtf8(path);

  std::ifstream stream(std::filesystem::path(path), std::ios::binary | std::ios::ate);
  if (!stream) {
    result.status = "level asset could not be opened";
    return result;
  }
  const std::streamsize file_size = stream.tellg();
  if (file_size < 0x2C || file_size > static_cast<std::streamsize>(512 * 1024 * 1024)) {
    result.status = "invalid level asset size";
    return result;
  }
  stream.seekg(0, std::ios::beg);
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(file_size));
  if (!stream.read(reinterpret_cast<char*>(bytes.data()), file_size)) {
    result.status = "level asset read failed";
    return result;
  }
  if (std::memcmp(bytes.data(), "TGCL", 4) != 0) {
    result.status = "unsupported level asset magic";
    return result;
  }

  std::uint32_t version = 0;
  std::uint32_t symbol_records_offset = 0;
  std::uint32_t string_pool_offset = 0;
  std::uint32_t object_data_offset = 0;
  std::uint32_t declared_size = 0;
  if (!ReadU32(bytes, 0x04, version) || version != 1 || !ReadU32(bytes, 0x08, result.type_count) ||
      !ReadU32(bytes, 0x0C, result.symbol_count) || !ReadU32(bytes, 0x10, result.object_count) ||
      !ReadU32(bytes, 0x14, result.property_count) || !ReadU32(bytes, 0x18, result.source_count) ||
      !ReadU32(bytes, 0x1C, symbol_records_offset) ||
      !ReadU32(bytes, 0x20, string_pool_offset) || !ReadU32(bytes, 0x24, object_data_offset) ||
      !ReadU32(bytes, 0x28, declared_size) || declared_size != bytes.size() ||
      result.type_count == 0 || result.type_count > 100000 || result.symbol_count > 1000000 ||
      result.object_count > 10000000 || symbol_records_offset < 0x2C ||
      string_pool_offset < symbol_records_offset || object_data_offset < string_pool_offset ||
      object_data_offset > bytes.size() ||
      symbol_records_offset + static_cast<std::uint64_t>(result.symbol_count) * 16 >
          string_pool_offset ||
      0x2C + static_cast<std::uint64_t>(result.type_count) * 12 > symbol_records_offset) {
    result.status = "invalid TGCL header or section bounds";
    return result;
  }

  std::vector<FieldDefinition> symbols;
  symbols.reserve(result.symbol_count);
  for (std::uint32_t index = 0; index < result.symbol_count; ++index) {
    const std::size_t record = symbol_records_offset + static_cast<std::size_t>(index) * 16;
    FieldDefinition field;
    std::uint32_t name_offset = 0;
    if (!ReadU32(bytes, record, field.kind) || !ReadU32(bytes, record + 4, name_offset) ||
        !ReadU32(bytes, record + 8, field.size) || !ReadI32(bytes, record + 12, field.auxiliary) ||
        name_offset >= object_data_offset - string_pool_offset) {
      result.status = "invalid TGCL symbol record";
      return result;
    }
    std::size_t name_address = string_pool_offset + name_offset;
    if (!ReadCString(bytes, name_address, object_data_offset, field.name)) {
      result.status = "invalid TGCL symbol name";
      return result;
    }
    symbols.push_back(std::move(field));
  }

  std::vector<TypeDefinition> types;
  types.reserve(result.type_count);
  for (std::uint32_t index = 0; index < result.type_count; ++index) {
    const std::size_t record = 0x2C + static_cast<std::size_t>(index) * 12;
    TypeDefinition type;
    std::uint32_t start = 0;
    std::uint32_t count = 0;
    if (!ReadU32(bytes, record, type.type_id) || !ReadU32(bytes, record + 4, start) ||
        !ReadU32(bytes, record + 8, count) || start > symbols.size() ||
        count > symbols.size() - start) {
      result.status = "invalid TGCL type record";
      return result;
    }
    type.fields.assign(symbols.begin() + start, symbols.begin() + start + count);
    types.push_back(std::move(type));
  }

  std::size_t offset = object_data_offset;
  std::optional<std::array<float, 3>> last_transform;
  for (std::uint32_t object_index = 0; object_index < result.object_count; ++object_index) {
    std::uint32_t type_index = 0;
    if (!ReadU32(bytes, offset, type_index) || type_index >= types.size()) {
      result.status = "invalid TGCL object type";
      return result;
    }
    offset += sizeof(std::uint32_t);
    std::string object_name;
    if (!ReadCString(bytes, offset, bytes.size(), object_name)) {
      result.status = "invalid TGCL object name";
      return result;
    }
    ParsedFields parsed;
    if (!ParseFields(bytes, types, types[type_index].fields, offset, &parsed, 0)) {
      result.status = "invalid TGCL object payload";
      return result;
    }
    if (parsed.transform) {
      last_transform = parsed.transform;
    }
    if (type_index == 41 && last_transform) {
      LevelWaxTarget target;
      target.object_index = object_index;
      target.object_name = object_name;
      target.position = *last_transform;
      target.spawn_count_min = parsed.spawn_count_min;
      target.spawn_count_max = parsed.spawn_count_max;
      target.networked = parsed.networked;
      target.always_spawn = parsed.always_spawn;
      target.usable = IsUsableWaxPosition(target.position);
      result.wax_targets.push_back(std::move(target));
    }
    if (type_index == 165) {
      result.room_max_candidates.push_back(parsed.max_players);
      if (parsed.max_players > 0 && parsed.max_players <= 60) {
        result.room_max_players = std::max(result.room_max_players, parsed.max_players);
      }
    }
  }
  if (offset != bytes.size()) {
    result.status = "TGCL object stream did not end at file boundary";
    return result;
  }
  result.wax_spawner_count = static_cast<std::uint32_t>(result.wax_targets.size());
  result.valid = true;
  result.status = "ready";
  return result;
}

LevelAssetSnapshot LoadLevelAssets(const std::string& level) {
  LevelAssetSnapshot result;
  result.level = level;
  if (level.empty() || level.size() > 128 ||
      !std::all_of(level.begin(), level.end(), [](unsigned char character) {
        return std::isalnum(character) || character == '_' || character == '-';
      })) {
    result.status = "invalid level name";
    return result;
  }

  wchar_t executable_path[MAX_PATH]{};
  const DWORD length = GetModuleFileNameW(nullptr, executable_path, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) {
    result.status = "game executable path unavailable";
    return result;
  }
  const std::filesystem::path assets =
      std::filesystem::path(executable_path).parent_path() / L"data" / L"assets";
  std::error_code error;
  for (const auto& domain : std::filesystem::directory_iterator(assets, error)) {
    if (error || !domain.is_directory(error)) {
      continue;
    }
    const std::filesystem::path candidate = domain.path() / L"Data" / L"Levels" /
                                            std::filesystem::u8path(level) /
                                            L"Objects.level.bin";
    if (std::filesystem::is_regular_file(candidate, error)) {
      return ParseLevelAssetFile(level, candidate.wstring());
    }
  }

  // MinGW's filesystem directory status can fail on some injected-process paths even when the
  // same file is accessible. Keep a Win32 enumeration fallback for the live game environment.
  WIN32_FIND_DATAW entry{};
  const std::wstring pattern = (assets / L"*").wstring();
  const HANDLE search = FindFirstFileW(pattern.c_str(), &entry);
  if (search != INVALID_HANDLE_VALUE) {
    do {
      if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
          std::wcscmp(entry.cFileName, L".") == 0 || std::wcscmp(entry.cFileName, L"..") == 0) {
        continue;
      }
      const std::filesystem::path candidate =
          assets / entry.cFileName / L"Data" / L"Levels" / std::filesystem::u8path(level) /
          L"Objects.level.bin";
      const DWORD attributes = GetFileAttributesW(candidate.c_str());
      if (attributes != INVALID_FILE_ATTRIBUTES &&
          (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        FindClose(search);
        return ParseLevelAssetFile(level, candidate.wstring());
      }
    } while (FindNextFileW(search, &entry));
    FindClose(search);
  }
  result.status = "level asset not found";
  return result;
}

}  // namespace skyqoe
