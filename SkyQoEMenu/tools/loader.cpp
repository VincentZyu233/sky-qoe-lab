#include "game_state.h"
#include "loader_telemetry.h"

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr std::uint16_t kPayloadResourceId = 101;
constexpr wchar_t kTargetProcessName[] = L"Sky.exe";
constexpr wchar_t kPayloadModuleName[] = L"SkyQoEMenu.dll";
constexpr char kShutdownExportAscii[] = "SkyQoE_RequestShutdown";
constexpr char kVersionExportAscii[] = "SkyQoE_GetVersion";
constexpr DWORD kRemoteThreadTimeoutMs = 15000;
constexpr DWORD kModuleUnloadTimeoutMs = 15000;
constexpr wchar_t kLoaderWindowClass[] = L"SkyQoELoaderWindow";
constexpr wchar_t kLoaderWindowTitle[] = L"Sky QoE Loader";
constexpr wchar_t kGuiMutexName[] = L"Local\\SkyQoELoader.Gui.SingleInstance";
constexpr wchar_t kActionMutexName[] = L"Local\\SkyQoELoader.Action.SingleInstance";
constexpr UINT_PTR kProcessRefreshTimer = 1;
constexpr UINT kProcessRefreshIntervalMs = 1000;
constexpr UINT kActionCompletedMessage = WM_APP + 1;
constexpr UINT kTelemetryUpdatedMessage = WM_APP + 2;

enum class LoadMode {
  kInjectOnly,
  kReloadOnly,
  kInjectOrReload,
};

enum ControlId : int {
  kInjectButtonId = 1001,
  kReloadButtonId = 1002,
};

class UniqueHandle {
 public:
  UniqueHandle() = default;
  explicit UniqueHandle(HANDLE handle) : handle_(handle) {}
  ~UniqueHandle() { Reset(); }

  UniqueHandle(const UniqueHandle&) = delete;
  UniqueHandle& operator=(const UniqueHandle&) = delete;

  UniqueHandle(UniqueHandle&& other) noexcept : handle_(other.Release()) {}
  UniqueHandle& operator=(UniqueHandle&& other) noexcept {
    if (this != &other) {
      Reset(other.Release());
    }
    return *this;
  }

  HANDLE Get() const { return handle_; }
  explicit operator bool() const { return handle_ && handle_ != INVALID_HANDLE_VALUE; }

  HANDLE Release() {
    const HANDLE value = handle_;
    handle_ = nullptr;
    return value;
  }

  void Reset(HANDLE next = nullptr) {
    if (handle_ && handle_ != INVALID_HANDLE_VALUE) {
      CloseHandle(handle_);
    }
    handle_ = next;
  }

 private:
  HANDLE handle_ = nullptr;
};

struct ModuleInfo {
  std::wstring name;
  std::wstring path;
  std::uintptr_t base = 0;
  std::uint32_t size = 0;
};

struct TargetProcess {
  DWORD pid = 0;
  UniqueHandle handle;
  ModuleInfo executable;
};

struct EmbeddedPayload {
  std::vector<std::byte> bytes;
  std::uint64_t hash = 0;
  std::uint32_t timestamp = 0;
  std::uint32_t image_size = 0;
};

struct RunResult {
  int exit_code = 1;
  bool success = false;
  std::wstring message;
};

struct ProcessProbe {
  bool running = false;
  bool readable = false;
  bool build_supported = false;
  bool menu_loaded = false;
  DWORD pid = 0;
  std::uintptr_t module_base = 0;
  std::uintptr_t entry_point = 0;
  std::uintptr_t menu_base = 0;
  std::uint32_t timestamp = 0;
  std::uint32_t image_size = 0;
  std::wstring detail;
};

struct LoaderWindowState {
  HWND window = nullptr;
  HWND process_status = nullptr;
  HWND pid_value = nullptr;
  HWND module_base_value = nullptr;
  HWND entry_point_value = nullptr;
  HWND menu_status_value = nullptr;
  HWND build_value = nullptr;
  HWND inject_button = nullptr;
  HWND reload_button = nullptr;
  HWND activity = nullptr;
  HWND api_status = nullptr;
  HWND telemetry_updated = nullptr;
  HWND player_status = nullptr;
  HWND position_status = nullptr;
  HWND world_status = nullptr;
  HWND environment_status = nullptr;
  HWND automation_status = nullptr;
  HWND outfit_summary = nullptr;
  HWND room_summary = nullptr;
  HWND chat_summary = nullptr;
  HFONT normal_font = nullptr;
  HFONT heading_font = nullptr;
  HFONT small_font = nullptr;
  HBRUSH window_brush = nullptr;
  COLORREF status_color = RGB(92, 99, 112);
  COLORREF api_color = RGB(92, 99, 112);
  UINT dpi = 96;
  bool busy = false;
  bool payload_ready = false;
  std::atomic<bool> telemetry_stop{false};
  std::thread telemetry_thread;
  ProcessProbe probe;
};

struct ActionRequest {
  HWND window = nullptr;
  LoadMode mode = LoadMode::kInjectOnly;
};

std::wstring Hex(std::uint64_t value, int width = 0) {
  std::wostringstream output;
  output << L"0x" << std::uppercase << std::hex << std::setfill(L'0');
  if (width > 0) {
    output << std::setw(width);
  }
  output << value;
  return output.str();
}

std::wstring FormatWindowsError(DWORD error) {
  wchar_t* buffer = nullptr;
  const DWORD length = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, error, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
  std::wstring message = length && buffer ? std::wstring(buffer, length) : L"unknown error";
  if (buffer) {
    LocalFree(buffer);
  }
  while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n')) {
    message.pop_back();
  }
  return message + L" (" + std::to_wstring(error) + L")";
}

bool EqualsIgnoreCase(std::wstring_view left, std::wstring_view right) {
  if (left.size() != right.size()) {
    return false;
  }
  return CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(),
                              static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

std::wstring BaseName(const std::wstring& path) {
  const std::size_t separator = path.find_last_of(L"\\/");
  return separator == std::wstring::npos ? path : path.substr(separator + 1);
}

bool CheckedRange(std::size_t offset, std::size_t length, std::size_t total) {
  return offset <= total && length <= total - offset;
}

template <typename T>
const T* ViewAt(const std::vector<std::byte>& bytes, std::size_t offset) {
  return CheckedRange(offset, sizeof(T), bytes.size())
             ? reinterpret_cast<const T*>(bytes.data() + offset)
             : nullptr;
}

struct PeView {
  const IMAGE_NT_HEADERS64* nt = nullptr;
  const IMAGE_SECTION_HEADER* sections = nullptr;
  std::uint16_t section_count = 0;
};

bool ParsePe(const std::vector<std::byte>& bytes, PeView& view, std::wstring& error) {
  const IMAGE_DOS_HEADER* dos = ViewAt<IMAGE_DOS_HEADER>(bytes, 0);
  if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0) {
    error = L"内嵌 DLL 缺少有效的 DOS 头喵。";
    return false;
  }

  const std::size_t nt_offset = static_cast<std::size_t>(dos->e_lfanew);
  const IMAGE_NT_HEADERS64* nt = ViewAt<IMAGE_NT_HEADERS64>(bytes, nt_offset);
  if (!nt || nt->Signature != IMAGE_NT_SIGNATURE ||
      nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
      nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
    error = L"内嵌 payload 不是有效的 Win64 PE DLL 喵。";
    return false;
  }
  if ((nt->FileHeader.Characteristics & IMAGE_FILE_DLL) == 0 ||
      nt->FileHeader.NumberOfSections == 0 || nt->FileHeader.NumberOfSections > 96) {
    error = L"内嵌 payload 的 PE 属性异常喵。";
    return false;
  }

  const std::size_t section_offset =
      nt_offset + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) +
      nt->FileHeader.SizeOfOptionalHeader;
  const std::size_t section_bytes =
      static_cast<std::size_t>(nt->FileHeader.NumberOfSections) *
      sizeof(IMAGE_SECTION_HEADER);
  if (!CheckedRange(section_offset, section_bytes, bytes.size())) {
    error = L"内嵌 payload 的节表越界喵。";
    return false;
  }

  view.nt = nt;
  view.sections = reinterpret_cast<const IMAGE_SECTION_HEADER*>(bytes.data() + section_offset);
  view.section_count = nt->FileHeader.NumberOfSections;
  return true;
}

bool RvaToOffset(const std::vector<std::byte>& bytes, const PeView& pe, std::uint32_t rva,
                 std::size_t length, std::size_t& offset) {
  if (rva < pe.nt->OptionalHeader.SizeOfHeaders) {
    offset = rva;
    return CheckedRange(offset, length, bytes.size());
  }

  for (std::uint16_t index = 0; index < pe.section_count; ++index) {
    const IMAGE_SECTION_HEADER& section = pe.sections[index];
    const std::uint64_t start = section.VirtualAddress;
    const std::uint64_t end = start + section.SizeOfRawData;
    if (rva < start || static_cast<std::uint64_t>(rva) + length > end) {
      continue;
    }
    const std::uint64_t raw = static_cast<std::uint64_t>(section.PointerToRawData) +
                              (static_cast<std::uint64_t>(rva) - start);
    if (raw > static_cast<std::uint64_t>(SIZE_MAX)) {
      return false;
    }
    offset = static_cast<std::size_t>(raw);
    return CheckedRange(offset, length, bytes.size());
  }
  return false;
}

bool ReadAsciiAtRva(const std::vector<std::byte>& bytes, const PeView& pe,
                    std::uint32_t rva, std::string& value) {
  std::size_t offset = 0;
  if (!RvaToOffset(bytes, pe, rva, 1, offset)) {
    return false;
  }
  value.clear();
  for (std::size_t index = offset; index < bytes.size() && value.size() < 512; ++index) {
    const char character = static_cast<char>(bytes[index]);
    if (character == '\0') {
      return true;
    }
    value.push_back(character);
  }
  return false;
}

bool HasExport(const std::vector<std::byte>& bytes, const PeView& pe,
               std::string_view wanted) {
  const IMAGE_DATA_DIRECTORY& directory =
      pe.nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
  if (!directory.VirtualAddress || directory.Size < sizeof(IMAGE_EXPORT_DIRECTORY)) {
    return false;
  }

  std::size_t export_offset = 0;
  if (!RvaToOffset(bytes, pe, directory.VirtualAddress, sizeof(IMAGE_EXPORT_DIRECTORY),
                   export_offset)) {
    return false;
  }
  const IMAGE_EXPORT_DIRECTORY* exports =
      ViewAt<IMAGE_EXPORT_DIRECTORY>(bytes, export_offset);
  if (!exports || exports->NumberOfNames > 65536) {
    return false;
  }

  std::size_t names_offset = 0;
  if (!RvaToOffset(bytes, pe, exports->AddressOfNames,
                   static_cast<std::size_t>(exports->NumberOfNames) * sizeof(std::uint32_t),
                   names_offset)) {
    return false;
  }
  const auto* names =
      reinterpret_cast<const std::uint32_t*>(bytes.data() + names_offset);
  for (std::uint32_t index = 0; index < exports->NumberOfNames; ++index) {
    std::string name;
    if (ReadAsciiAtRva(bytes, pe, names[index], name) && name == wanted) {
      return true;
    }
  }
  return false;
}

std::uint64_t HashPayload(const std::vector<std::byte>& bytes) {
  std::uint64_t hash = 14695981039346656037ULL;
  for (const std::byte value : bytes) {
    hash ^= static_cast<std::uint8_t>(value);
    hash *= 1099511628211ULL;
  }
  return hash;
}

bool LoadEmbeddedPayload(EmbeddedPayload& payload, std::wstring& error) {
  const HMODULE executable = GetModuleHandleW(nullptr);
  const HRSRC resource = FindResourceW(executable, MAKEINTRESOURCEW(kPayloadResourceId),
                                      RT_RCDATA);
  if (!resource) {
    error = L"加载器中没有找到内嵌的 SkyQoEMenu.dll 资源喵。";
    return false;
  }
  const DWORD size = SizeofResource(executable, resource);
  const HGLOBAL loaded = LoadResource(executable, resource);
  const void* data = loaded ? LockResource(loaded) : nullptr;
  if (!data || size < sizeof(IMAGE_DOS_HEADER)) {
    error = L"无法读取加载器中的菜单 DLL 资源喵。";
    return false;
  }

  payload.bytes.resize(size);
  std::memcpy(payload.bytes.data(), data, size);
  PeView pe;
  if (!ParsePe(payload.bytes, pe, error)) {
    return false;
  }
  if (!HasExport(payload.bytes, pe, kVersionExportAscii) ||
      !HasExport(payload.bytes, pe, kShutdownExportAscii)) {
    error = L"内嵌菜单 DLL 缺少版本或安全卸载导出喵。";
    return false;
  }

  payload.hash = HashPayload(payload.bytes);
  payload.timestamp = pe.nt->FileHeader.TimeDateStamp;
  payload.image_size = pe.nt->OptionalHeader.SizeOfImage;
  return true;
}

bool EnsureDirectory(const std::wstring& path, std::wstring& error) {
  if (CreateDirectoryW(path.c_str(), nullptr)) {
    return true;
  }
  const DWORD code = GetLastError();
  if (code == ERROR_ALREADY_EXISTS) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
      return true;
    }
  }
  error = L"无法创建目录 " + path + L"：" + FormatWindowsError(code) + L" 喵。";
  return false;
}

bool ReadFileBytes(const std::wstring& path, std::vector<std::byte>& bytes) {
  UniqueHandle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!file) {
    return false;
  }
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(file.Get(), &size) || size.QuadPart < 0 ||
      static_cast<std::uint64_t>(size.QuadPart) > SIZE_MAX) {
    return false;
  }
  bytes.resize(static_cast<std::size_t>(size.QuadPart));
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const DWORD chunk = static_cast<DWORD>(
        std::min<std::size_t>(bytes.size() - offset, 4 * 1024 * 1024));
    DWORD read = 0;
    if (!ReadFile(file.Get(), bytes.data() + offset, chunk, &read, nullptr) || read != chunk) {
      return false;
    }
    offset += read;
  }
  return true;
}

bool WriteFileBytesAtomic(const std::wstring& path, const std::vector<std::byte>& bytes,
                          std::wstring& error) {
  const std::wstring temporary = path + L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
  UniqueHandle file(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!file) {
    error = L"无法创建临时 payload：" + FormatWindowsError(GetLastError()) + L" 喵。";
    return false;
  }

  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const DWORD chunk = static_cast<DWORD>(
        std::min<std::size_t>(bytes.size() - offset, 4 * 1024 * 1024));
    DWORD written = 0;
    if (!WriteFile(file.Get(), bytes.data() + offset, chunk, &written, nullptr) ||
        written != chunk) {
      const DWORD code = GetLastError();
      file.Reset();
      DeleteFileW(temporary.c_str());
      error = L"写入临时 payload 失败：" + FormatWindowsError(code) + L" 喵。";
      return false;
    }
    offset += written;
  }
  if (!FlushFileBuffers(file.Get())) {
    const DWORD code = GetLastError();
    file.Reset();
    DeleteFileW(temporary.c_str());
    error = L"刷新临时 payload 失败：" + FormatWindowsError(code) + L" 喵。";
    return false;
  }
  file.Reset();

  if (!MoveFileExW(temporary.c_str(), path.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    const DWORD code = GetLastError();
    DeleteFileW(temporary.c_str());
    error = L"提交 payload 文件失败：" + FormatWindowsError(code) + L" 喵。";
    return false;
  }
  return true;
}

bool ExtractPayload(const EmbeddedPayload& payload, std::wstring& path, std::wstring& error) {
  PWSTR local_app_data = nullptr;
  const HRESULT result = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr,
                                              &local_app_data);
  if (FAILED(result) || !local_app_data) {
    error = L"无法定位 LocalAppData 目录喵。";
    return false;
  }
  const std::wstring root(local_app_data);
  CoTaskMemFree(local_app_data);

  const std::wstring lab = root + L"\\SkyQoELab";
  const std::wstring payloads = lab + L"\\payloads";
  std::wostringstream folder_name;
  folder_name << SKYQOE_PAYLOAD_VERSION << L"-" << std::uppercase << std::hex
              << std::setfill(L'0') << std::setw(16) << payload.hash;
  const std::wstring version_folder = payloads + L"\\" + folder_name.str();
  if (!EnsureDirectory(lab, error) || !EnsureDirectory(payloads, error) ||
      !EnsureDirectory(version_folder, error)) {
    return false;
  }
  path = version_folder + L"\\" + kPayloadModuleName;

  std::vector<std::byte> existing;
  if (ReadFileBytes(path, existing) && existing == payload.bytes) {
    return true;
  }
  return WriteFileBytesAtomic(path, payload.bytes, error);
}

bool SnapshotModules(DWORD pid, std::vector<ModuleInfo>& modules, std::wstring& error) {
  HANDLE raw_snapshot = INVALID_HANDLE_VALUE;
  for (int attempt = 0; attempt < 8; ++attempt) {
    raw_snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (raw_snapshot != INVALID_HANDLE_VALUE || GetLastError() != ERROR_BAD_LENGTH) {
      break;
    }
    Sleep(20);
  }
  UniqueHandle snapshot(raw_snapshot);
  if (!snapshot) {
    error = L"无法枚举 Sky 模块：" + FormatWindowsError(GetLastError()) + L" 喵。";
    return false;
  }

  MODULEENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  if (!Module32FirstW(snapshot.Get(), &entry)) {
    error = L"读取 Sky 模块列表失败：" + FormatWindowsError(GetLastError()) + L" 喵。";
    return false;
  }
  modules.clear();
  do {
    modules.push_back(ModuleInfo{entry.szModule, entry.szExePath,
                                 reinterpret_cast<std::uintptr_t>(entry.modBaseAddr),
                                 entry.modBaseSize});
  } while (Module32NextW(snapshot.Get(), &entry));
  return true;
}

const ModuleInfo* FindModule(const std::vector<ModuleInfo>& modules, std::wstring_view name) {
  const auto found = std::find_if(modules.begin(), modules.end(), [&](const ModuleInfo& module) {
    return EqualsIgnoreCase(module.name, name);
  });
  return found == modules.end() ? nullptr : &*found;
}

bool FindTargetPid(DWORD& pid, std::wstring& error) {
  pid = 0;
  UniqueHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
  if (!snapshot) {
    error = L"无法枚举进程：" + FormatWindowsError(GetLastError()) + L" 喵。";
    return false;
  }

  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  if (!Process32FirstW(snapshot.Get(), &entry)) {
    error = L"读取进程列表失败：" + FormatWindowsError(GetLastError()) + L" 喵。";
    return false;
  }
  do {
    if (EqualsIgnoreCase(entry.szExeFile, kTargetProcessName)) {
      pid = entry.th32ProcessID;
      break;
    }
  } while (Process32NextW(snapshot.Get(), &entry));
  if (!pid) {
    error = L"没有找到正在运行的 Sky.exe，请先启动游戏并进入主界面喵。";
    return false;
  }
  return true;
}

bool FindTargetProcess(TargetProcess& target, std::wstring& error) {
  if (!FindTargetPid(target.pid, error)) {
    return false;
  }

  constexpr DWORD access = PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                           PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE |
                           SYNCHRONIZE;
  target.handle.Reset(OpenProcess(access, FALSE, target.pid));
  if (!target.handle) {
    error = L"无法打开 Sky.exe（PID " + std::to_wstring(target.pid) + L"）：" +
            FormatWindowsError(GetLastError()) +
            L"，如果游戏以管理员身份运行，请以相同权限启动加载器喵。";
    return false;
  }

  std::vector<ModuleInfo> modules;
  if (!SnapshotModules(target.pid, modules, error)) {
    return false;
  }
  const ModuleInfo* executable = FindModule(modules, kTargetProcessName);
  if (!executable) {
    error = L"Sky.exe 已运行，但模块列表中没有主程序映像喵。";
    return false;
  }
  target.executable = *executable;
  return true;
}

template <typename T>
bool ReadRemote(HANDLE process, std::uintptr_t address, T& value) {
  SIZE_T read = 0;
  return ReadProcessMemory(process, reinterpret_cast<const void*>(address), &value,
                           sizeof(value), &read) &&
         read == sizeof(value);
}

ProcessProbe ProbeTargetProcess() {
  ProcessProbe probe;
  std::wstring error;
  if (!FindTargetPid(probe.pid, error)) {
    probe.detail = std::move(error);
    return probe;
  }
  probe.running = true;

  std::vector<ModuleInfo> modules;
  if (!SnapshotModules(probe.pid, modules, error)) {
    probe.detail = std::move(error);
    return probe;
  }
  const ModuleInfo* executable = FindModule(modules, kTargetProcessName);
  if (!executable) {
    probe.detail = L"Sky.exe 正在运行，但尚未发现主程序模块喵。";
    return probe;
  }
  probe.module_base = executable->base;
  if (const ModuleInfo* menu = FindModule(modules, kPayloadModuleName)) {
    probe.menu_loaded = true;
    probe.menu_base = menu->base;
  }

  UniqueHandle process(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | SYNCHRONIZE,
                                   FALSE, probe.pid));
  if (!process) {
    probe.detail = L"已检测到 Sky.exe，但无法读取进程信息：" +
                   FormatWindowsError(GetLastError()) + L" 喵。";
    return probe;
  }

  IMAGE_DOS_HEADER dos{};
  IMAGE_NT_HEADERS64 nt{};
  if (!ReadRemote(process.Get(), probe.module_base, dos) ||
      dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 ||
      !ReadRemote(process.Get(),
                  probe.module_base + static_cast<std::uintptr_t>(dos.e_lfanew), nt) ||
      nt.Signature != IMAGE_NT_SIGNATURE ||
      nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
      nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
    probe.detail = L"已检测到 Sky.exe，但无法读取有效的 Win64 PE 头喵。";
    return probe;
  }

  probe.readable = true;
  probe.timestamp = nt.FileHeader.TimeDateStamp;
  probe.image_size = nt.OptionalHeader.SizeOfImage;
  probe.entry_point = probe.module_base + nt.OptionalHeader.AddressOfEntryPoint;
  probe.build_supported = probe.timestamp == skyqoe::kSupportedTimestamp &&
                          probe.image_size == skyqoe::kSupportedImageSize;
  probe.detail = probe.build_supported
                     ? L"Sky.exe 已就绪，当前构建与内嵌菜单匹配喵。"
                     : L"Sky.exe 已运行，但当前构建与内嵌菜单不匹配喵。";
  return probe;
}

bool ValidateTargetBuild(const TargetProcess& target, std::wstring& error) {
  IMAGE_DOS_HEADER dos{};
  if (!ReadRemote(target.handle.Get(), target.executable.base, dos) ||
      dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0) {
    error = L"无法读取 Sky.exe 的 PE 头喵。";
    return false;
  }
  IMAGE_NT_HEADERS64 nt{};
  if (!ReadRemote(target.handle.Get(),
                  target.executable.base + static_cast<std::uintptr_t>(dos.e_lfanew), nt) ||
      nt.Signature != IMAGE_NT_SIGNATURE ||
      nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
      nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
    error = L"目标 Sky.exe 不是受支持的 Win64 PE 进程喵。";
    return false;
  }
  if (nt.FileHeader.TimeDateStamp != skyqoe::kSupportedTimestamp ||
      nt.OptionalHeader.SizeOfImage != skyqoe::kSupportedImageSize) {
    error = L"当前 Sky 构建与内嵌模组不匹配，已拒绝注入喵。\n\n检测到：timestamp=" +
            Hex(nt.FileHeader.TimeDateStamp, 8) + L"，SizeOfImage=" +
            Hex(nt.OptionalHeader.SizeOfImage) + L"\n支持：timestamp=" +
            Hex(skyqoe::kSupportedTimestamp, 8) + L"，SizeOfImage=" +
            Hex(skyqoe::kSupportedImageSize) + L" 喵。";
    return false;
  }
  return true;
}

bool ReadRemoteBytes(HANDLE process, std::uintptr_t address, void* output,
                     std::size_t size) {
  SIZE_T read = 0;
  return ReadProcessMemory(process, reinterpret_cast<const void*>(address), output, size,
                           &read) &&
         read == size;
}

bool ReadRemoteAscii(HANDLE process, std::uintptr_t address, std::string& value) {
  value.clear();
  std::array<char, 256> buffer{};
  for (std::size_t offset = 0; offset < 2048; offset += buffer.size()) {
    if (!ReadRemoteBytes(process, address + offset, buffer.data(), buffer.size())) {
      return false;
    }
    for (char character : buffer) {
      if (character == '\0') {
        return true;
      }
      value.push_back(character);
    }
  }
  return false;
}

bool FindRemoteExport(HANDLE process, const ModuleInfo& module, std::string_view wanted,
                      std::uintptr_t& address, std::wstring& error) {
  IMAGE_DOS_HEADER dos{};
  IMAGE_NT_HEADERS64 nt{};
  if (!ReadRemote(process, module.base, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
      dos.e_lfanew < 0 ||
      !ReadRemote(process, module.base + static_cast<std::uintptr_t>(dos.e_lfanew), nt) ||
      nt.Signature != IMAGE_NT_SIGNATURE) {
    error = L"无法读取已加载菜单的 PE 导出表喵。";
    return false;
  }
  const IMAGE_DATA_DIRECTORY& directory =
      nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
  IMAGE_EXPORT_DIRECTORY exports{};
  if (!directory.VirtualAddress ||
      !ReadRemote(process, module.base + directory.VirtualAddress, exports) ||
      exports.NumberOfNames > 65536 || exports.NumberOfFunctions > 65536) {
    error = L"已加载菜单没有有效的导出目录喵。";
    return false;
  }

  std::vector<std::uint32_t> names(exports.NumberOfNames);
  std::vector<std::uint16_t> ordinals(exports.NumberOfNames);
  std::vector<std::uint32_t> functions(exports.NumberOfFunctions);
  if (!ReadRemoteBytes(process, module.base + exports.AddressOfNames, names.data(),
                       names.size() * sizeof(names[0])) ||
      !ReadRemoteBytes(process, module.base + exports.AddressOfNameOrdinals, ordinals.data(),
                       ordinals.size() * sizeof(ordinals[0])) ||
      !ReadRemoteBytes(process, module.base + exports.AddressOfFunctions, functions.data(),
                       functions.size() * sizeof(functions[0]))) {
    error = L"读取已加载菜单的导出数组失败喵。";
    return false;
  }

  for (std::size_t index = 0; index < names.size(); ++index) {
    std::string name;
    if (!ReadRemoteAscii(process, module.base + names[index], name) || name != wanted) {
      continue;
    }
    if (ordinals[index] >= functions.size()) {
      break;
    }
    const std::uint32_t function_rva = functions[ordinals[index]];
    if (function_rva >= directory.VirtualAddress &&
        function_rva < directory.VirtualAddress + directory.Size) {
      error = L"安全卸载导出意外指向转发器喵。";
      return false;
    }
    address = module.base + function_rva;
    return true;
  }
  error = L"已加载菜单不支持安全卸载，请在游戏中按 End 卸载后重试喵。";
  return false;
}

bool RunRemoteThread(HANDLE process, std::uintptr_t start, void* parameter,
                     DWORD timeout_ms, bool& timed_out, std::wstring& error) {
  UniqueHandle thread(CreateRemoteThread(
      process, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(start), parameter, 0,
      nullptr));
  if (!thread) {
    error = L"创建远程加载线程失败：" + FormatWindowsError(GetLastError()) + L" 喵。";
    return false;
  }
  const DWORD wait = WaitForSingleObject(thread.Get(), timeout_ms);
  timed_out = wait == WAIT_TIMEOUT;
  if (wait != WAIT_OBJECT_0) {
    if (timed_out) {
      error = L"等待远程线程超时，为避免破坏游戏，加载器没有强制终止线程喵。";
    } else {
      error = L"等待远程线程失败：" + FormatWindowsError(GetLastError()) + L" 喵。";
    }
    return false;
  }
  DWORD exit_code = 0;
  if (!GetExitCodeThread(thread.Get(), &exit_code)) {
    error = L"无法读取远程线程结果：" + FormatWindowsError(GetLastError()) + L" 喵。";
    return false;
  }
  return true;
}

bool WaitForModuleState(DWORD pid, std::wstring_view name, bool present, DWORD timeout_ms,
                        ModuleInfo* result, std::wstring& error) {
  const ULONGLONG deadline = GetTickCount64() + timeout_ms;
  do {
    std::vector<ModuleInfo> modules;
    std::wstring snapshot_error;
    if (SnapshotModules(pid, modules, snapshot_error)) {
      const ModuleInfo* module = FindModule(modules, name);
      if ((module != nullptr) == present) {
        if (result && module) {
          *result = *module;
        }
        return true;
      }
    }
    Sleep(100);
  } while (GetTickCount64() < deadline);
  error = present ? L"远程加载线程已返回，但模块没有出现在 Sky 进程中喵。"
                  : L"旧菜单没有在超时时间内安全卸载，请回到游戏按 End 后重试喵。";
  return false;
}

bool UnloadExistingMenu(const TargetProcess& target, const ModuleInfo& module,
                        std::wstring& error) {
  std::uintptr_t shutdown = 0;
  if (!FindRemoteExport(target.handle.Get(), module, kShutdownExportAscii, shutdown, error)) {
    return false;
  }
  bool timed_out = false;
  if (!RunRemoteThread(target.handle.Get(), shutdown, nullptr, kRemoteThreadTimeoutMs,
                       timed_out, error)) {
    return false;
  }
  return WaitForModuleState(target.pid, kPayloadModuleName, false,
                            kModuleUnloadTimeoutMs, nullptr, error);
}

bool ResolveRemoteProcedure(DWORD pid, const char* procedure, std::uintptr_t& address,
                            std::wstring& error) {
  const HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
  const FARPROC local_procedure = kernel32 ? GetProcAddress(kernel32, procedure) : nullptr;
  if (!local_procedure) {
    error = L"本机无法解析 LoadLibraryW 喵。";
    return false;
  }

  HMODULE owner = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(local_procedure), &owner)) {
    error = L"无法确定 LoadLibraryW 所属系统模块喵。";
    return false;
  }
  std::array<wchar_t, 32768> owner_path{};
  const DWORD length = GetModuleFileNameW(owner, owner_path.data(), owner_path.size());
  if (!length || length >= owner_path.size()) {
    error = L"无法读取 LoadLibraryW 所属模块路径喵。";
    return false;
  }

  std::vector<ModuleInfo> modules;
  if (!SnapshotModules(pid, modules, error)) {
    return false;
  }
  const ModuleInfo* remote_owner = FindModule(modules, BaseName(owner_path.data()));
  if (!remote_owner) {
    error = L"Sky 进程中没有找到 LoadLibraryW 所属系统模块喵。";
    return false;
  }
  const std::uintptr_t offset = reinterpret_cast<std::uintptr_t>(local_procedure) -
                                reinterpret_cast<std::uintptr_t>(owner);
  if (offset >= remote_owner->size) {
    error = L"LoadLibraryW 的模块偏移异常喵。";
    return false;
  }
  address = remote_owner->base + offset;
  return true;
}

bool InjectPayload(const TargetProcess& target, const std::wstring& payload_path,
                   ModuleInfo& loaded_module, std::wstring& error) {
  std::uintptr_t load_library = 0;
  if (!ResolveRemoteProcedure(target.pid, "LoadLibraryW", load_library, error)) {
    return false;
  }
  const std::size_t bytes = (payload_path.size() + 1) * sizeof(wchar_t);
  void* remote_path = VirtualAllocEx(target.handle.Get(), nullptr, bytes,
                                     MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  if (!remote_path) {
    error = L"无法在 Sky 进程中分配 DLL 路径：" + FormatWindowsError(GetLastError()) +
            L" 喵。";
    return false;
  }

  SIZE_T written = 0;
  if (!WriteProcessMemory(target.handle.Get(), remote_path, payload_path.c_str(), bytes,
                          &written) ||
      written != bytes) {
    const DWORD code = GetLastError();
    VirtualFreeEx(target.handle.Get(), remote_path, 0, MEM_RELEASE);
    error = L"写入远程 DLL 路径失败：" + FormatWindowsError(code) + L" 喵。";
    return false;
  }

  bool timed_out = false;
  const bool thread_ok = RunRemoteThread(target.handle.Get(), load_library, remote_path,
                                         kRemoteThreadTimeoutMs, timed_out, error);
  if (!timed_out) {
    VirtualFreeEx(target.handle.Get(), remote_path, 0, MEM_RELEASE);
  }
  if (!thread_ok) {
    return false;
  }
  if (!WaitForModuleState(target.pid, kPayloadModuleName, true, kRemoteThreadTimeoutMs,
                          &loaded_module, error)) {
    return false;
  }
  if (!EqualsIgnoreCase(loaded_module.path, payload_path)) {
    error = L"Sky 中出现了同名菜单模块，但路径不是本加载器的内嵌版本，已停止喵。";
    return false;
  }
  return true;
}

RunResult CheckPayloadOnly() {
  EmbeddedPayload payload;
  std::wstring error;
  if (!LoadEmbeddedPayload(payload, error)) {
    return {2, false, error};
  }
  std::wostringstream message;
  message << L"内嵌 SkyQoEMenu.dll 校验通过喵。\n\n版本：" << SKYQOE_PAYLOAD_VERSION
          << L"\n文件大小：" << payload.bytes.size() << L" 字节"
          << L"\nFNV-1a：" << Hex(payload.hash, 16)
          << L"\nPE timestamp：" << Hex(payload.timestamp, 8)
          << L"\nPE SizeOfImage：" << Hex(payload.image_size);
  return {0, true, message.str()};
}

RunResult LoadMenu(LoadMode mode) {
  EmbeddedPayload payload;
  std::wstring error;
  if (!LoadEmbeddedPayload(payload, error)) {
    return {2, false, error};
  }

  std::wstring payload_path;
  if (!ExtractPayload(payload, payload_path, error)) {
    return {3, false, error};
  }

  TargetProcess target;
  if (!FindTargetProcess(target, error)) {
    return {4, false, error};
  }
  if (!ValidateTargetBuild(target, error)) {
    return {5, false, error};
  }

  bool reloaded = false;
  std::vector<ModuleInfo> modules;
  if (!SnapshotModules(target.pid, modules, error)) {
    return {6, false, error};
  }
  const ModuleInfo* existing = FindModule(modules, kPayloadModuleName);
  if (mode == LoadMode::kInjectOnly && existing) {
    return {7, false,
            L"Sky QoE 菜单已经注入，请使用“重载菜单”按钮更新内嵌版本喵。"};
  }
  if (mode == LoadMode::kReloadOnly && !existing) {
    return {7, false,
            L"当前 Sky 进程尚未加载 Sky QoE 菜单，请先使用“注入”按钮喵。"};
  }
  if (existing) {
    const ModuleInfo existing_copy = *existing;
    if (!UnloadExistingMenu(target, existing_copy, error)) {
      return {7, false, error};
    }
    reloaded = true;
  }

  ModuleInfo loaded;
  if (!InjectPayload(target, payload_path, loaded, error)) {
    return {8, false, error};
  }

  std::wostringstream message;
  message << (reloaded ? L"旧菜单已安全卸载，新的 Sky QoE 菜单已重新注入喵。"
                       : L"Sky QoE 菜单已注入喵。")
          << L"\n\nPID：" << target.pid << L"\n模组版本：" << SKYQOE_PAYLOAD_VERSION
          << L"\n模块基址：" << Hex(loaded.base)
          << L"\n缓存路径：" << payload_path
          << L"\n\nInsert 显示或隐藏菜单，End 安全卸载菜单喵。";
  return {0, true, message.str()};
}

RunResult RunSerializedLoad(LoadMode mode) {
  UniqueHandle mutex(CreateMutexW(nullptr, FALSE, kActionMutexName));
  if (!mutex) {
    return {9, false,
            L"无法创建加载器操作互斥体：" + FormatWindowsError(GetLastError()) + L" 喵。"};
  }
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    return {9, false, L"另一个注入或重载操作正在进行，请稍后再试喵。"};
  }
  return LoadMenu(mode);
}

RunResult InspectTarget() {
  const ProcessProbe probe = ProbeTargetProcess();
  if (!probe.running || !probe.readable) {
    return {4, false, probe.detail};
  }
  std::wostringstream message;
  message << probe.detail << L"\n\nPID：" << probe.pid
          << L"\n模块基址：" << Hex(probe.module_base)
          << L"\n入口地址：" << Hex(probe.entry_point)
          << L"\n菜单状态："
          << (probe.menu_loaded ? L"已注入 @ " + Hex(probe.menu_base) : L"未注入")
          << L"\nPE timestamp：" << Hex(probe.timestamp, 8)
          << L"\nPE SizeOfImage：" << Hex(probe.image_size);
  return {probe.build_supported ? 0 : 5, probe.build_supported, message.str()};
}

void WriteOutput(const std::wstring& message) {
  const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
  if (!output || output == INVALID_HANDLE_VALUE) {
    return;
  }
  const std::wstring line = message + L"\r\n";
  DWORD written = 0;
  if (GetFileType(output) == FILE_TYPE_CHAR) {
    WriteConsoleW(output, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    return;
  }
  const int required = WideCharToMultiByte(CP_UTF8, 0, line.data(),
                                           static_cast<int>(line.size()), nullptr, 0,
                                           nullptr, nullptr);
  if (required <= 0) {
    return;
  }
  std::string utf8(static_cast<std::size_t>(required), '\0');
  WideCharToMultiByte(CP_UTF8, 0, line.data(), static_cast<int>(line.size()), utf8.data(),
                      required, nullptr, nullptr);
  WriteFile(output, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
}

int ScaleForDpi(int value, UINT dpi) {
  return MulDiv(value, static_cast<int>(dpi), 96);
}

HFONT CreateUiFont(UINT dpi, int point_size, int weight) {
  return CreateFontW(-MulDiv(point_size, static_cast<int>(dpi), 72), 0, 0, 0, weight,
                     FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                     CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                     L"Microsoft YaHei UI");
}

void ApplyFont(HWND control, HFONT font) {
  if (control && font) {
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
  }
}

void SetControlText(HWND control, const std::wstring& text) {
  if (control) {
    SetWindowTextW(control, text.c_str());
  }
}

HWND CreateLoaderControl(LoaderWindowState& state, DWORD extended_style,
                         const wchar_t* class_name, const wchar_t* text, DWORD style,
                         int x, int y, int width, int height, int id = 0,
                         HFONT font = nullptr) {
  HWND control = CreateWindowExW(
      extended_style, class_name, text, WS_CHILD | WS_VISIBLE | style,
      ScaleForDpi(x, state.dpi), ScaleForDpi(y, state.dpi),
      ScaleForDpi(width, state.dpi), ScaleForDpi(height, state.dpi), state.window,
      id ? reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)) : nullptr,
      GetModuleHandleW(nullptr), nullptr);
  ApplyFont(control, font ? font : state.normal_font);
  return control;
}

HWND CreateValueField(LoaderWindowState& state, int x, int y, int width) {
  HWND field = CreateLoaderControl(
      state, WS_EX_CLIENTEDGE, L"EDIT", L"—",
      ES_READONLY | ES_AUTOHSCROLL | WS_TABSTOP, x, y, width, 25);
  if (field) {
    SendMessageW(field, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                 MAKELPARAM(7, 7));
  }
  return field;
}

HWND CreateSummaryField(LoaderWindowState& state, int x, int y, int width, int height) {
  HWND field = CreateLoaderControl(
      state, WS_EX_CLIENTEDGE, L"EDIT", L"等待状态数据…",
      ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
      x, y, width, height, 0, state.small_font);
  if (field) {
    SendMessageW(field, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                 MAKELPARAM(8, 8));
  }
  return field;
}

bool CreateLoaderControls(LoaderWindowState& state) {
  state.heading_font = CreateUiFont(state.dpi, 19, FW_SEMIBOLD);
  state.normal_font = CreateUiFont(state.dpi, 10, FW_NORMAL);
  state.small_font = CreateUiFont(state.dpi, 9, FW_NORMAL);

  CreateLoaderControl(state, 0, L"STATIC", L"Sky QoE 状态与模组加载器",
                      SS_LEFT, 24, 18, 520, 34, 0, state.heading_font);
  const std::wstring subtitle =
      std::wstring(L"单文件静态加载器  ·  内嵌菜单 v") + SKYQOE_PAYLOAD_VERSION;
  CreateLoaderControl(state, 0, L"STATIC", subtitle.c_str(),
                      SS_LEFT, 25, 54, 470, 22, 0, state.small_font);
  CreateLoaderControl(state, 0, L"STATIC", L"每秒刷新进程状态与游戏内 HTTP 数据",
                      SS_RIGHT, 610, 55, 285, 20, 0, state.small_font);

  CreateLoaderControl(state, 0, L"BUTTON", L"进程与模组",
                      BS_GROUPBOX, 20, 82, 430, 210);
  CreateLoaderControl(state, 0, L"STATIC", L"进程状态", SS_LEFT, 38, 110, 82, 23);
  state.process_status = CreateLoaderControl(state, 0, L"STATIC", L"正在检测…",
                                             SS_LEFT, 132, 110, 300, 23);

  CreateLoaderControl(state, 0, L"STATIC", L"进程 PID", SS_LEFT, 38, 140, 82, 25);
  state.pid_value = CreateValueField(state, 132, 137, 300);
  CreateLoaderControl(state, 0, L"STATIC", L"模块基址", SS_LEFT, 38, 170, 82, 25);
  state.module_base_value = CreateValueField(state, 132, 167, 300);
  CreateLoaderControl(state, 0, L"STATIC", L"入口地址", SS_LEFT, 38, 200, 82, 25);
  state.entry_point_value = CreateValueField(state, 132, 197, 300);
  CreateLoaderControl(state, 0, L"STATIC", L"菜单模块", SS_LEFT, 38, 230, 82, 25);
  state.menu_status_value = CreateValueField(state, 132, 227, 300);
  CreateLoaderControl(state, 0, L"STATIC", L"构建校验", SS_LEFT, 38, 260, 82, 25);
  state.build_value = CreateValueField(state, 132, 257, 300);

  CreateLoaderControl(state, 0, L"BUTTON", L"游戏实时状态 · HTTP API",
                      BS_GROUPBOX, 470, 82, 430, 210);
  state.api_status = CreateLoaderControl(state, 0, L"STATIC", L"API 正在连接…",
                                         SS_LEFT, 488, 110, 238, 22);
  state.telemetry_updated = CreateLoaderControl(state, 0, L"STATIC", L"—",
                                                SS_RIGHT, 730, 110, 152, 22, 0,
                                                state.small_font);
  state.player_status = CreateLoaderControl(state, 0, L"STATIC", L"人物：等待数据",
                                            SS_LEFT, 488, 137, 394, 20, 0,
                                            state.small_font);
  state.position_status = CreateLoaderControl(state, 0, L"STATIC", L"位置：—",
                                              SS_LEFT, 488, 162, 394, 20, 0,
                                              state.small_font);
  state.world_status = CreateLoaderControl(state, 0, L"STATIC", L"地图：—",
                                           SS_LEFT, 488, 187, 394, 20, 0,
                                           state.small_font);
  state.environment_status = CreateLoaderControl(state, 0, L"STATIC", L"环境数据：—",
                                                 SS_LEFT, 488, 212, 394, 20, 0,
                                                 state.small_font);
  state.automation_status = CreateLoaderControl(state, 0, L"STATIC", L"自动功能：—",
                                                SS_LEFT, 488, 237, 394, 20, 0,
                                                state.small_font);
  CreateLoaderControl(state, 0, L"STATIC",
                      L"API 仅监听 127.0.0.1:27891，断开时面板会自动等待重连。",
                      SS_LEFT, 488, 264, 394, 18, 0, state.small_font);

  state.inject_button = CreateLoaderControl(
      state, 0, L"BUTTON", L"注入", BS_PUSHBUTTON | WS_TABSTOP,
      20, 306, 430, 42, kInjectButtonId);
  state.reload_button = CreateLoaderControl(
      state, 0, L"BUTTON", L"重载菜单", BS_PUSHBUTTON | WS_TABSTOP,
      470, 306, 430, 42, kReloadButtonId);

  CreateLoaderControl(state, 0, L"BUTTON", L"当前玩家穿搭 · internal_name / ID",
                      BS_GROUPBOX, 20, 360, 880, 116);
  state.outfit_summary = CreateSummaryField(state, 34, 384, 852, 78);

  CreateLoaderControl(state, 0, L"BUTTON", L"房间成员与位置",
                      BS_GROUPBOX, 20, 490, 430, 128);
  state.room_summary = CreateSummaryField(state, 34, 514, 402, 90);
  CreateLoaderControl(state, 0, L"BUTTON", L"聊天与接口计数",
                      BS_GROUPBOX, 470, 490, 430, 128);
  state.chat_summary = CreateSummaryField(state, 484, 514, 402, 90);

  CreateLoaderControl(state, 0, L"BUTTON", L"操作结果",
                      BS_GROUPBOX, 20, 632, 880, 94);
  state.activity = CreateLoaderControl(
      state, WS_EX_CLIENTEDGE, L"EDIT", L"正在校验内嵌菜单…",
      ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
      34, 656, 852, 56, 0, state.small_font);
  if (state.activity) {
    SendMessageW(state.activity, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                 MAKELPARAM(8, 8));
  }
  CreateLoaderControl(
      state, 0, L"STATIC",
      L"关闭此窗口不会卸载已经注入的模组菜单；Insert 切换菜单，End 安全卸载。",
      SS_LEFT, 24, 739, 872, 20, 0, state.small_font);

  return state.process_status && state.pid_value && state.module_base_value &&
         state.entry_point_value && state.menu_status_value && state.build_value &&
         state.inject_button && state.reload_button && state.activity && state.api_status &&
         state.telemetry_updated && state.player_status && state.position_status &&
         state.world_status && state.environment_status && state.automation_status &&
         state.outfit_summary && state.room_summary && state.chat_summary;
}

void ApplyTelemetry(LoaderWindowState& state, const LoaderTelemetrySnapshot& telemetry) {
  if (state.busy) {
    return;
  }
  if (!telemetry.connected && !state.probe.running) {
    SetControlText(state.api_status, L"API 等待 Sky 启动");
    state.api_color = RGB(112, 117, 128);
  } else if (!telemetry.connected && !state.probe.menu_loaded) {
    SetControlText(state.api_status, L"API 等待菜单注入");
    state.api_color = RGB(181, 103, 32);
  } else {
    SetControlText(state.api_status, telemetry.api_status);
    state.api_color = telemetry.connected ? RGB(28, 126, 91) : RGB(184, 63, 63);
  }
  SetControlText(state.telemetry_updated, telemetry.updated_at);
  SetControlText(state.player_status, telemetry.player);
  SetControlText(state.position_status, telemetry.position);
  SetControlText(state.world_status, telemetry.world);
  SetControlText(state.environment_status, telemetry.environment);
  SetControlText(state.automation_status, telemetry.automation);
  SetControlText(state.outfit_summary, telemetry.outfit);
  SetControlText(state.room_summary, telemetry.room);
  SetControlText(state.chat_summary, telemetry.chat);
  InvalidateRect(state.api_status, nullptr, TRUE);
}

void TelemetryThreadMain(LoaderWindowState* state) {
  auto next_refresh = std::chrono::steady_clock::now();
  while (!state->telemetry_stop.load(std::memory_order_acquire)) {
    try {
      auto* telemetry = new LoaderTelemetrySnapshot(FetchLoaderTelemetry());
      if (state->telemetry_stop.load(std::memory_order_acquire) ||
          !PostMessageW(state->window, kTelemetryUpdatedMessage, 0,
                        reinterpret_cast<LPARAM>(telemetry))) {
        delete telemetry;
      }
    } catch (...) {
      // A failed polling cycle must not terminate the persistent loader window.
    }
    next_refresh += std::chrono::seconds(1);
    while (!state->telemetry_stop.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < next_refresh) {
      Sleep(25);
    }
    if (std::chrono::steady_clock::now() > next_refresh + std::chrono::seconds(1)) {
      next_refresh = std::chrono::steady_clock::now();
    }
  }
}

void StopTelemetryThread(LoaderWindowState& state) {
  state.telemetry_stop.store(true, std::memory_order_release);
  if (state.telemetry_thread.joinable()) {
    state.telemetry_thread.join();
  }
  MSG pending{};
  while (PeekMessageW(&pending, state.window, kTelemetryUpdatedMessage,
                      kTelemetryUpdatedMessage, PM_REMOVE)) {
    delete reinterpret_cast<LoaderTelemetrySnapshot*>(pending.lParam);
  }
}

void RefreshLoaderWindow(LoaderWindowState& state) {
  state.probe = ProbeTargetProcess();
  const ProcessProbe& probe = state.probe;

  if (!probe.running) {
    SetControlText(state.process_status, L"未检测到 Sky.exe");
    SetControlText(state.pid_value, L"—");
    SetControlText(state.module_base_value, L"—");
    SetControlText(state.entry_point_value, L"—");
    SetControlText(state.menu_status_value, L"未注入");
    SetControlText(state.build_value, L"等待游戏进程");
    state.status_color = RGB(112, 117, 128);
  } else {
    SetControlText(state.pid_value, std::to_wstring(probe.pid));
    SetControlText(state.module_base_value,
                   probe.module_base ? Hex(probe.module_base) : L"读取中…");
    SetControlText(state.entry_point_value,
                   probe.entry_point ? Hex(probe.entry_point) : L"读取中…");
    SetControlText(state.menu_status_value,
                   probe.menu_loaded ? L"已注入 @ " + Hex(probe.menu_base) : L"未注入");
    if (!probe.readable) {
      SetControlText(state.process_status, L"已检测到，但暂时无法读取");
      SetControlText(state.build_value, L"无法校验");
      state.status_color = RGB(181, 103, 32);
    } else if (!probe.build_supported) {
      SetControlText(state.process_status, L"已检测到，但构建不受支持");
      SetControlText(state.build_value,
                     L"不匹配  ·  timestamp " + Hex(probe.timestamp, 8) +
                         L"  ·  image " + Hex(probe.image_size));
      state.status_color = RGB(184, 63, 63);
    } else {
      SetControlText(state.process_status, L"运行中，已就绪");
      SetControlText(state.build_value,
                     L"匹配  ·  timestamp " + Hex(probe.timestamp, 8) +
                         L"  ·  image " + Hex(probe.image_size));
      state.status_color = RGB(28, 126, 91);
    }
  }

  const bool can_operate = state.payload_ready && !state.busy && probe.running &&
                           probe.readable && probe.build_supported;
  EnableWindow(state.inject_button, can_operate && !probe.menu_loaded);
  EnableWindow(state.reload_button, can_operate && probe.menu_loaded);
  InvalidateRect(state.process_status, nullptr, TRUE);
}

DWORD WINAPI LoaderActionThread(void* parameter) {
  auto* request = static_cast<ActionRequest*>(parameter);
  const HWND window = request->window;
  const LoadMode mode = request->mode;
  delete request;

  auto* result = new RunResult(RunSerializedLoad(mode));
  if (!PostMessageW(window, kActionCompletedMessage, 0,
                    reinterpret_cast<LPARAM>(result))) {
    delete result;
  }
  return 0;
}

void StartLoaderAction(LoaderWindowState& state, LoadMode mode) {
  if (state.busy) {
    return;
  }
  state.busy = true;
  EnableWindow(state.inject_button, FALSE);
  EnableWindow(state.reload_button, FALSE);
  SetControlText(state.activity,
                 mode == LoadMode::kInjectOnly
                     ? L"正在校验游戏并注入内嵌菜单，请稍候…"
                     : L"正在安全卸载旧菜单并注入内嵌版本，请稍候…");
  SetControlText(state.api_status, L"菜单正在变更，等待 API 重新连接…");
  state.api_color = RGB(181, 103, 32);

  auto* request = new ActionRequest{state.window, mode};
  HANDLE thread = CreateThread(nullptr, 0, LoaderActionThread, request, 0, nullptr);
  if (!thread) {
    const std::wstring error =
        L"无法启动加载器后台线程：" + FormatWindowsError(GetLastError()) + L" 喵。";
    delete request;
    state.busy = false;
    SetControlText(state.activity, error);
    RefreshLoaderWindow(state);
    return;
  }
  CloseHandle(thread);
}

LRESULT CALLBACK LoaderWindowProcedure(HWND window, UINT message, WPARAM w_param,
                                       LPARAM l_param) {
  auto* state = reinterpret_cast<LoaderWindowState*>(
      GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    const auto* create = reinterpret_cast<const CREATESTRUCTW*>(l_param);
    state = static_cast<LoaderWindowState*>(create->lpCreateParams);
    state->window = window;
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
  }

  switch (message) {
    case WM_CREATE:
      if (!state || !CreateLoaderControls(*state)) {
        return -1;
      }
      return 0;
    case WM_TIMER:
      if (state && w_param == kProcessRefreshTimer && !state->busy) {
        RefreshLoaderWindow(*state);
      }
      return 0;
    case WM_COMMAND:
      if (!state || HIWORD(w_param) != BN_CLICKED) {
        break;
      }
      if (LOWORD(w_param) == kInjectButtonId) {
        StartLoaderAction(*state, LoadMode::kInjectOnly);
        return 0;
      }
      if (LOWORD(w_param) == kReloadButtonId) {
        StartLoaderAction(*state, LoadMode::kReloadOnly);
        return 0;
      }
      break;
    case kActionCompletedMessage:
      if (state) {
        auto* result = reinterpret_cast<RunResult*>(l_param);
        state->busy = false;
        if (result) {
          SetControlText(state->activity, result->message);
          MessageBeep(result->success ? MB_OK : MB_ICONERROR);
          delete result;
        }
        RefreshLoaderWindow(*state);
      }
      return 0;
    case kTelemetryUpdatedMessage:
      if (state) {
        auto* telemetry = reinterpret_cast<LoaderTelemetrySnapshot*>(l_param);
        if (telemetry) {
          ApplyTelemetry(*state, *telemetry);
          delete telemetry;
        }
      }
      return 0;
    case WM_CTLCOLORSTATIC:
      if (state) {
        const HDC device = reinterpret_cast<HDC>(w_param);
        const HWND control = reinterpret_cast<HWND>(l_param);
        SetBkMode(device, TRANSPARENT);
        SetBkColor(device, RGB(247, 248, 250));
        if (control == state->process_status) {
          SetTextColor(device, state->status_color);
        } else if (control == state->api_status) {
          SetTextColor(device, state->api_color);
        } else {
          SetTextColor(device, RGB(42, 47, 55));
        }
        return reinterpret_cast<LRESULT>(state->window_brush);
      }
      break;
    case WM_CLOSE:
      if (state && state->busy) {
        MessageBoxW(window, L"注入或重载正在进行，请等待操作完成后再关闭加载器喵。",
                    kLoaderWindowTitle, MB_OK | MB_ICONINFORMATION);
        return 0;
      }
      DestroyWindow(window);
      return 0;
    case WM_DESTROY:
      KillTimer(window, kProcessRefreshTimer);
      if (state) {
        StopTelemetryThread(*state);
      }
      PostQuitMessage(0);
      return 0;
    case WM_NCDESTROY:
      SetWindowLongPtrW(window, GWLP_USERDATA, 0);
      break;
    default:
      break;
  }
  return DefWindowProcW(window, message, w_param, l_param);
}

int RunLoaderWindow(HINSTANCE instance, int show_command) {
  UniqueHandle mutex(CreateMutexW(nullptr, FALSE, kGuiMutexName));
  if (!mutex) {
    MessageBoxW(nullptr, L"无法创建加载器窗口互斥体喵。", kLoaderWindowTitle,
                MB_OK | MB_ICONERROR);
    return 9;
  }
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    if (HWND existing = FindWindowW(kLoaderWindowClass, nullptr)) {
      ShowWindow(existing, SW_RESTORE);
      SetForegroundWindow(existing);
    }
    return 0;
  }

  INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
  InitCommonControlsEx(&controls);

  LoaderWindowState state;
  state.dpi = GetDpiForSystem();
  state.window_brush = CreateSolidBrush(RGB(247, 248, 250));

  WNDCLASSEXW window_class{};
  window_class.cbSize = sizeof(window_class);
  window_class.lpfnWndProc = LoaderWindowProcedure;
  window_class.hInstance = instance;
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  window_class.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
  window_class.hIconSm = LoadIconW(nullptr, IDI_APPLICATION);
  window_class.hbrBackground = state.window_brush;
  window_class.lpszClassName = kLoaderWindowClass;
  if (!RegisterClassExW(&window_class)) {
    const std::wstring error =
        L"无法注册加载器窗口：" + FormatWindowsError(GetLastError()) + L" 喵。";
    MessageBoxW(nullptr, error.c_str(), kLoaderWindowTitle, MB_OK | MB_ICONERROR);
    DeleteObject(state.window_brush);
    return 10;
  }

  RECT window_bounds{0, 0, ScaleForDpi(920, state.dpi), ScaleForDpi(770, state.dpi)};
  AdjustWindowRectEx(&window_bounds, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU |
                                         WS_MINIMIZEBOX,
                     FALSE, 0);
  const int width = window_bounds.right - window_bounds.left;
  const int height = window_bounds.bottom - window_bounds.top;
  RECT work_area{};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_area, 0);
  const int available_width = static_cast<int>(work_area.right - work_area.left) - width;
  const int available_height = static_cast<int>(work_area.bottom - work_area.top) - height;
  const int x = static_cast<int>(work_area.left) + std::max(0, available_width / 2);
  const int y = static_cast<int>(work_area.top) + std::max(0, available_height / 2);
  const std::wstring title =
      std::wstring(kLoaderWindowTitle) + L"  v" + SKYQOE_PAYLOAD_VERSION;
  HWND window = CreateWindowExW(
      0, kLoaderWindowClass, title.c_str(),
      WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
      x, y, width, height, nullptr, nullptr, instance, &state);
  if (!window) {
    const std::wstring error =
        L"无法创建加载器窗口：" + FormatWindowsError(GetLastError()) + L" 喵。";
    MessageBoxW(nullptr, error.c_str(), kLoaderWindowTitle, MB_OK | MB_ICONERROR);
    UnregisterClassW(kLoaderWindowClass, instance);
    DeleteObject(state.window_brush);
    return 11;
  }

  const RunResult payload_check = CheckPayloadOnly();
  state.payload_ready = payload_check.success;
  SetControlText(state.activity, payload_check.message);
  RefreshLoaderWindow(state);
  SetTimer(window, kProcessRefreshTimer, kProcessRefreshIntervalMs, nullptr);
  try {
    state.telemetry_thread = std::thread(TelemetryThreadMain, &state);
  } catch (...) {
    SetControlText(state.api_status, L"无法启动 HTTP 状态刷新线程");
    state.api_color = RGB(184, 63, 63);
  }
  ShowWindow(window, show_command == SW_HIDE ? SW_SHOW : show_command);
  UpdateWindow(window);

  MSG message{};
  int exit_code = 0;
  while (true) {
    const BOOL result = GetMessageW(&message, nullptr, 0, 0);
    if (result == 0) {
      exit_code = static_cast<int>(message.wParam);
      break;
    }
    if (result < 0) {
      exit_code = 12;
      break;
    }
    if (!IsDialogMessageW(window, &message)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
  }

  StopTelemetryThread(state);
  if (state.heading_font) {
    DeleteObject(state.heading_font);
  }
  if (state.normal_font) {
    DeleteObject(state.normal_font);
  }
  if (state.small_font) {
    DeleteObject(state.small_font);
  }
  UnregisterClassW(kLoaderWindowClass, instance);
  if (state.window_brush) {
    DeleteObject(state.window_brush);
  }
  return exit_code;
}

bool HasArgument(int count, wchar_t** arguments, std::wstring_view wanted) {
  for (int index = 1; index < count; ++index) {
    if (EqualsIgnoreCase(arguments[index], wanted)) {
      return true;
    }
  }
  return false;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
  int argument_count = 0;
  wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
  const bool quiet = arguments && HasArgument(argument_count, arguments, L"--quiet");
  const bool check_only = arguments && HasArgument(argument_count, arguments, L"--check");
  const bool status_only = arguments && HasArgument(argument_count, arguments, L"--status");
  const bool inject_only = arguments && HasArgument(argument_count, arguments, L"--inject");
  const bool reload_only = arguments && HasArgument(argument_count, arguments, L"--reload");
  const bool automatic = arguments && HasArgument(argument_count, arguments, L"--auto");
  const bool gui = argument_count <= 1 ||
                   (arguments && HasArgument(argument_count, arguments, L"--gui"));
  const bool help = arguments &&
                    (HasArgument(argument_count, arguments, L"--help") ||
                     HasArgument(argument_count, arguments, L"-h"));

  if (help) {
    const std::wstring message =
        L"SkyQoELoader.exe [--gui | --inject | --reload | --auto | --status | --check] "
        L"[--quiet]\n\n"
        L"无参数或 --gui：打开常驻加载器窗口喵。\n"
        L"--inject：仅在菜单尚未加载时执行首次注入喵。\n"
        L"--reload：安全卸载已加载菜单并注入内嵌版本喵。\n"
        L"--auto：自动选择首次注入或安全重载喵。\n"
        L"--status：输出 Sky 进程、入口地址和菜单状态喵。\n"
        L"--check：只校验内嵌菜单资源，不访问游戏进程喵。\n"
        L"--quiet：命令行动作不显示结果对话框；单独使用时兼容旧版 --auto 行为喵。";
    WriteOutput(message);
    if (!quiet) {
      MessageBoxW(nullptr, message.c_str(), L"Sky QoE Loader", MB_OK | MB_ICONINFORMATION);
    }
    if (arguments) {
      LocalFree(arguments);
    }
    return 0;
  }

  if (gui) {
    if (arguments) {
      LocalFree(arguments);
    }
    return RunLoaderWindow(instance, show_command);
  }

  const int selected_modes = static_cast<int>(check_only) + static_cast<int>(status_only) +
                             static_cast<int>(inject_only) + static_cast<int>(reload_only) +
                             static_cast<int>(automatic);
  if (selected_modes > 1) {
    const std::wstring message = L"一次只能指定一个加载器动作，请使用 --help 查看参数喵。";
    WriteOutput(message);
    if (!quiet) {
      MessageBoxW(nullptr, message.c_str(), kLoaderWindowTitle, MB_OK | MB_ICONERROR);
    }
    if (arguments) {
      LocalFree(arguments);
    }
    return 2;
  }

  RunResult result;
  if (check_only) {
    result = CheckPayloadOnly();
  } else if (status_only) {
    result = InspectTarget();
  } else if (inject_only) {
    result = RunSerializedLoad(LoadMode::kInjectOnly);
  } else if (reload_only) {
    result = RunSerializedLoad(LoadMode::kReloadOnly);
  } else {
    result = RunSerializedLoad(LoadMode::kInjectOrReload);
  }
  WriteOutput(result.message);
  OutputDebugStringW((result.message + L"\n").c_str());
  if (!quiet) {
    MessageBoxW(nullptr, result.message.c_str(), L"Sky QoE Loader",
                MB_OK | (result.success ? MB_ICONINFORMATION : MB_ICONERROR) |
                    MB_SETFOREGROUND);
  }
  if (arguments) {
    LocalFree(arguments);
  }
  return result.exit_code;
}
