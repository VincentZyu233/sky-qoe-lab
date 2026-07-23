#include "http_server.h"

#include "snapshot_json.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>

namespace skyqoe {
namespace {

constexpr unsigned short kHttpPort = 27891;
std::atomic<bool> g_stop{false};
std::atomic<std::uintptr_t> g_listen_socket{static_cast<std::uintptr_t>(INVALID_SOCKET)};
std::thread g_server_thread;

bool SendAll(SOCKET socket, const char* data, std::size_t size) {
  std::size_t sent = 0;
  while (sent < size) {
    const int chunk = send(socket, data + sent, static_cast<int>(size - sent), 0);
    if (chunk <= 0) {
      return false;
    }
    sent += static_cast<std::size_t>(chunk);
  }
  return true;
}

void Respond(SOCKET client, int status, std::string_view reason, const std::string& body) {
  std::string header = "HTTP/1.1 " + std::to_string(status) + " " + std::string(reason) +
                       "\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: " +
                       std::to_string(body.size()) +
                       "\r\nAccess-Control-Allow-Origin: *\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
  SendAll(client, header.data(), header.size());
  SendAll(client, body.data(), body.size());
}

void HandleClient(SOCKET client) {
  DWORD timeout = 1500;
  setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
             sizeof(timeout));
  std::array<char, 8192> buffer{};
  std::size_t used = 0;
  while (used < buffer.size() - 1) {
    const int received = recv(client, buffer.data() + used,
                              static_cast<int>(buffer.size() - 1 - used), 0);
    if (received <= 0) {
      return;
    }
    used += static_cast<std::size_t>(received);
    buffer[used] = '\0';
    if (std::string_view(buffer.data(), used).find("\r\n\r\n") != std::string_view::npos) {
      break;
    }
  }

  const std::string_view request(buffer.data(), used);
  const std::size_t line_end = request.find("\r\n");
  const std::string_view line = request.substr(0, line_end);
  const std::size_t first_space = line.find(' ');
  const std::size_t second_space =
      first_space == std::string_view::npos ? first_space : line.find(' ', first_space + 1);
  if (first_space == std::string_view::npos || second_space == std::string_view::npos) {
    Respond(client, 400, "Bad Request", "{\"error\":\"malformed request line\"}");
    return;
  }
  const std::string_view method = line.substr(0, first_space);
  std::string_view path = line.substr(first_space + 1, second_space - first_space - 1);
  if (const std::size_t query = path.find('?'); query != std::string_view::npos) {
    path = path.substr(0, query);
  }
  if (method != "GET") {
    Respond(client, 405, "Method Not Allowed", "{\"error\":\"read-only GET endpoint\"}");
    return;
  }
  if (path == "/health") {
    Respond(client, 200, "OK", BuildHealthJson());
  } else if (path == "/v1/state") {
    Respond(client, 200, "OK", BuildSnapshotJson());
  } else if (path == "/v1/player") {
    Respond(client, 200, "OK", BuildPlayerJson());
  } else if (path == "/v1/world") {
    Respond(client, 200, "OK", BuildWorldJson());
  } else if (path == "/v1/outfits") {
    Respond(client, 200, "OK", BuildOutfitCatalogJson());
  } else if (path == "/v1/schema") {
    Respond(client, 200, "OK", BuildSchemaJson());
  } else {
    Respond(client, 404, "Not Found", "{\"error\":\"unknown endpoint\"}");
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
      listen(listener, 8) == SOCKET_ERROR) {
    closesocket(listener);
    WSACleanup();
    return;
  }
  g_listen_socket.store(static_cast<std::uintptr_t>(listener), std::memory_order_release);

  while (!g_stop.load(std::memory_order_acquire)) {
    const SOCKET client = accept(listener, nullptr, nullptr);
    if (client == INVALID_SOCKET) {
      if (g_stop.load(std::memory_order_acquire)) {
        break;
      }
      continue;
    }
    HandleClient(client);
    shutdown(client, SD_BOTH);
    closesocket(client);
  }

  const std::uintptr_t owned = g_listen_socket.exchange(
      static_cast<std::uintptr_t>(INVALID_SOCKET), std::memory_order_acq_rel);
  if (owned != static_cast<std::uintptr_t>(INVALID_SOCKET)) {
    closesocket(static_cast<SOCKET>(owned));
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
  if (g_server_thread.joinable()) {
    g_server_thread.join();
  }
}

}  // namespace skyqoe
