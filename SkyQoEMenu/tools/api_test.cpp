#include <windows.h>

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

int main(int argument_count, char** arguments) {
  const char* dll_path = argument_count > 1 ? arguments[1] : "SkyQoEMenu.dll";
  const HMODULE module = LoadLibraryA(dll_path);
  if (!module) {
    std::cerr << "LoadLibrary failed: " << GetLastError() << '\n';
    return 1;
  }

  using UnaryExport = std::uint64_t(__stdcall*)(std::uint64_t);
  using CopySnapshot = std::uint64_t(__stdcall*)(char*, std::uint64_t);
  const auto get_version =
      reinterpret_cast<UnaryExport>(GetProcAddress(module, "SkyQoE_GetVersion"));
  const auto copy_snapshot =
      reinterpret_cast<CopySnapshot>(GetProcAddress(module, "SkyQoE_CopySnapshotJson"));
  const auto request_shutdown =
      reinterpret_cast<UnaryExport>(GetProcAddress(module, "SkyQoE_RequestShutdown"));
  if (!get_version || !copy_snapshot || !request_shutdown) {
    std::cerr << "Required export is missing\n";
    return 2;
  }
  if (get_version(0) != 0x00010001) {
    std::cerr << "Unexpected DLL version\n";
    return 3;
  }

  const std::uint64_t required = copy_snapshot(nullptr, 0);
  if (required <= 1 || required > 1024 * 1024) {
    std::cerr << "Invalid JSON size: " << required << '\n';
    return 4;
  }
  std::vector<char> buffer(static_cast<std::size_t>(required));
  if (copy_snapshot(buffer.data(), buffer.size()) != required) {
    std::cerr << "JSON copy failed\n";
    return 5;
  }

  const std::string json(buffer.data());
  if (json.empty() || json.front() != '{' || json.back() != '}' ||
      json.find("\"version\":\"0.1.1\"") == std::string::npos ||
      json.find("\"slots\":[") == std::string::npos ||
      json.find("\"coordinateCandidates\":[") == std::string::npos) {
    std::cerr << "JSON structure check failed\n";
    return 6;
  }

  std::cout << json << '\n';
  request_shutdown(0);
  Sleep(200);
  return 0;
}
