#include "overlay.h"

#include "game_state.h"
#include "http_server.h"
#include "local_effects.h"

#include <d3d11.h>
#include <dwmapi.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND window, UINT message,
                                                             WPARAM w_param, LPARAM l_param);

namespace skyqoe {
namespace {

std::atomic<bool> g_shutdown{false};
std::atomic<bool> g_menu_visible{true};
HWND g_game_window = nullptr;
HWND g_overlay_window = nullptr;
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_device_context = nullptr;
IDXGISwapChain* g_swap_chain = nullptr;
ID3D11RenderTargetView* g_render_target = nullptr;
std::string g_ini_path;

struct FeatureToggle {
  const char* name;
  const char* category;
  bool enabled;
  bool supported;
};

std::array<FeatureToggle, 3> g_features = {{{u8"玩家监视", u8"监视", true, true},
                                           {u8"穿搭监视", u8"监视", true, true},
                                           {u8"坐标候选扫描", u8"诊断", true, true}}};

bool FeatureEnabled(const char* name) {
  for (const auto& feature : g_features) {
    if (std::strcmp(feature.name, name) == 0) {
      return feature.enabled && feature.supported;
    }
  }
  return false;
}

BOOL CALLBACK FindGameWindowCallback(HWND window, LPARAM parameter) {
  DWORD process_id = 0;
  GetWindowThreadProcessId(window, &process_id);
  if (process_id == GetCurrentProcessId() && IsWindowVisible(window) && GetWindow(window, GW_OWNER) == nullptr) {
    *reinterpret_cast<HWND*>(parameter) = window;
    return FALSE;
  }
  return TRUE;
}

HWND FindGameWindow() {
  HWND result = nullptr;
  EnumWindows(FindGameWindowCallback, reinterpret_cast<LPARAM>(&result));
  return result;
}

void CreateRenderTarget() {
  ID3D11Texture2D* back_buffer = nullptr;
  if (g_swap_chain && SUCCEEDED(g_swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer)))) {
    g_device->CreateRenderTargetView(back_buffer, nullptr, &g_render_target);
    back_buffer->Release();
  }
}

void CleanupRenderTarget() {
  if (g_render_target) {
    g_render_target->Release();
    g_render_target = nullptr;
  }
}

bool CreateDevice(HWND window) {
  DXGI_SWAP_CHAIN_DESC description{};
  description.BufferCount = 2;
  description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  description.OutputWindow = window;
  description.SampleDesc.Count = 1;
  description.Windowed = TRUE;
  description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
  D3D_FEATURE_LEVEL selected_level{};
  const HRESULT result = D3D11CreateDeviceAndSwapChain(
      nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &description,
      &g_swap_chain, &g_device, &selected_level, &g_device_context);
  if (FAILED(result)) {
    return false;
  }
  CreateRenderTarget();
  return g_render_target != nullptr;
}

void CleanupDevice() {
  CleanupRenderTarget();
  if (g_swap_chain) {
    g_swap_chain->Release();
    g_swap_chain = nullptr;
  }
  if (g_device_context) {
    g_device_context->Release();
    g_device_context = nullptr;
  }
  if (g_device) {
    g_device->Release();
    g_device = nullptr;
  }
}

void SetMenuInteraction(bool visible) {
  if (!g_overlay_window) {
    return;
  }
  LONG_PTR style = GetWindowLongPtrW(g_overlay_window, GWL_EXSTYLE);
  if (visible) {
    style &= ~(WS_EX_TRANSPARENT | WS_EX_NOACTIVATE);
  } else {
    style |= WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;
  }
  SetWindowLongPtrW(g_overlay_window, GWL_EXSTYLE, style);
  SetWindowPos(g_overlay_window, HWND_TOPMOST, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
  if (visible) {
    SetForegroundWindow(g_overlay_window);
  } else if (g_game_window) {
    SetForegroundWindow(g_game_window);
  }
}

LRESULT WINAPI OverlayWindowProcedure(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
  if (ImGui_ImplWin32_WndProcHandler(window, message, w_param, l_param)) {
    return TRUE;
  }
  switch (message) {
    case WM_SIZE:
      if (g_device && w_param != SIZE_MINIMIZED) {
        CleanupRenderTarget();
        g_swap_chain->ResizeBuffers(0, static_cast<UINT>(LOWORD(l_param)),
                                    static_cast<UINT>(HIWORD(l_param)), DXGI_FORMAT_UNKNOWN, 0);
        CreateRenderTarget();
      }
      return 0;
    case WM_SYSCOMMAND:
      if ((w_param & 0xFFF0U) == SC_KEYMENU) {
        return 0;
      }
      break;
    case WM_NCHITTEST:
      if (!g_menu_visible.load(std::memory_order_acquire)) {
        return HTTRANSPARENT;
      }
      break;
    case WM_DESTROY:
      g_shutdown.store(true, std::memory_order_release);
      PostQuitMessage(0);
      return 0;
    default:
      break;
  }
  return DefWindowProcW(window, message, w_param, l_param);
}

std::string FormatAddress(std::uint64_t address) {
  char buffer[32]{};
  std::snprintf(buffer, sizeof(buffer), "0x%llX", static_cast<unsigned long long>(address));
  return buffer;
}

void DrawStatusValue(const char* label, const std::string& value) {
  ImGui::TableNextRow();
  ImGui::TableSetColumnIndex(0);
  ImGui::TextUnformatted(label);
  ImGui::TableSetColumnIndex(1);
  ImGui::TextUnformatted(value.c_str());
}

void DrawFeatureTab(const GameSnapshot& snapshot) {
  if (ImGui::BeginTable("features", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                           ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn(u8"功能", ImGuiTableColumnFlags_WidthStretch, 2.0F);
    ImGui::TableSetupColumn(u8"分类", ImGuiTableColumnFlags_WidthStretch, 1.0F);
    ImGui::TableSetupColumn(u8"状态", ImGuiTableColumnFlags_WidthFixed, 90.0F);
    ImGui::TableHeadersRow();
    for (auto& feature : g_features) {
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::TextUnformatted(feature.name);
      ImGui::TableSetColumnIndex(1);
      ImGui::TextDisabled("%s", feature.category);
      ImGui::TableSetColumnIndex(2);
      ImGui::PushID(feature.name);
      ImGui::BeginDisabled(!feature.supported || !snapshot.build.supported);
      ImGui::Checkbox("##enabled", &feature.enabled);
      ImGui::EndDisabled();
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  GetGameState().SetCoordinateScanEnabled(FeatureEnabled(u8"坐标候选扫描"));

  ImGui::SeparatorText(u8"自动功能");
  bool wax_loop = GetGameState().WaxLoopEnabled();
  if (ImGui::Checkbox(u8"循环传到烛火", &wax_loop)) {
    GetGameState().SetWaxLoopEnabled(wax_loop);
  }
  std::uint32_t interval = GetGameState().WaxLoopInterval();
  ImGui::SetNextItemWidth(150.0F);
  if (ImGui::InputScalar(u8"烛火间隔 (ms)", ImGuiDataType_U32, &interval)) {
    GetGameState().SetWaxLoopInterval(interval);
  }
  ImGui::TextDisabled(u8"关卡 %s   目标 %u / 可用 %zu", snapshot.world.level.empty()
                                                               ? "-"
                                                               : snapshot.world.level.c_str(),
                      snapshot.world.wax_spawner_count,
                      static_cast<std::size_t>(std::count_if(
                          snapshot.world.wax_targets.begin(), snapshot.world.wax_targets.end(),
                          [](const auto& target) { return target.usable; })));
  ImGui::TextWrapped("%s", snapshot.world.wax_loop_status.c_str());

  ImGui::Spacing();
  bool effect_loop = LocalEffectLoopEnabled();
  ImGui::BeginDisabled(!snapshot.local_effects.hook_installed);
  if (ImGui::Checkbox(u8"循环生成全特效", &effect_loop)) {
    SetLocalEffectLoopEnabled(effect_loop);
  }
  ImGui::EndDisabled();
  std::uint32_t effect_interval = LocalEffectInterval();
  ImGui::SetNextItemWidth(150.0F);
  if (ImGui::InputScalar(u8"特效间隔 (ms)", ImGuiDataType_U32, &effect_interval)) {
    SetLocalEffectInterval(effect_interval);
  }
  ImGui::TextDisabled(u8"本地定义 %u / %u   进度 %u   已生成 %llu",
                      snapshot.local_effects.loaded_count,
                      snapshot.local_effects.catalog_count,
                      snapshot.local_effects.next_index,
                      static_cast<unsigned long long>(snapshot.local_effects.generated));
  ImGui::TextDisabled(u8"Emitter 池 %u / %u",
                      snapshot.local_effects.pool_active,
                      snapshot.local_effects.pool_capacity);
  ImGui::TextWrapped("%s", snapshot.local_effects.status.c_str());
}

void DrawTeleportControls(const GameSnapshot& snapshot) {
  static float distance = 1.0F;
  static std::string feedback;
  static bool feedback_success = false;

  ImGui::SeparatorText(u8"精准位移");
  ImGui::SetNextItemWidth(150.0F);
  ImGui::InputFloat(u8"距离", &distance, 0.1F, 1.0F, "%.3f");
  if (!std::isfinite(distance)) {
    distance = 1.0F;
  }
  distance = std::clamp(distance, 0.01F, 10000.0F);

  if (snapshot.transform.valid) {
    ImGui::TextDisabled("X %.3f   Y %.3f   Z %.3f", snapshot.transform.position[0],
                        snapshot.transform.position[1], snapshot.transform.position[2]);
  } else {
    ImGui::TextDisabled(u8"位置尚未就绪");
  }

  const auto teleport = [&](MoveDirection direction, const char* label) {
    std::string error;
    feedback_success = GetGameState().TeleportRelative(direction, distance, error);
    if (feedback_success) {
      char buffer[96]{};
      std::snprintf(buffer, sizeof(buffer), "%s %.3f", label, distance);
      feedback = buffer;
    } else {
      feedback = error;
    }
  };

  const ImVec2 button_size{88.0F, 34.0F};
  if (ImGui::BeginTable("teleport-buttons", 4, ImGuiTableFlags_SizingFixedFit)) {
    for (int column = 0; column < 4; ++column) {
      ImGui::TableSetupColumn(nullptr, ImGuiTableColumnFlags_WidthFixed, 96.0F);
    }
    ImGui::BeginDisabled(!snapshot.transform.valid);

    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(1);
    if (ImGui::Button(u8"向前", button_size)) {
      teleport(MoveDirection::Forward, u8"向前");
    }
    ImGui::TableSetColumnIndex(3);
    if (ImGui::Button(u8"向上", button_size)) {
      teleport(MoveDirection::Up, u8"向上");
    }

    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    if (ImGui::Button(u8"向左", button_size)) {
      teleport(MoveDirection::Left, u8"向左");
    }
    ImGui::TableSetColumnIndex(2);
    if (ImGui::Button(u8"向右", button_size)) {
      teleport(MoveDirection::Right, u8"向右");
    }
    ImGui::TableSetColumnIndex(3);
    if (ImGui::Button(u8"向下", button_size)) {
      teleport(MoveDirection::Down, u8"向下");
    }

    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(1);
    if (ImGui::Button(u8"向后", button_size)) {
      teleport(MoveDirection::Backward, u8"向后");
    }

    ImGui::EndDisabled();
    ImGui::EndTable();
  }

  if (!feedback.empty()) {
    ImGui::TextColored(feedback_success ? ImVec4(0.25F, 0.80F, 0.58F, 1.0F)
                                        : ImVec4(0.95F, 0.52F, 0.38F, 1.0F),
                       "%s", feedback.c_str());
  }
}

void DrawPlayerTab(const GameSnapshot& snapshot) {
  static char manager_input[32]{};
  static std::uint32_t raw_offset = 0;

  if (manager_input[0] == '\0' && snapshot.manager != 0) {
    std::snprintf(manager_input, sizeof(manager_input), "0x%llX",
                  static_cast<unsigned long long>(snapshot.manager));
  }
  ImGui::SetNextItemWidth(220.0F);
  ImGui::InputText("Manager", manager_input, sizeof(manager_input), ImGuiInputTextFlags_CharsHexadecimal);
  ImGui::SameLine();
  if (ImGui::Button(u8"应用")) {
    GetGameState().SetManager(std::strtoull(manager_input, nullptr, 0));
  }

  if (ImGui::BeginTable("player-status", 2,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
    ImGui::TableSetupColumn(u8"字段", ImGuiTableColumnFlags_WidthFixed, 150.0F);
    ImGui::TableSetupColumn(u8"值", ImGuiTableColumnFlags_WidthStretch);
    DrawStatusValue(u8"状态", snapshot.status);
    DrawStatusValue("Manager", FormatAddress(snapshot.manager));
    DrawStatusValue("Avatar", FormatAddress(snapshot.avatar));
    DrawStatusValue(u8"玩家槽位", std::to_string(snapshot.avatar_index));
    DrawStatusValue("Flags", FormatAddress(snapshot.avatar_flags));
    DrawStatusValue("Outfit", FormatAddress(snapshot.outfit));
    DrawStatusValue(u8"穿搭数据库", FormatAddress(snapshot.outfit_database));
    ImGui::EndTable();
  }

  DrawTeleportControls(snapshot);

  ImGui::SeparatorText(u8"坐标候选");
  if (snapshot.coordinate_candidates.empty()) {
    ImGui::TextDisabled(u8"等待移动样本");
  } else if (ImGui::BeginTable("coordinates", 5,
                               ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
    ImGui::TableSetupColumn(u8"偏移");
    ImGui::TableSetupColumn("X");
    ImGui::TableSetupColumn("Y");
    ImGui::TableSetupColumn("Z");
    ImGui::TableSetupColumn(u8"分数");
    ImGui::TableHeadersRow();
    for (const auto& candidate : snapshot.coordinate_candidates) {
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      if (ImGui::Selectable(FormatAddress(candidate.offset).c_str(), raw_offset == candidate.offset,
                            ImGuiSelectableFlags_SpanAllColumns)) {
        raw_offset = candidate.offset;
      }
      ImGui::TableSetColumnIndex(1);
      ImGui::Text("%.4f", candidate.value[0]);
      ImGui::TableSetColumnIndex(2);
      ImGui::Text("%.4f", candidate.value[1]);
      ImGui::TableSetColumnIndex(3);
      ImGui::Text("%.4f", candidate.value[2]);
      ImGui::TableSetColumnIndex(4);
      ImGui::Text("%.1f", candidate.score);
    }
    ImGui::EndTable();
  }

  ImGui::SetNextItemWidth(150.0F);
  ImGui::InputScalar(u8"原始偏移", ImGuiDataType_U32, &raw_offset, nullptr, nullptr, "0x%08X",
                     ImGuiInputTextFlags_CharsHexadecimal);
  if (snapshot.avatar != 0 && ImGui::BeginTable("raw-floats", 5, ImGuiTableFlags_RowBg)) {
    ImGui::TableSetupColumn(u8"偏移");
    ImGui::TableSetupColumn("F0");
    ImGui::TableSetupColumn("F1");
    ImGui::TableSetupColumn("F2");
    ImGui::TableSetupColumn("F3");
    for (std::uint32_t row = 0; row < 8; ++row) {
      std::array<float, 4> values{};
      const std::uint32_t offset = raw_offset + row * 0x10;
      if (!GetGameState().ReadFloat4(snapshot.avatar, offset, values)) {
        break;
      }
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::Text("0x%X", offset);
      for (int column = 0; column < 4; ++column) {
        ImGui::TableSetColumnIndex(column + 1);
        ImGui::Text("%.5g", values[static_cast<std::size_t>(column)]);
      }
    }
    ImGui::EndTable();
  }
}

void DrawOutfitTab(const GameSnapshot& snapshot) {
  if (!snapshot.valid) {
    ImGui::TextDisabled(u8"本地 Avatar 尚未就绪");
    return;
  }
  if (ImGui::BeginTable("outfit", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                        ImGuiTableFlags_ScrollY,
                        ImVec2(0, 390))) {
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(u8"槽位", ImGuiTableColumnFlags_WidthFixed, 58.0F);
    ImGui::TableSetupColumn(u8"类型", ImGuiTableColumnFlags_WidthFixed, 72.0F);
    ImGui::TableSetupColumn(u8"基础 ID", ImGuiTableColumnFlags_WidthFixed, 95.0F);
    ImGui::TableSetupColumn(u8"生效 ID", ImGuiTableColumnFlags_WidthFixed, 95.0F);
    ImGui::TableSetupColumn(u8"资源名", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (const auto& slot : snapshot.slots) {
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::Text("%u", slot.index);
      ImGui::TableSetColumnIndex(1);
      ImGui::TextUnformatted(slot.type.c_str());
      ImGui::TableSetColumnIndex(2);
      ImGui::Text("%u", slot.base_id);
      ImGui::TableSetColumnIndex(3);
      ImGui::Text("%u%s", slot.effective_id, slot.override_flag != 0 ? " *" : "");
      ImGui::TableSetColumnIndex(4);
      ImGui::TextUnformatted(slot.resource_name.empty() ? "-" : slot.resource_name.c_str());
    }
    ImGui::EndTable();
  }
}

void DrawDiagnosticsTab(const GameSnapshot& snapshot) {
  if (ImGui::BeginTable("diagnostics", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
    ImGui::TableSetupColumn(u8"字段", ImGuiTableColumnFlags_WidthFixed, 180.0F);
    ImGui::TableSetupColumn(u8"值", ImGuiTableColumnFlags_WidthStretch);
    DrawStatusValue(u8"模块基址", FormatAddress(snapshot.build.module_base));
    DrawStatusValue("PE Timestamp", FormatAddress(snapshot.build.timestamp));
    DrawStatusValue("SizeOfImage", FormatAddress(snapshot.build.image_size));
    DrawStatusValue(u8"构建兼容", snapshot.build.supported ? u8"是" : u8"否");
    DrawStatusValue(u8"菜单渲染", "D3D11 overlay HWND");
    DrawStatusValue(u8"游戏渲染", "Vulkan");
    DrawStatusValue("FPS", std::to_string(static_cast<int>(ImGui::GetIO().Framerate)));
    ImGui::EndTable();
  }
  ImGui::Spacing();
  if (ImGui::Button(u8"卸载模组", ImVec2(120, 32))) {
    RequestShutdown();
  }
}

void DrawWorldTab(const GameSnapshot& snapshot) {
  const auto& world = snapshot.world;
  if (ImGui::BeginTable("world-status", 2,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
    ImGui::TableSetupColumn(u8"字段", ImGuiTableColumnFlags_WidthFixed, 180.0F);
    ImGui::TableSetupColumn(u8"值", ImGuiTableColumnFlags_WidthStretch);
    DrawStatusValue(u8"关卡", world.level.empty() ? "-" : world.level);
    DrawStatusValue(u8"关卡来源", world.level_source.empty() ? "-" : world.level_source);
    DrawStatusValue(u8"游戏主对象", FormatAddress(world.root));
    DrawStatusValue(u8"Manager 来源", world.manager_source.empty() ? "-" : world.manager_source);
    const std::string room = std::to_string(world.room_current_players) + " / " +
                             (world.room_max_players == 0 ? "?" :
                              std::to_string(world.room_max_players));
    DrawStatusValue(u8"房间人数", room);
    DrawStatusValue(u8"Avatar 槽位容量", std::to_string(world.avatar_capacity));
    DrawStatusValue(u8"关卡对象", std::to_string(world.level_object_count));
    DrawStatusValue(u8"关卡属性", std::to_string(world.level_property_count));
    DrawStatusValue(u8"关卡来源文件", std::to_string(world.level_source_count));
    DrawStatusValue(u8"Wax 生成器", std::to_string(world.wax_spawner_count));
    DrawStatusValue(u8"运行时 Transform", std::to_string(world.transform_candidates));
    DrawStatusValue(u8"附近 Transform (100m)", std::to_string(world.nearby_transform_count));
    DrawStatusValue(u8"模块对象头候选", std::to_string(world.object_header_candidates));
    DrawStatusValue(u8"世界扫描", world.scan_status);
    ImGui::EndTable();
  }

  ImGui::SeparatorText(u8"Wax 目标");
  if (world.wax_targets.empty()) {
    ImGui::TextDisabled(u8"等待关卡资源解析");
  } else if (ImGui::BeginTable("wax-targets", 6,
                               ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                   ImGuiTableFlags_ScrollY,
                               ImVec2(0, 210))) {
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 36.0F);
    ImGui::TableSetupColumn("X");
    ImGui::TableSetupColumn("Y");
    ImGui::TableSetupColumn("Z");
    ImGui::TableSetupColumn(u8"网络", ImGuiTableColumnFlags_WidthFixed, 48.0F);
    ImGui::TableSetupColumn(u8"可用", ImGuiTableColumnFlags_WidthFixed, 48.0F);
    ImGui::TableHeadersRow();
    for (std::size_t index = 0; index < world.wax_targets.size(); ++index) {
      const auto& target = world.wax_targets[index];
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::Text("%zu", index + 1);
      for (int component = 0; component < 3; ++component) {
        ImGui::TableSetColumnIndex(component + 1);
        ImGui::Text("%.3f", target.position[static_cast<std::size_t>(component)]);
      }
      ImGui::TableSetColumnIndex(4);
      ImGui::TextUnformatted(target.networked ? u8"是" : u8"否");
      ImGui::TableSetColumnIndex(5);
      ImGui::TextUnformatted(target.usable ? u8"是" : u8"否");
    }
    ImGui::EndTable();
  }

  ImGui::SeparatorText(u8"附近 Transform");
  if (world.nearby_transforms.empty()) {
    ImGui::TextDisabled(u8"等待完整世界扫描");
  } else if (ImGui::BeginTable("nearby-transforms", 5,
                               ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                   ImGuiTableFlags_ScrollY,
                               ImVec2(0, 180))) {
    ImGui::TableSetupColumn(u8"地址", ImGuiTableColumnFlags_WidthFixed, 120.0F);
    ImGui::TableSetupColumn("X");
    ImGui::TableSetupColumn("Y");
    ImGui::TableSetupColumn("Z");
    ImGui::TableSetupColumn(u8"距离");
    ImGui::TableHeadersRow();
    for (const auto& item : world.nearby_transforms) {
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::TextUnformatted(FormatAddress(item.address).c_str());
      for (int component = 0; component < 3; ++component) {
        ImGui::TableSetColumnIndex(component + 1);
        ImGui::Text("%.2f", item.position[static_cast<std::size_t>(component)]);
      }
      ImGui::TableSetColumnIndex(4);
      ImGui::Text("%.1f", item.distance);
    }
    ImGui::EndTable();
  }
}

void DrawMenu(const GameSnapshot& snapshot) {
  if (!g_menu_visible.load(std::memory_order_acquire)) {
    return;
  }
  ImGui::SetNextWindowPos(ImVec2(22, 28), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(720, 650), ImGuiCond_FirstUseEver);
  bool open = true;
  if (!ImGui::Begin("Sky QoE", &open, ImGuiWindowFlags_NoCollapse)) {
    ImGui::End();
    return;
  }

  const ImVec4 ready_color = snapshot.valid ? ImVec4(0.25F, 0.80F, 0.58F, 1.0F)
                                              : ImVec4(0.95F, 0.62F, 0.22F, 1.0F);
  ImGui::TextColored(ready_color, "%s", snapshot.status.c_str());
  ImGui::SameLine(ImGui::GetWindowWidth() - 140.0F);
  ImGui::TextDisabled("v0.3.0");

  if (ImGui::BeginTabBar("main-tabs")) {
    if (ImGui::BeginTabItem(u8"功能")) {
      DrawFeatureTab(snapshot);
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(u8"玩家")) {
      DrawPlayerTab(snapshot);
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(u8"穿搭")) {
      DrawOutfitTab(snapshot);
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(u8"世界")) {
      DrawWorldTab(snapshot);
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(u8"诊断")) {
      DrawDiagnosticsTab(snapshot);
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }
  ImGui::End();
  if (!open) {
    g_menu_visible.store(false, std::memory_order_release);
    SetMenuInteraction(false);
  }
}

void DrawHud(const GameSnapshot& snapshot) {
  ImGui::SetNextWindowPos(ImVec2(14, 14), ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.62F);
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                 ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
  if (ImGui::Begin("SkyQoE-HUD", nullptr, flags)) {
    ImGui::TextColored(snapshot.valid ? ImVec4(0.25F, 0.85F, 0.60F, 1.0F)
                                      : ImVec4(0.95F, 0.64F, 0.25F, 1.0F),
                       "Sky QoE");
    ImGui::Text("Avatar %s", FormatAddress(snapshot.avatar).c_str());
    if (snapshot.transform.valid) {
      ImGui::Text("XYZ %.2f  %.2f  %.2f", snapshot.transform.position[0],
                  snapshot.transform.position[1], snapshot.transform.position[2]);
    }
    ImGui::Text(u8"房间 %u / %s   槽位 %u", snapshot.world.room_current_players,
                snapshot.world.room_max_players == 0
                    ? "?"
                    : std::to_string(snapshot.world.room_max_players).c_str(),
                snapshot.world.avatar_capacity);
    ImGui::Text(u8"关卡 %s   对象 %u   属性 %u",
                snapshot.world.level.empty() ? "-" : snapshot.world.level.c_str(),
                snapshot.world.level_object_count, snapshot.world.level_property_count);
    ImGui::Text(u8"附近 %llu   Transform %llu   Wax %u",
                static_cast<unsigned long long>(snapshot.world.nearby_transform_count),
                static_cast<unsigned long long>(snapshot.world.transform_candidates),
                snapshot.world.wax_spawner_count);
    ImGui::TextColored(snapshot.world.wax_loop_enabled
                           ? ImVec4(0.95F, 0.72F, 0.28F, 1.0F)
                           : ImVec4(0.58F, 0.62F, 0.66F, 1.0F),
                       u8"烛火循环 %s", snapshot.world.wax_loop_enabled ? u8"开" : u8"关");
    ImGui::SameLine();
    ImGui::TextColored(snapshot.local_effects.enabled
                           ? ImVec4(0.92F, 0.48F, 0.76F, 1.0F)
                           : ImVec4(0.58F, 0.62F, 0.66F, 1.0F),
                       u8"特效 %s", snapshot.local_effects.enabled ? u8"开" : u8"关");
  }
  ImGui::End();
}

void ConfigureStyle() {
  ImGuiStyle& style = ImGui::GetStyle();
  style.WindowRounding = 4.0F;
  style.ChildRounding = 4.0F;
  style.FrameRounding = 3.0F;
  style.PopupRounding = 4.0F;
  style.ScrollbarRounding = 3.0F;
  style.GrabRounding = 3.0F;
  style.WindowPadding = ImVec2(14, 12);
  style.FramePadding = ImVec2(9, 6);
  style.ItemSpacing = ImVec2(9, 7);

  ImVec4* colors = style.Colors;
  colors[ImGuiCol_WindowBg] = ImVec4(0.075F, 0.082F, 0.090F, 0.96F);
  colors[ImGuiCol_TitleBg] = ImVec4(0.10F, 0.11F, 0.12F, 1.0F);
  colors[ImGuiCol_TitleBgActive] = ImVec4(0.13F, 0.15F, 0.16F, 1.0F);
  colors[ImGuiCol_FrameBg] = ImVec4(0.14F, 0.15F, 0.16F, 1.0F);
  colors[ImGuiCol_FrameBgHovered] = ImVec4(0.20F, 0.23F, 0.24F, 1.0F);
  colors[ImGuiCol_FrameBgActive] = ImVec4(0.24F, 0.31F, 0.30F, 1.0F);
  colors[ImGuiCol_Button] = ImVec4(0.17F, 0.23F, 0.22F, 1.0F);
  colors[ImGuiCol_ButtonHovered] = ImVec4(0.23F, 0.38F, 0.34F, 1.0F);
  colors[ImGuiCol_ButtonActive] = ImVec4(0.25F, 0.47F, 0.40F, 1.0F);
  colors[ImGuiCol_CheckMark] = ImVec4(0.35F, 0.86F, 0.65F, 1.0F);
  colors[ImGuiCol_Header] = ImVec4(0.19F, 0.27F, 0.26F, 1.0F);
  colors[ImGuiCol_HeaderHovered] = ImVec4(0.25F, 0.40F, 0.36F, 1.0F);
  colors[ImGuiCol_TabSelected] = ImVec4(0.22F, 0.36F, 0.33F, 1.0F);
  colors[ImGuiCol_Separator] = ImVec4(0.25F, 0.28F, 0.29F, 1.0F);
}

std::string BuildIniPath() {
  char local_app_data[MAX_PATH]{};
  const DWORD length = GetEnvironmentVariableA("LOCALAPPDATA", local_app_data, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) {
    return "SkyQoE-imgui.ini";
  }
  const std::string directory = std::string(local_app_data) + "\\SkyQoE";
  CreateDirectoryA(directory.c_str(), nullptr);
  return directory + "\\imgui.ini";
}

bool CreateOverlayWindow(HINSTANCE instance) {
  WNDCLASSEXW window_class{};
  window_class.cbSize = sizeof(window_class);
  window_class.style = CS_CLASSDC;
  window_class.lpfnWndProc = OverlayWindowProcedure;
  window_class.hInstance = instance;
  window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
  window_class.lpszClassName = L"SkyQoEOverlayWindow";
  if (!RegisterClassExW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    return false;
  }

  RECT client{};
  GetClientRect(g_game_window, &client);
  POINT origin{client.left, client.top};
  ClientToScreen(g_game_window, &origin);
  const int width = client.right - client.left;
  const int height = client.bottom - client.top;
  g_overlay_window = CreateWindowExW(
      WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW, window_class.lpszClassName, L"Sky QoE",
      WS_POPUP, origin.x, origin.y, width, height, g_game_window, nullptr, instance, nullptr);
  if (!g_overlay_window) {
    return false;
  }
  SetLayeredWindowAttributes(g_overlay_window, RGB(0, 0, 0), 255, LWA_COLORKEY | LWA_ALPHA);
  const MARGINS margins{-1, -1, -1, -1};
  DwmExtendFrameIntoClientArea(g_overlay_window, &margins);
  ShowWindow(g_overlay_window, SW_SHOW);
  UpdateWindow(g_overlay_window);
  return true;
}

void SyncOverlayToGame() {
  if (!g_game_window || !g_overlay_window) {
    return;
  }
  const HWND foreground = GetForegroundWindow();
  const bool active = foreground == g_game_window || foreground == g_overlay_window;
  if (!active || IsIconic(g_game_window) || !IsWindowVisible(g_game_window)) {
    ShowWindow(g_overlay_window, SW_HIDE);
    return;
  }
  ShowWindow(g_overlay_window, SW_SHOWNA);
  RECT client{};
  GetClientRect(g_game_window, &client);
  POINT origin{client.left, client.top};
  ClientToScreen(g_game_window, &origin);
  SetWindowPos(g_overlay_window, HWND_TOPMOST, origin.x, origin.y, client.right - client.left,
               client.bottom - client.top, SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

}  // namespace

void RequestShutdown() {
  g_shutdown.store(true, std::memory_order_release);
}

void ToggleMenu() {
  const bool visible = !g_menu_visible.load(std::memory_order_acquire);
  g_menu_visible.store(visible, std::memory_order_release);
  SetMenuInteraction(visible);
}

bool IsMenuVisible() {
  return g_menu_visible.load(std::memory_order_acquire);
}

DWORD WINAPI OverlayThread(void* module_pointer) {
  const auto module = static_cast<HMODULE>(module_pointer);
  for (int attempt = 0; attempt < 300 && !g_shutdown.load(std::memory_order_acquire); ++attempt) {
    g_game_window = FindGameWindow();
    if (g_game_window) {
      break;
    }
    Sleep(100);
  }
  if (!g_game_window || !CreateOverlayWindow(module) || !CreateDevice(g_overlay_window)) {
    if (g_overlay_window) {
      DestroyWindow(g_overlay_window);
    }
    FreeLibraryAndExitThread(module, 1);
  }

  InitializeLocalEffects();
  StartHttpServer();

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  g_ini_path = BuildIniPath();
  io.IniFilename = g_ini_path.c_str();
  ImFont* font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\msyh.ttc", 18.0F, nullptr,
                                             io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
  if (!font) {
    io.Fonts->AddFontDefault();
  }
  ConfigureStyle();
  ImGui_ImplWin32_Init(g_overlay_window);
  ImGui_ImplDX11_Init(g_device, g_device_context);
  SetMenuInteraction(true);

  auto last_refresh = std::chrono::steady_clock::now() - std::chrono::seconds(1);
  bool insert_down = false;
  bool end_down = false;
  MSG message{};
  while (!g_shutdown.load(std::memory_order_acquire)) {
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }

    const bool current_insert = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
    if (current_insert && !insert_down) {
      ToggleMenu();
    }
    insert_down = current_insert;
    const bool current_end = (GetAsyncKeyState(VK_END) & 0x8000) != 0;
    if (current_end && !end_down) {
      RequestShutdown();
    }
    end_down = current_end;

    SyncOverlayToGame();
    const auto now = std::chrono::steady_clock::now();
    if (now - last_refresh >= std::chrono::milliseconds(100)) {
      GetGameState().Refresh();
      last_refresh = now;
    }
    GetGameState().TickAutomation();
    const GameSnapshot snapshot = GetGameState().Snapshot();

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    DrawHud(snapshot);
    DrawMenu(snapshot);
    ImGui::Render();

    constexpr float clear_color[4] = {0.0F, 0.0F, 0.0F, 0.0F};
    g_device_context->OMSetRenderTargets(1, &g_render_target, nullptr);
    g_device_context->ClearRenderTargetView(g_render_target, clear_color);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_swap_chain->Present(1, 0);
  }

  StopHttpServer();
  ShutdownLocalEffects();
  ImGui_ImplDX11_Shutdown();
  ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext();
  CleanupDevice();
  if (g_overlay_window) {
    DestroyWindow(g_overlay_window);
    g_overlay_window = nullptr;
  }
  UnregisterClassW(L"SkyQoEOverlayWindow", module);
  FreeLibraryAndExitThread(module, 0);
}

}  // namespace skyqoe
