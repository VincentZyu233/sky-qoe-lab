#include "loader_telemetry.h"

#include <iostream>
#include <string>

int main() {
  const std::string valid = R"({
    "version":"0.7.0",
    "player":{
      "valid":true,"avatarIndex":2,"avatarActive":1,"avatarFlags":428,
      "transform":{"position":[1.25,2.5,-3.75]},
      "slots":[
        {"effectiveId":1,"resourceName":"Body_A"},
        {"effectiveId":2,"resourceName":"Wing_B"}
      ]
    },
    "world":{
      "level":"CandleSpace","scanCycle":3,"scanStatus":"ready",
      "nearbyTransformCount":42,
      "room":{"current":1,"max":8,"players":[
        {"index":2,"local":true,"distance":0,"transform":{"position":[1,2,3]}}
      ]},
      "levelAssets":{"objects":10048,"properties":34665,"waxSpawners":13},
      "waxLoop":{"enabled":false},
      "localEffects":{"enabled":true}
    },
    "chat":{
      "captureHookInstalled":true,"gameThreadReady":true,
      "messagesStored":4,"messageCapacity":256,"queueDepth":1,"queueCapacity":8,
      "captured":7,"captureDropped":0,"submitted":2,"failed":0
    }
  })";

  const LoaderTelemetrySnapshot parsed = ParseLoaderTelemetryJson(valid);
  if (!parsed.connected || parsed.api_status.find(L"v0.7.0") == std::wstring::npos ||
      parsed.player.find(L"Avatar #2") == std::wstring::npos ||
      parsed.position.find(L"-3.75") == std::wstring::npos ||
      parsed.world.find(L"CandleSpace") == std::wstring::npos ||
      parsed.environment.find(L"10048") == std::wstring::npos ||
      parsed.automation.find(L"全特效 开启") == std::wstring::npos ||
      parsed.outfit.find(L"Body_A") == std::wstring::npos ||
      parsed.room.find(L"1 / 8") == std::wstring::npos ||
      parsed.chat.find(L"已提交 2") == std::wstring::npos) {
    std::cerr << "Valid telemetry JSON was not summarized correctly\n";
    return 1;
  }

  const LoaderTelemetrySnapshot malformed = ParseLoaderTelemetryJson("{broken");
  if (malformed.connected || malformed.api_status.find(L"JSON") == std::wstring::npos) {
    std::cerr << "Malformed telemetry JSON was not rejected safely\n";
    return 2;
  }

  const LoaderTelemetrySnapshot incomplete = ParseLoaderTelemetryJson(R"({"version":"x"})");
  if (incomplete.connected || incomplete.api_status.find(L"player/world") ==
                                  std::wstring::npos) {
    std::cerr << "Incomplete telemetry JSON was not rejected safely\n";
    return 3;
  }

  const LoaderTelemetrySnapshot wrong_types = ParseLoaderTelemetryJson(R"({
    "version":123,
    "player":{"valid":"yes","avatarIndex":[],"transform":{"position":[1,"bad",3]},
              "slots":[null,{"effectiveId":"bad","resourceName":42}]},
    "world":{"level":42,"room":{"players":[{"index":-1,"distance":"near"}]},
             "levelAssets":{"objects":"many"}},
    "chat":{"captured":"many"}
  })");
  if (!wrong_types.connected || wrong_types.position.find(L"等待有效") == std::wstring::npos ||
      wrong_types.world.find(L"识别中") == std::wstring::npos) {
    std::cerr << "Unexpected telemetry field types were not isolated safely\n";
    return 4;
  }

  const LoaderTelemetrySnapshot comments =
      ParseLoaderTelemetryJson(R"({"player":{},/* invalid */"world":{}})");
  if (comments.connected || comments.api_status.find(L"JSON") == std::wstring::npos) {
    std::cerr << "Non-standard commented JSON was not rejected\n";
    return 5;
  }

  return 0;
}
