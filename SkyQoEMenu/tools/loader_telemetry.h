#pragma once

#include <string>
#include <string_view>

struct LoaderTelemetrySnapshot {
  bool connected = false;
  std::wstring api_status;
  std::wstring player;
  std::wstring position;
  std::wstring world;
  std::wstring environment;
  std::wstring automation;
  std::wstring outfit;
  std::wstring room;
  std::wstring chat;
  std::wstring updated_at;
};

LoaderTelemetrySnapshot ParseLoaderTelemetryJson(std::string_view body);
LoaderTelemetrySnapshot FetchLoaderTelemetry();
