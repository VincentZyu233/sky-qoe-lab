#include "http_server.h"

#include "chat_bridge.h"
#include "snapshot_json.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <condition_variable>
#include <cctype>
#include <cstdint>
#include <deque>
#include <initializer_list>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>

namespace skyqoe {
namespace {

constexpr unsigned short kHttpPort = 27891;
constexpr std::size_t kMaximumHeaderBytes = 16 * 1024;
constexpr std::size_t kMaximumBodyBytes = 4 * 1024;
constexpr std::size_t kMaximumRequestTargetBytes = 2048;
constexpr std::size_t kMaximumQueryParameters = 16;
constexpr std::size_t kWorkerCount = 4;
constexpr std::size_t kClientQueueCapacity = 16;

struct HttpRequest {
  std::string method;
  std::string path;
  std::string version;
  std::unordered_map<std::string, std::string> query;
  std::unordered_map<std::string, std::string> headers;
  std::string body;
};

struct HttpError {
  int status = 400;
  std::string reason = "Bad Request";
  std::string code = "bad_request";
  std::string message = "invalid request";
};

std::atomic<bool> g_stop{false};
std::atomic<std::uintptr_t> g_listen_socket{static_cast<std::uintptr_t>(INVALID_SOCKET)};
std::atomic<std::uint64_t> g_request_id{1};
std::thread g_server_thread;
std::array<std::thread, kWorkerCount> g_workers;
std::mutex g_client_mutex;
std::condition_variable g_client_ready;
std::deque<SOCKET> g_clients;

bool SendAll(SOCKET socket, const char* data, std::size_t size) {
  std::size_t sent = 0;
  while (sent < size) {
    const int request = static_cast<int>(std::min<std::size_t>(
        size - sent, static_cast<std::size_t>(std::numeric_limits<int>::max())));
    const int chunk = send(socket, data + sent, request, 0);
    if (chunk <= 0) {
      return false;
    }
    sent += static_cast<std::size_t>(chunk);
  }
  return true;
}

std::string ErrorJson(std::string_view code, std::string_view message,
                      std::uint64_t request_id, std::uint32_t retry_after_ms = 0) {
  nlohmann::json body = {
      {"error", {{"code", code}, {"message", message}}},
      {"requestId", request_id},
  };
  if (retry_after_ms != 0) {
    body["error"]["retryAfterMs"] = retry_after_ms;
  }
  return body.dump();
}

void Respond(SOCKET client, int status, std::string_view reason, const std::string& body,
             std::uint64_t request_id, std::string_view extra_headers = {}) {
  std::string header = "HTTP/1.1 " + std::to_string(status) + " " + std::string(reason) +
                       "\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: " +
                       std::to_string(body.size()) +
                       "\r\nAccess-Control-Allow-Origin: *"
                       "\r\nAccess-Control-Allow-Methods: GET, POST, OPTIONS"
                       "\r\nAccess-Control-Allow-Headers: Content-Type"
                       "\r\nAccess-Control-Max-Age: 600"
                       "\r\nX-Content-Type-Options: nosniff"
                       "\r\nX-SkyQoE-Request-Id: " + std::to_string(request_id) +
                       "\r\nCache-Control: no-store\r\n";
  header.append(extra_headers);
  header += "Connection: close\r\n\r\n";
  SendAll(client, header.data(), header.size());
  if (!body.empty()) {
    SendAll(client, body.data(), body.size());
  }
}

std::string Lower(std::string_view value) {
  std::string output(value);
  std::transform(output.begin(), output.end(), output.begin(), [](unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  return output;
}

std::string_view Trim(std::string_view value) {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
    value.remove_prefix(1);
  }
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
    value.remove_suffix(1);
  }
  return value;
}

bool IsHeaderName(std::string_view value) {
  return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char character) {
    return std::isalnum(character) || character == '!' || character == '#' ||
           character == '$' || character == '%' || character == '&' || character == '\'' ||
           character == '*' || character == '+' || character == '-' || character == '.' ||
           character == '^' || character == '_' || character == '`' || character == '|' ||
           character == '~';
  });
}

bool IsValidUtf8(std::string_view text) {
  std::size_t index = 0;
  while (index < text.size()) {
    const std::uint8_t first = static_cast<std::uint8_t>(text[index]);
    if (first <= 0x7FU) {
      ++index;
      continue;
    }
    std::size_t length = 0;
    std::uint32_t codepoint = 0;
    if (first >= 0xC2U && first <= 0xDFU) {
      length = 2;
      codepoint = first & 0x1FU;
    } else if (first >= 0xE0U && first <= 0xEFU) {
      length = 3;
      codepoint = first & 0x0FU;
    } else if (first >= 0xF0U && first <= 0xF4U) {
      length = 4;
      codepoint = first & 0x07U;
    } else {
      return false;
    }
    if (index + length > text.size()) {
      return false;
    }
    for (std::size_t part = 1; part < length; ++part) {
      const std::uint8_t value = static_cast<std::uint8_t>(text[index + part]);
      if ((value & 0xC0U) != 0x80U) {
        return false;
      }
      codepoint = (codepoint << 6U) | (value & 0x3FU);
    }
    if ((length == 3 && codepoint < 0x800U) || (length == 4 && codepoint < 0x10000U) ||
        codepoint > 0x10FFFFU || (codepoint >= 0xD800U && codepoint <= 0xDFFFU)) {
      return false;
    }
    index += length;
  }
  return true;
}

int HexValue(char value) {
  if (value >= '0' && value <= '9') {
    return value - '0';
  }
  if (value >= 'a' && value <= 'f') {
    return value - 'a' + 10;
  }
  if (value >= 'A' && value <= 'F') {
    return value - 'A' + 10;
  }
  return -1;
}

bool PercentDecode(std::string_view value, std::string& output) {
  output.clear();
  output.reserve(value.size());
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (value[index] == '+') {
      output.push_back(' ');
      continue;
    }
    if (value[index] != '%') {
      output.push_back(value[index]);
      continue;
    }
    if (index + 2 >= value.size()) {
      return false;
    }
    const int high = HexValue(value[index + 1]);
    const int low = HexValue(value[index + 2]);
    if (high < 0 || low < 0) {
      return false;
    }
    output.push_back(static_cast<char>((high << 4) | low));
    index += 2;
  }
  return output.find('\0') == std::string::npos && IsValidUtf8(output);
}

bool ParseQuery(std::string_view text, std::unordered_map<std::string, std::string>& query,
                HttpError& error) {
  if (text.empty()) {
    return true;
  }
  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t end = text.find('&', start);
    const std::string_view pair = text.substr(
        start, end == std::string_view::npos ? text.size() - start : end - start);
    if (pair.empty() || query.size() >= kMaximumQueryParameters) {
      error = {400, "Bad Request", "invalid_query", "query contains empty or too many parameters"};
      return false;
    }
    const std::size_t equals = pair.find('=');
    std::string key;
    std::string value;
    if (!PercentDecode(pair.substr(0, equals), key) ||
        !PercentDecode(equals == std::string_view::npos ? std::string_view{} :
                                                            pair.substr(equals + 1),
                       value) ||
        key.empty() || query.find(key) != query.end()) {
      error = {400, "Bad Request", "invalid_query", "query encoding or duplicate parameter is invalid"};
      return false;
    }
    query.emplace(std::move(key), std::move(value));
    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1;
  }
  return true;
}

bool ParseTarget(std::string_view target, HttpRequest& request, HttpError& error) {
  if (target.empty() || target.size() > kMaximumRequestTargetBytes || target.front() != '/' ||
      target.find('#') != std::string_view::npos) {
    error = {414, "URI Too Long", "invalid_target", "request target is invalid or too long"};
    return false;
  }
  const std::size_t query_offset = target.find('?');
  const std::string_view path = target.substr(0, query_offset);
  if (!PercentDecode(path, request.path) || request.path.find('?') != std::string::npos) {
    error = {400, "Bad Request", "invalid_target", "request path encoding is invalid"};
    return false;
  }
  return query_offset == std::string_view::npos ||
         ParseQuery(target.substr(query_offset + 1), request.query, error);
}

bool ParseUnsigned(std::string_view text, std::uint64_t& output) {
  if (text.empty()) {
    return false;
  }
  output = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), output);
  return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

bool ReceiveRequest(SOCKET client, HttpRequest& request, HttpError& error) {
  std::string bytes;
  bytes.reserve(4096);
  std::array<char, 4096> buffer{};
  std::size_t header_end = std::string::npos;
  while (header_end == std::string::npos) {
    if (bytes.size() >= kMaximumHeaderBytes) {
      error = {431, "Request Header Fields Too Large", "headers_too_large",
               "request headers exceed 16384 bytes"};
      return false;
    }
    const int received = recv(client, buffer.data(), static_cast<int>(buffer.size()), 0);
    if (received <= 0) {
      error = {400, "Bad Request", "incomplete_request", "connection ended before headers completed"};
      return false;
    }
    bytes.append(buffer.data(), static_cast<std::size_t>(received));
    header_end = bytes.find("\r\n\r\n");
    if (header_end != std::string::npos && header_end + 4 > kMaximumHeaderBytes) {
      error = {431, "Request Header Fields Too Large", "headers_too_large",
               "request headers exceed 16384 bytes"};
      return false;
    }
  }

  const std::string_view headers(bytes.data(), header_end);
  const std::size_t line_end = headers.find("\r\n");
  const std::string_view line = headers.substr(0, line_end);
  const std::size_t first_space = line.find(' ');
  const std::size_t second_space = first_space == std::string_view::npos
                                       ? std::string_view::npos
                                       : line.find(' ', first_space + 1);
  if (line.size() > 4096 || first_space == std::string_view::npos ||
      second_space == std::string_view::npos || line.find(' ', second_space + 1) !=
                                                    std::string_view::npos) {
    error = {400, "Bad Request", "malformed_request_line", "request line must contain method, target and HTTP version"};
    return false;
  }
  request.method.assign(line.substr(0, first_space));
  request.version.assign(line.substr(second_space + 1));
  if (request.method.empty() || !std::all_of(request.method.begin(), request.method.end(),
                                             [](unsigned char value) {
                                               return value >= 'A' && value <= 'Z';
                                             }) ||
      (request.version != "HTTP/1.1" && request.version != "HTTP/1.0") ||
      !ParseTarget(line.substr(first_space + 1, second_space - first_space - 1), request,
                   error)) {
    if (error.code == "bad_request") {
      error = {400, "Bad Request", "malformed_request_line", "method, target or HTTP version is invalid"};
    }
    return false;
  }

  std::size_t cursor = line_end == std::string_view::npos ? headers.size() : line_end + 2;
  std::size_t header_count = 0;
  while (cursor < headers.size()) {
    const std::size_t end = headers.find("\r\n", cursor);
    const std::string_view header = headers.substr(
        cursor, end == std::string_view::npos ? headers.size() - cursor : end - cursor);
    const std::size_t colon = header.find(':');
    if (++header_count > 64 || colon == std::string_view::npos ||
        !IsHeaderName(header.substr(0, colon))) {
      error = {400, "Bad Request", "invalid_header", "request contains a malformed header"};
      return false;
    }
    const std::string name = Lower(header.substr(0, colon));
    const std::string_view value = Trim(header.substr(colon + 1));
    if (value.find('\0') != std::string_view::npos || request.headers.find(name) != request.headers.end()) {
      error = {400, "Bad Request", "duplicate_header", "duplicate or invalid headers are not accepted"};
      return false;
    }
    request.headers.emplace(name, std::string(value));
    if (end == std::string_view::npos) {
      break;
    }
    cursor = end + 2;
  }

  if (request.headers.find("transfer-encoding") != request.headers.end()) {
    error = {400, "Bad Request", "unsupported_transfer_encoding",
             "Transfer-Encoding is not supported; use Content-Length"};
    return false;
  }
  std::uint64_t content_length = 0;
  if (const auto found = request.headers.find("content-length");
      found != request.headers.end() && !ParseUnsigned(found->second, content_length)) {
    error = {400, "Bad Request", "invalid_content_length", "Content-Length must be an unsigned decimal integer"};
    return false;
  }
  if (content_length > kMaximumBodyBytes) {
    error = {413, "Payload Too Large", "body_too_large", "request body exceeds 4096 bytes"};
    return false;
  }

  const std::size_t body_offset = header_end + 4;
  if (bytes.size() - body_offset > content_length) {
    error = {400, "Bad Request", "unexpected_request_bytes", "request contains bytes beyond Content-Length"};
    return false;
  }
  while (bytes.size() - body_offset < content_length) {
    const std::size_t remaining = static_cast<std::size_t>(content_length) -
                                  (bytes.size() - body_offset);
    const int received = recv(client, buffer.data(),
                              static_cast<int>(std::min(remaining, buffer.size())), 0);
    if (received <= 0) {
      error = {400, "Bad Request", "incomplete_body", "connection ended before request body completed"};
      return false;
    }
    bytes.append(buffer.data(), static_cast<std::size_t>(received));
  }
  request.body.assign(bytes.data() + body_offset, static_cast<std::size_t>(content_length));
  return true;
}

bool OnlyQuery(const HttpRequest& request, std::initializer_list<std::string_view> allowed,
               HttpError& error) {
  for (const auto& [key, value] : request.query) {
    (void)value;
    if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) {
      error = {400, "Bad Request", "unknown_query_parameter", "endpoint received an unknown query parameter"};
      return false;
    }
  }
  return true;
}

bool QueryUnsigned(const HttpRequest& request, std::string_view name, std::uint64_t fallback,
                   std::uint64_t maximum, std::uint64_t& output, HttpError& error) {
  const auto found = request.query.find(std::string(name));
  if (found == request.query.end()) {
    output = fallback;
    return true;
  }
  if (!ParseUnsigned(found->second, output) || output > maximum) {
    error = {400, "Bad Request", "invalid_query_value", "numeric query parameter is invalid or out of range"};
    return false;
  }
  return true;
}

bool RequireEmptyBody(const HttpRequest& request, HttpError& error) {
  if (!request.body.empty()) {
    error = {400, "Bad Request", "unexpected_body", "GET and OPTIONS requests must not include a body"};
    return false;
  }
  return true;
}

void RouteGet(SOCKET client, const HttpRequest& request, std::uint64_t request_id) {
  HttpError error;
  if (!RequireEmptyBody(request, error)) {
    Respond(client, error.status, error.reason,
            ErrorJson(error.code, error.message, request_id), request_id);
    return;
  }
  if (request.path == "/health" && request.query.empty()) {
    Respond(client, 200, "OK", BuildHealthJson(), request_id);
  } else if (request.path == "/v1/state" && request.query.empty()) {
    Respond(client, 200, "OK", BuildSnapshotJson(), request_id);
  } else if (request.path == "/v1/player" && request.query.empty()) {
    Respond(client, 200, "OK", BuildPlayerJson(), request_id);
  } else if (request.path == "/v1/world" && request.query.empty()) {
    Respond(client, 200, "OK", BuildWorldJson(), request_id);
  } else if (request.path == "/v1/environment" && request.query.empty()) {
    Respond(client, 200, "OK", BuildEnvironmentJson(), request_id);
  } else if (request.path == "/v1/entities" && request.query.empty()) {
    Respond(client, 200, "OK", BuildEntitiesJson(), request_id);
  } else if (request.path == "/v1/room" && request.query.empty()) {
    Respond(client, 200, "OK", BuildRoomJson(), request_id);
  } else if (request.path == "/v1/outfits" && request.query.empty()) {
    Respond(client, 200, "OK", BuildOutfitCatalogJson(), request_id);
  } else if (request.path == "/v1/schema" && request.query.empty()) {
    Respond(client, 200, "OK", BuildSchemaJson(), request_id);
  } else if (request.path == "/v1/chat/status" && request.query.empty()) {
    Respond(client, 200, "OK", BuildChatStatusJson(), request_id);
  } else if (request.path == "/v1/objects") {
    std::uint64_t offset = 0;
    std::uint64_t limit = 100;
    if (!OnlyQuery(request, {"offset", "limit", "search"}, error) ||
        !QueryUnsigned(request, "offset", 0, 10000000, offset, error) ||
        !QueryUnsigned(request, "limit", 100, 256, limit, error)) {
      Respond(client, error.status, error.reason,
              ErrorJson(error.code, error.message, request_id), request_id);
      return;
    }
    const auto search = request.query.find("search");
    const std::string value = search == request.query.end() ? std::string{} : search->second;
    if (value.size() > 128) {
      Respond(client, 400, "Bad Request",
              ErrorJson("search_too_long", "search query exceeds 128 UTF-8 bytes", request_id),
              request_id);
      return;
    }
    Respond(client, 200, "OK", BuildObjectsJson(static_cast<std::size_t>(offset),
                                                  static_cast<std::size_t>(limit), value),
            request_id);
  } else if (request.path == "/v1/chat/messages") {
    std::uint64_t after = 0;
    std::uint64_t limit = 50;
    if (!OnlyQuery(request, {"after", "limit"}, error) ||
        !QueryUnsigned(request, "after", 0, std::numeric_limits<std::uint64_t>::max(),
                       after, error) ||
        !QueryUnsigned(request, "limit", 50, 100, limit, error)) {
      Respond(client, error.status, error.reason,
              ErrorJson(error.code, error.message, request_id), request_id);
      return;
    }
    Respond(client, 200, "OK", BuildChatMessagesJson(after, static_cast<std::size_t>(limit)),
            request_id);
  } else if (request.path.rfind("/v1/tasks/", 0) == 0 && request.query.empty()) {
    std::uint64_t id = 0;
    if (!ParseUnsigned(std::string_view(request.path).substr(10), id) || id == 0) {
      Respond(client, 400, "Bad Request",
              ErrorJson("invalid_task_id", "task ID must be a positive integer", request_id),
              request_id);
      return;
    }
    const auto json = BuildChatTaskJson(id);
    if (!json) {
      Respond(client, 404, "Not Found",
              ErrorJson("task_not_found", "task is unknown or has expired", request_id),
              request_id);
      return;
    }
    Respond(client, 200, "OK", *json, request_id);
  } else {
    Respond(client, 404, "Not Found",
            ErrorJson("unknown_endpoint", "no GET endpoint matches the request path", request_id),
            request_id);
  }
}

void RoutePost(SOCKET client, const HttpRequest& request, std::uint64_t request_id) {
  if (request.path != "/v1/chat/send") {
    Respond(client, 404, "Not Found",
            ErrorJson("unknown_endpoint", "no POST endpoint matches the request path", request_id),
            request_id);
    return;
  }
  if (!request.query.empty()) {
    Respond(client, 400, "Bad Request",
            ErrorJson("unexpected_query", "chat send does not accept query parameters", request_id),
            request_id);
    return;
  }
  const auto content_type = request.headers.find("content-type");
  const std::string lowered_content_type =
      content_type == request.headers.end() ? std::string{} : Lower(content_type->second);
  const std::size_t parameter = lowered_content_type.find(';');
  const std::string_view media_type = Trim(std::string_view(lowered_content_type).substr(
      0, parameter == std::string::npos ? lowered_content_type.size() : parameter));
  if (media_type != "application/json") {
    Respond(client, 415, "Unsupported Media Type",
            ErrorJson("invalid_content_type", "Content-Type must be application/json", request_id),
            request_id);
    return;
  }
  if (!IsValidUtf8(request.body)) {
    Respond(client, 400, "Bad Request",
            ErrorJson("invalid_utf8", "request body must be valid UTF-8", request_id), request_id);
    return;
  }
  const nlohmann::json body = nlohmann::json::parse(request.body, nullptr, false, true);
  if (body.is_discarded() || !body.is_object()) {
    Respond(client, 400, "Bad Request",
            ErrorJson("invalid_json", "request body must be a JSON object", request_id), request_id);
    return;
  }
  if (body.size() != 1 || body.find("message") == body.end() || !body["message"].is_string()) {
    Respond(client, 400, "Bad Request",
            ErrorJson("invalid_chat_request", "body must contain only one string field named message", request_id),
            request_id);
    return;
  }
  const ChatEnqueueResult queued = QueueChatMessage(body["message"].get<std::string>());
  if (!queued.accepted) {
    const int status = queued.error_code == "chat_not_ready" ? 503 :
                       queued.error_code == "rate_limited" || queued.error_code == "queue_full"
                           ? 429
                           : 400;
    const std::string_view reason = status == 503 ? "Service Unavailable" :
                                    status == 429 ? "Too Many Requests" : "Bad Request";
    std::string retry_header;
    if (queued.retry_after_ms != 0) {
      retry_header = "Retry-After: " +
                     std::to_string((queued.retry_after_ms + 999) / 1000) + "\r\n";
    }
    Respond(client, status, reason,
            ErrorJson(queued.error_code, queued.error, request_id, queued.retry_after_ms),
            request_id, retry_header);
    return;
  }
  nlohmann::json response = {
      {"accepted", true},
      {"taskId", queued.task_id},
      {"state", "queued"},
      {"queueDepth", queued.queue_depth},
      {"taskUrl", "/v1/tasks/" + std::to_string(queued.task_id)},
  };
  Respond(client, 202, "Accepted", response.dump(), request_id);
}

void HandleClient(SOCKET client) {
  DWORD timeout = 1500;
  setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
             sizeof(timeout));
  setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout),
             sizeof(timeout));
  const std::uint64_t request_id = g_request_id.fetch_add(1, std::memory_order_relaxed);
  HttpRequest request;
  HttpError error;
  if (!ReceiveRequest(client, request, error)) {
    Respond(client, error.status, error.reason,
            ErrorJson(error.code, error.message, request_id), request_id);
    return;
  }
  if (request.method == "OPTIONS") {
    if (!request.body.empty()) {
      Respond(client, 400, "Bad Request",
              ErrorJson("unexpected_body", "OPTIONS requests must not include a body", request_id),
              request_id);
    } else {
      Respond(client, 204, "No Content", {}, request_id);
    }
  } else if (request.method == "GET") {
    RouteGet(client, request, request_id);
  } else if (request.method == "POST") {
    RoutePost(client, request, request_id);
  } else {
    Respond(client, 405, "Method Not Allowed",
            ErrorJson("method_not_allowed", "only GET, POST and OPTIONS are supported", request_id),
            request_id, "Allow: GET, POST, OPTIONS\r\n");
  }
}

void WorkerMain() {
  while (true) {
    SOCKET client = INVALID_SOCKET;
    {
      std::unique_lock lock(g_client_mutex);
      g_client_ready.wait(lock, [] {
        return g_stop.load(std::memory_order_acquire) || !g_clients.empty();
      });
      if (g_clients.empty()) {
        if (g_stop.load(std::memory_order_acquire)) {
          return;
        }
        continue;
      }
      client = g_clients.front();
      g_clients.pop_front();
    }
    try {
      HandleClient(client);
    } catch (...) {
      const std::uint64_t request_id = g_request_id.fetch_add(1, std::memory_order_relaxed);
      Respond(client, 500, "Internal Server Error",
              ErrorJson("internal_error", "request handling failed safely", request_id), request_id);
    }
    shutdown(client, SD_BOTH);
    closesocket(client);
  }
}

void ServerMain() {
  WSADATA data{};
  if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
    return;
  }
  const SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listener == INVALID_SOCKET) {
    WSACleanup();
    return;
  }
  BOOL exclusive = TRUE;
  setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive),
             sizeof(exclusive));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(kHttpPort);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR ||
      listen(listener, static_cast<int>(kClientQueueCapacity)) == SOCKET_ERROR) {
    closesocket(listener);
    WSACleanup();
    return;
  }
  g_listen_socket.store(static_cast<std::uintptr_t>(listener), std::memory_order_release);
  for (auto& worker : g_workers) {
    worker = std::thread(WorkerMain);
  }

  while (!g_stop.load(std::memory_order_acquire)) {
    const SOCKET client = accept(listener, nullptr, nullptr);
    if (client == INVALID_SOCKET) {
      if (g_stop.load(std::memory_order_acquire)) {
        break;
      }
      continue;
    }
    bool queued = false;
    {
      std::scoped_lock lock(g_client_mutex);
      if (g_clients.size() < kClientQueueCapacity) {
        g_clients.push_back(client);
        queued = true;
      }
    }
    if (queued) {
      g_client_ready.notify_one();
    } else {
      const std::uint64_t request_id = g_request_id.fetch_add(1, std::memory_order_relaxed);
      Respond(client, 503, "Service Unavailable",
              ErrorJson("server_busy", "HTTP worker queue is full", request_id), request_id,
              "Retry-After: 1\r\n");
      shutdown(client, SD_BOTH);
      closesocket(client);
    }
  }

  const std::uintptr_t owned = g_listen_socket.exchange(
      static_cast<std::uintptr_t>(INVALID_SOCKET), std::memory_order_acq_rel);
  if (owned != static_cast<std::uintptr_t>(INVALID_SOCKET)) {
    closesocket(static_cast<SOCKET>(owned));
  }
  {
    std::scoped_lock lock(g_client_mutex);
    while (!g_clients.empty()) {
      shutdown(g_clients.front(), SD_BOTH);
      closesocket(g_clients.front());
      g_clients.pop_front();
    }
  }
  g_client_ready.notify_all();
  for (auto& worker : g_workers) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  WSACleanup();
}

}  // namespace

bool StartHttpServer() {
  if (g_server_thread.joinable()) {
    return true;
  }
  g_stop.store(false, std::memory_order_release);
  g_server_thread = std::thread(ServerMain);
  for (int attempt = 0; attempt < 50; ++attempt) {
    if (g_listen_socket.load(std::memory_order_acquire) !=
        static_cast<std::uintptr_t>(INVALID_SOCKET)) {
      return true;
    }
    Sleep(10);
  }
  return false;
}

void StopHttpServer() {
  g_stop.store(true, std::memory_order_release);
  const std::uintptr_t listener = g_listen_socket.exchange(
      static_cast<std::uintptr_t>(INVALID_SOCKET), std::memory_order_acq_rel);
  if (listener != static_cast<std::uintptr_t>(INVALID_SOCKET)) {
    shutdown(static_cast<SOCKET>(listener), SD_BOTH);
    closesocket(static_cast<SOCKET>(listener));
  }
  g_client_ready.notify_all();
  if (g_server_thread.joinable()) {
    g_server_thread.join();
  }
}

}  // namespace skyqoe
