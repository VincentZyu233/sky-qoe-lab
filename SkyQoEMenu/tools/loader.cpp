#include "game_state.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::uint16_t kPayloadResourceId = 101;
constexpr wchar_t kTargetProcessName[] = L"Sky.exe";
constexpr wchar_t kPayloadModuleName[] = L"SkyQoEMenu.dll";
constexpr char kShutdownExportAscii[] = "SkyQoE_RequestShutdown";
constexpr char kVersionExportAscii[] = "SkyQoE_GetVersion";
constexpr DWORD kRemoteThreadTimeoutMs = 15000;
constexpr DWORD kModuleUnloadTimeoutMs = 15000;

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

bool FindTargetProcess(TargetProcess& target, std::wstring& error) {
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
      target.pid = entry.th32ProcessID;
      break;
    }
  } while (Process32NextW(snapshot.Get(), &entry));
  if (!target.pid) {
    error = L"没有找到正在运行的 Sky.exe，请先启动游戏并进入主界面喵。";
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

RunResult InjectOrReload() {
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
  if (const ModuleInfo* existing = FindModule(modules, kPayloadModuleName)) {
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

bool HasArgument(int count, wchar_t** arguments, std::wstring_view wanted) {
  for (int index = 1; index < count; ++index) {
    if (EqualsIgnoreCase(arguments[index], wanted)) {
      return true;
    }
  }
  return false;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  int argument_count = 0;
  wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
  const bool quiet = arguments && HasArgument(argument_count, arguments, L"--quiet");
  const bool check_only = arguments && HasArgument(argument_count, arguments, L"--check");
  const bool help = arguments &&
                    (HasArgument(argument_count, arguments, L"--help") ||
                     HasArgument(argument_count, arguments, L"-h"));

  if (help) {
    const std::wstring message =
        L"SkyQoELoader.exe [--check] [--quiet]\n\n"
        L"无参数：校验游戏构建并注入或安全重载内嵌菜单喵。\n"
        L"--check：只校验内嵌菜单资源，不访问游戏进程喵。\n"
        L"--quiet：不显示结果对话框喵。";
    WriteOutput(message);
    if (!quiet) {
      MessageBoxW(nullptr, message.c_str(), L"Sky QoE Loader", MB_OK | MB_ICONINFORMATION);
    }
    if (arguments) {
      LocalFree(arguments);
    }
    return 0;
  }

  UniqueHandle mutex(CreateMutexW(nullptr, FALSE, L"Local\\SkyQoELoader.SingleInstance"));
  if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
    const std::wstring message = L"另一个 Sky QoE Loader 正在运行，请等待它完成喵。";
    WriteOutput(message);
    if (!quiet) {
      MessageBoxW(nullptr, message.c_str(), L"Sky QoE Loader",
                  MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
    }
    if (arguments) {
      LocalFree(arguments);
    }
    return 9;
  }

  const RunResult result = check_only ? CheckPayloadOnly() : InjectOrReload();
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
