#include <windows.h>
#include <dbghelp.h>

#include <cwchar>
#include <iostream>
#include <string>

int wmain(int argument_count, wchar_t** arguments) {
  if (argument_count != 3) {
    std::wcerr << L"Usage: SkyProcessDump <pid> <output.dmp>\n";
    return 1;
  }

  wchar_t* end = nullptr;
  const unsigned long raw_pid = std::wcstoul(arguments[1], &end, 10);
  if (!end || *end != L'\0' || raw_pid == 0) {
    std::wcerr << L"Invalid process id\n";
    return 2;
  }
  const DWORD process_id = static_cast<DWORD>(raw_pid);
  const std::wstring output_path = arguments[2];

  const HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_DUP_HANDLE,
                                     FALSE, process_id);
  if (!process) {
    std::wcerr << L"OpenProcess failed: " << GetLastError() << L'\n';
    return 3;
  }
  const HANDLE output = CreateFileW(output_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (output == INVALID_HANDLE_VALUE) {
    std::wcerr << L"CreateFile failed: " << GetLastError() << L'\n';
    CloseHandle(process);
    return 4;
  }

  const auto dump_type = static_cast<MINIDUMP_TYPE>(
      MiniDumpWithFullMemory | MiniDumpWithFullMemoryInfo | MiniDumpWithHandleData |
      MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules | MiniDumpWithProcessThreadData |
      MiniDumpWithTokenInformation);
  const BOOL success = MiniDumpWriteDump(process, process_id, output, dump_type, nullptr, nullptr,
                                         nullptr);
  const DWORD error = success ? ERROR_SUCCESS : GetLastError();
  CloseHandle(output);
  CloseHandle(process);
  if (!success) {
    std::wcerr << L"MiniDumpWriteDump failed: " << error << L'\n';
    return 5;
  }

  std::wcout << output_path << L'\n';
  return 0;
}
