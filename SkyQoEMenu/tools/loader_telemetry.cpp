#include "loader_telemetry.h"

#include <windows.h>
#include <winhttp.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <iomanip>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr wchar_t kApiHost[] = L"127.0.0.1";
constexpr INTERNET_PORT kApiPort = 27891;
constexpr wchar_t kStatePath[] = L"/v1/state";
constexpr std::size_t kMaximumResponseBytes = 2 * 1024 * 1024;

class UniqueInternetHandle {
 public:
  UniqueInternetHandle() = default;
  explicit UniqueInternetHandle(HINTERNET handle) : handle_(handle) {}
  ~UniqueInternetHandle() { Reset(); }

  UniqueInternetHandle(const UniqueInternetHandle&) = delete;
  UniqueInternetHandle& operator=(const UniqueInternetHandle&) = delete;

  HINTERNET Get() const { return handle_; }
  explicit operator bool() const { return handle_ != nullptr; }

  void Reset(HINTERNET next = nullptr) {
    if (handle_) {
      WinHttpCloseHandle(handle_);
    }
    handle_ = next;
  }

 private:
  HINTERNET handle_ = nullptr;
};

std::wstring CurrentTime() {
  SYSTEMTIME time{};
  GetLocalTime(&time);
  wchar_t buffer[16]{};
  std::swprintf(buffer, std::size(buffer), L"%02u:%02u:%02u", time.wHour,
                time.wMinute, time.wSecond);
  return buffer;
}

std::wstring Utf8ToWide(std::string_view value) {
  if (value.empty()) {
    return {};
  }
  if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return L"[文本过长]";
  }
  const int size = static_cast<int>(value.size());
  const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), size,
                                            nullptr, 0);
  if (required <= 0) {
    return L"[UTF-8 无效]";
  }
  std::wstring output(static_cast<std::size_t>(required), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), size, output.data(),
                          required) != required) {
    return L"[UTF-8 转换失败]";
  }
  return output;
}

std::wstring DisplayText(std::string_view value, std::size_t maximum = 140) {
  std::wstring output = Utf8ToWide(value);
  std::replace(output.begin(), output.end(), L'\r', L' ');
  std::replace(output.begin(), output.end(), L'\n', L' ');
  if (output.size() > maximum) {
    output.resize(maximum - 1);
    output.push_back(L'…');
  }
  return output;
}

const nlohmann::json* FindJson(
    const nlohmann::json& root, std::initializer_list<std::string_view> path) {
  const nlohmann::json* current = &root;
  for (const std::string_view part : path) {
    if (!current->is_object()) {
      return nullptr;
    }
    const auto found = current->find(std::string(part));
    if (found == current->end()) {
      return nullptr;
    }
    current = &*found;
  }
  return current;
}

std::string JsonString(const nlohmann::json& root,
                       std::initializer_list<std::string_view> path,
                       std::string fallback = {}) {
  const nlohmann::json* value = FindJson(root, path);
  return value && value->is_string() ? value->get<std::string>() : std::move(fallback);
}

bool JsonBool(const nlohmann::json& root,
              std::initializer_list<std::string_view> path, bool fallback = false) {
  const nlohmann::json* value = FindJson(root, path);
  return value && value->is_boolean() ? value->get<bool>() : fallback;
}

std::uint64_t JsonUnsigned(const nlohmann::json& root,
                           std::initializer_list<std::string_view> path,
                           std::uint64_t fallback = 0) {
  const nlohmann::json* value = FindJson(root, path);
  if (!value) {
    return fallback;
  }
  if (value->is_number_unsigned()) {
    return value->get<std::uint64_t>();
  }
  if (value->is_number_integer()) {
    const std::int64_t number = value->get<std::int64_t>();
    return number >= 0 ? static_cast<std::uint64_t>(number) : fallback;
  }
  return fallback;
}

double JsonNumber(const nlohmann::json& root,
                  std::initializer_list<std::string_view> path,
                  double fallback = 0.0) {
  const nlohmann::json* value = FindJson(root, path);
  if (!value || !value->is_number()) {
    return fallback;
  }
  const double number = value->get<double>();
  return std::isfinite(number) ? number : fallback;
}

std::wstring Fixed(double value, int precision = 2) {
  std::wostringstream output;
  output << std::fixed << std::setprecision(precision) << value;
  return output.str();
}

std::wstring OnOff(bool value) {
  return value ? L"开启" : L"关闭";
}

std::wstring OutfitName(const nlohmann::json& slot) {
  if (!slot.is_object()) {
    return L"未读取";
  }
  const auto name = slot.find("resourceName");
  const auto id = slot.find("effectiveId");
  const std::wstring resource =
      name != slot.end() && name->is_string() && !name->get_ref<const std::string&>().empty()
          ? DisplayText(name->get_ref<const std::string&>(), 92)
          : L"无 / 未解析";
  if (id == slot.end() || (!id->is_number_unsigned() && !id->is_number_integer())) {
    return resource;
  }
  std::uint64_t effective_id = 0;
  if (id->is_number_unsigned()) {
    effective_id = id->get<std::uint64_t>();
  } else {
    const std::int64_t signed_id = id->get<std::int64_t>();
    effective_id = signed_id >= 0 ? static_cast<std::uint64_t>(signed_id) : 0;
  }
  return resource + L"  [" + std::to_wstring(effective_id) + L"]";
}

LoaderTelemetrySnapshot ErrorSnapshot(std::wstring error) {
  LoaderTelemetrySnapshot snapshot;
  snapshot.api_status = L"API 未连接 · " + std::move(error);
  snapshot.player = L"等待模组菜单提供玩家状态";
  snapshot.position = L"位置：—";
  snapshot.world = L"地图：—";
  snapshot.environment = L"环境数据：—";
  snapshot.automation = L"自动功能：—";
  snapshot.outfit = L"等待穿搭数据…";
  snapshot.room = L"等待房间数据…";
  snapshot.chat = L"等待聊天接口数据…";
  snapshot.updated_at = L"最后尝试：" + CurrentTime();
  return snapshot;
}

std::wstring WinHttpError(DWORD error) {
  switch (error) {
    case ERROR_WINHTTP_CANNOT_CONNECT:
    case ERROR_WINHTTP_CONNECTION_ERROR:
      return L"菜单尚未启动或正在重载";
    case ERROR_WINHTTP_TIMEOUT:
      return L"本机 API 请求超时";
    case ERROR_WINHTTP_INVALID_SERVER_RESPONSE:
      return L"菜单返回了无效 HTTP 响应";
    default:
      return L"WinHTTP 错误 " + std::to_wstring(error);
  }
}

}  // namespace

LoaderTelemetrySnapshot ParseLoaderTelemetryJson(std::string_view body) {
  const nlohmann::json root = nlohmann::json::parse(body, nullptr, false, false);
  if (root.is_discarded() || !root.is_object()) {
    return ErrorSnapshot(L"状态 JSON 无效");
  }
  const nlohmann::json* player = FindJson(root, {"player"});
  const nlohmann::json* world = FindJson(root, {"world"});
  if (!player || !player->is_object() || !world || !world->is_object()) {
    return ErrorSnapshot(L"状态 JSON 缺少 player/world");
  }

  LoaderTelemetrySnapshot snapshot;
  snapshot.connected = true;
  const std::wstring version = DisplayText(JsonString(root, {"version"}, "unknown"), 24);
  snapshot.api_status = L"API 已连接 · 模组 v" + version;

  const bool player_valid = JsonBool(root, {"player", "valid"});
  const std::uint64_t avatar_index = JsonUnsigned(root, {"player", "avatarIndex"});
  const std::uint64_t active = JsonUnsigned(root, {"player", "avatarActive"});
  const std::uint64_t flags = JsonUnsigned(root, {"player", "avatarFlags"});
  std::wostringstream player_line;
  player_line << L"人物：" << (player_valid ? L"已就绪" : L"读取中")
              << L" · Avatar #" << avatar_index << L" · Active " << active
              << L" · Flags 0x" << std::uppercase << std::hex << flags;
  snapshot.player = player_line.str();

  const nlohmann::json* position = FindJson(root, {"player", "transform", "position"});
  if (position && position->is_array() && position->size() >= 3 &&
      (*position)[0].is_number() && (*position)[1].is_number() &&
      (*position)[2].is_number()) {
    snapshot.position = L"位置：X " + Fixed((*position)[0].get<double>()) + L" · Y " +
                        Fixed((*position)[1].get<double>()) + L" · Z " +
                        Fixed((*position)[2].get<double>());
  } else {
    snapshot.position = L"位置：等待有效 Transform";
  }

  std::wstring level = DisplayText(JsonString(root, {"world", "level"}), 64);
  if (level.empty()) {
    level = L"识别中";
  }
  const std::uint64_t scan_cycle = JsonUnsigned(root, {"world", "scanCycle"});
  const std::wstring scan_status =
      DisplayText(JsonString(root, {"world", "scanStatus"}), 72);
  snapshot.world = L"地图：" + level + L" · 扫描轮次 " +
                   std::to_wstring(scan_cycle) +
                   (scan_status.empty() ? L"" : L" · " + scan_status);

  const std::uint64_t objects = JsonUnsigned(root, {"world", "levelAssets", "objects"});
  const std::uint64_t properties =
      JsonUnsigned(root, {"world", "levelAssets", "properties"});
  const std::uint64_t nearby = JsonUnsigned(root, {"world", "nearbyTransformCount"});
  const std::uint64_t wax = JsonUnsigned(root, {"world", "levelAssets", "waxSpawners"});
  snapshot.environment = L"环境：对象 " + std::to_wstring(objects) + L" · 属性 " +
                         std::to_wstring(properties) + L" · 附近 Transform " +
                         std::to_wstring(nearby) + L" · Wax 点 " +
                         std::to_wstring(wax);

  const bool wax_loop = JsonBool(root, {"world", "waxLoop", "enabled"});
  const bool effect_loop = JsonBool(root, {"world", "localEffects", "enabled"});
  snapshot.automation = L"自动功能：循环烛火 " + OnOff(wax_loop) +
                        L" · 全特效 " + OnOff(effect_loop);

  constexpr std::array<const wchar_t*, 10> kSlotLabels = {
      L"身体", L"斗篷", L"发型", L"面具", L"颈饰",
      L"鞋子", L"头饰", L"面饰", L"道具", L"帽子",
  };
  std::array<std::wstring, 10> slots;
  slots.fill(L"未读取");
  const nlohmann::json* slot_list = FindJson(root, {"player", "slots"});
  if (slot_list && slot_list->is_array()) {
    for (std::size_t index = 0; index < slot_list->size() && index < slots.size(); ++index) {
      slots[index] = OutfitName((*slot_list)[index]);
    }
  }
  std::wostringstream outfit;
  for (std::size_t row = 0; row < 5; ++row) {
    if (row != 0) {
      outfit << L"\r\n";
    }
    outfit << kSlotLabels[row] << L"：" << slots[row] << L"    "
           << kSlotLabels[row + 5] << L"：" << slots[row + 5];
  }
  snapshot.outfit = outfit.str();

  const std::uint64_t current = JsonUnsigned(root, {"world", "room", "current"});
  const std::uint64_t maximum = JsonUnsigned(root, {"world", "room", "max"});
  const nlohmann::json* players = FindJson(root, {"world", "room", "players"});
  const std::size_t member_count = players && players->is_array() ? players->size() : 0;
  std::wostringstream room;
  room << L"房间：" << current << L" / "
       << (maximum == 0 ? L"未知" : std::to_wstring(maximum))
       << L" 人 · 有效成员 " << member_count;
  if (players && players->is_array()) {
    for (std::size_t index = 0; index < players->size() && index < 8; ++index) {
      const nlohmann::json& member = (*players)[index];
      room << L"\r\n#" << JsonUnsigned(member, {"index"})
           << (JsonBool(member, {"local"}) ? L" 本人" : L" 玩家")
           << L" · 距离 " << Fixed(JsonNumber(member, {"distance"}), 1);
      const nlohmann::json* member_position = FindJson(member, {"transform", "position"});
      if (member_position && member_position->is_array() && member_position->size() >= 3 &&
          (*member_position)[0].is_number() && (*member_position)[1].is_number() &&
          (*member_position)[2].is_number()) {
        room << L" · (" << Fixed((*member_position)[0].get<double>(), 1) << L", "
             << Fixed((*member_position)[1].get<double>(), 1) << L", "
             << Fixed((*member_position)[2].get<double>(), 1) << L")";
      }
    }
  }
  snapshot.room = room.str();

  const bool capture = JsonBool(root, {"chat", "captureHookInstalled"});
  const bool send = JsonBool(root, {"chat", "gameThreadReady"});
  const std::uint64_t stored = JsonUnsigned(root, {"chat", "messagesStored"});
  const std::uint64_t message_capacity = JsonUnsigned(root, {"chat", "messageCapacity"});
  const std::uint64_t queue = JsonUnsigned(root, {"chat", "queueDepth"});
  const std::uint64_t queue_capacity = JsonUnsigned(root, {"chat", "queueCapacity"});
  const std::uint64_t captured = JsonUnsigned(root, {"chat", "captured"});
  const std::uint64_t dropped = JsonUnsigned(root, {"chat", "captureDropped"});
  const std::uint64_t submitted = JsonUnsigned(root, {"chat", "submitted"});
  const std::uint64_t failed = JsonUnsigned(root, {"chat", "failed"});
  std::wostringstream chat;
  chat << L"接收 Hook：" << (capture ? L"就绪" : L"未就绪")
       << L" · 发送：" << (send ? L"就绪" : L"未就绪")
       << L"\r\n消息缓冲 " << stored << L" / " << message_capacity
       << L" · 发送队列 " << queue << L" / " << queue_capacity
       << L"\r\n捕获 " << captured << L" · 丢弃 " << dropped
       << L" · 已提交 " << submitted << L" · 失败 " << failed;
  snapshot.chat = chat.str();
  snapshot.updated_at = L"最后更新：" + CurrentTime();
  return snapshot;
}

LoaderTelemetrySnapshot FetchLoaderTelemetry() {
  UniqueInternetHandle session(WinHttpOpen(
      L"SkyQoELoader/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
      WINHTTP_NO_PROXY_BYPASS, 0));
  if (!session) {
    return ErrorSnapshot(WinHttpError(GetLastError()));
  }
  WinHttpSetTimeouts(session.Get(), 100, 300, 500, 800);

  UniqueInternetHandle connection(WinHttpConnect(session.Get(), kApiHost, kApiPort, 0));
  if (!connection) {
    return ErrorSnapshot(WinHttpError(GetLastError()));
  }
  const wchar_t* accept_types[] = {L"application/json", nullptr};
  UniqueInternetHandle request(WinHttpOpenRequest(
      connection.Get(), L"GET", kStatePath, nullptr, WINHTTP_NO_REFERER, accept_types,
      WINHTTP_FLAG_REFRESH));
  if (!request) {
    return ErrorSnapshot(WinHttpError(GetLastError()));
  }
  const wchar_t headers[] = L"Accept: application/json\r\nCache-Control: no-cache\r\n";
  if (!WinHttpSendRequest(request.Get(), headers, static_cast<DWORD>(-1L),
                          WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
      !WinHttpReceiveResponse(request.Get(), nullptr)) {
    return ErrorSnapshot(WinHttpError(GetLastError()));
  }

  DWORD status = 0;
  DWORD status_size = sizeof(status);
  if (!WinHttpQueryHeaders(request.Get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                           WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                           WINHTTP_NO_HEADER_INDEX)) {
    return ErrorSnapshot(L"无法读取 HTTP 状态码");
  }
  if (status != 200) {
    return ErrorSnapshot(L"HTTP " + std::to_wstring(status));
  }

  std::string body;
  body.reserve(64 * 1024);
  std::array<char, 8192> buffer{};
  while (true) {
    DWORD read = 0;
    if (!WinHttpReadData(request.Get(), buffer.data(), static_cast<DWORD>(buffer.size()),
                         &read)) {
      return ErrorSnapshot(WinHttpError(GetLastError()));
    }
    if (read == 0) {
      break;
    }
    if (body.size() > kMaximumResponseBytes - read) {
      return ErrorSnapshot(L"状态响应超过 2 MiB 上限");
    }
    body.append(buffer.data(), read);
  }
  return ParseLoaderTelemetryJson(body);
}
