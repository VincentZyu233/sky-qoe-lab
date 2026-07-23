#include <windows.h>
#include <shellapi.h>

#include <string>

namespace {

HMODULE g_module = nullptr;

LRESULT CALLBACK HarnessWindowProcedure(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
  switch (message) {
    case WM_PAINT: {
      PAINTSTRUCT paint{};
      const HDC context = BeginPaint(window, &paint);
      RECT client{};
      GetClientRect(window, &client);
      FillRect(context, &client, reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
      EndPaint(window, &paint);
      return 0;
    }
    case WM_DESTROY:
      if (g_module && GetModuleHandleW(L"SkyQoEMenu.dll")) {
        using RequestShutdown = unsigned long long(__stdcall*)(unsigned long long);
        const auto shutdown = reinterpret_cast<RequestShutdown>(
            GetProcAddress(g_module, "SkyQoE_RequestShutdown"));
        if (shutdown) {
          shutdown(0);
        }
      }
      PostQuitMessage(0);
      return 0;
    default:
      return DefWindowProcW(window, message, w_param, l_param);
  }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
  WNDCLASSEXW window_class{};
  window_class.cbSize = sizeof(window_class);
  window_class.lpfnWndProc = HarnessWindowProcedure;
  window_class.hInstance = instance;
  window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
  window_class.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
  window_class.lpszClassName = L"SkyQoEMenuHarnessWindow";
  if (!RegisterClassExW(&window_class)) {
    return 1;
  }

  const HWND window = CreateWindowExW(0, window_class.lpszClassName, L"Sky QoE Menu Harness",
                                      WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1280, 720,
                                      nullptr, nullptr, instance, nullptr);
  if (!window) {
    return 2;
  }
  ShowWindow(window, show_command);
  UpdateWindow(window);

  int argument_count = 0;
  LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
  const std::wstring dll_path = arguments && argument_count > 1 ? arguments[1] : L"SkyQoEMenu.dll";
  if (arguments) {
    LocalFree(arguments);
  }
  g_module = LoadLibraryW(dll_path.c_str());
  if (!g_module) {
    const DWORD error = GetLastError();
    wchar_t system_message[512]{};
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error, 0,
                   system_message, 512, nullptr);
    const std::wstring message = L"SkyQoEMenu.dll could not be loaded\nError " +
                                 std::to_wstring(error) + L": " + system_message +
                                 L"\nPath: " + dll_path;
    MessageBoxW(window, message.c_str(), L"Harness", MB_ICONERROR);
    DestroyWindow(window);
    return 3;
  }

  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  return 0;
}
