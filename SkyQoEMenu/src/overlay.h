#pragma once

#include <windows.h>

#include <cstdint>

namespace skyqoe {

DWORD WINAPI OverlayThread(void* module);
void RequestShutdown();
void ToggleMenu();
bool IsMenuVisible();

}  // namespace skyqoe

