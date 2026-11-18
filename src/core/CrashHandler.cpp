// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough

#include <OpenDesktopVR/CrashHandler.hpp>

#include <DbgHelp.h>

#include <cstdlib>
#include <cwchar>
#include <exception>

namespace OpenDesktopVR::CrashHandler {

namespace {

// Filled once by Install; read only after
wchar_t gFolder[MAX_PATH] {};
wchar_t gProgram[64] {};
volatile LONG gReporting = 0;// one report per process

// Writes the dump and the line. Nothing here allocates, locks or logs.
void WriteReport(EXCEPTION_POINTERS* exception) noexcept {
  if (InterlockedExchange(&gReporting, 1) != 0 || gFolder[0] == L'\0') {
    return;// another thread is reporting; or not installed
  }
  SYSTEMTIME now {};
  GetLocalTime(&now);
  wchar_t name[MAX_PATH] {};
  swprintf_s(name, L"%s-%04u%02u%02u-%02u%02u%02u-%lu.dmp", gProgram, now.wYear, now.wMonth, now.wDay, now.wHour,
    now.wMinute, now.wSecond, GetCurrentProcessId());
  wchar_t dumpPath[MAX_PATH * 2] {};
  swprintf_s(dumpPath, L"%s\\%s", gFolder, name);

  bool dumped = false;
  const HANDLE file = CreateFileW(dumpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file != INVALID_HANDLE_VALUE) {
    MINIDUMP_EXCEPTION_INFORMATION info {GetCurrentThreadId(), exception, FALSE};
    const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
    dumped = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, type, exception ? &info : nullptr, nullptr,
               nullptr)
      != FALSE;
    CloseHandle(file);
  }

  // One line a crash, appended
  const DWORD code = (exception && exception->ExceptionRecord) ? exception->ExceptionRecord->ExceptionCode : 0;
  const void* address = (exception && exception->ExceptionRecord) ? exception->ExceptionRecord->ExceptionAddress : nullptr;
  char line[MAX_PATH * 3] {};
  const int length = sprintf_s(line, "%04u-%02u-%02u %02u:%02u:%02u %ls (process %lu) crashed: exception 0x%08lX at %p; %s %ls\r\n",
    now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, gProgram, GetCurrentProcessId(),
    static_cast<unsigned long>(code), address, dumped ? "dump" : "no dump could be written:", name);
  wchar_t logPath[MAX_PATH * 2] {};
  swprintf_s(logPath, L"%s\\crashes.txt", gFolder);
  const HANDLE log = CreateFileW(logPath, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (log != INVALID_HANDLE_VALUE) {
    DWORD written = 0;
    WriteFile(log, line, length > 0 ? static_cast<DWORD>(length) : 0, &written, nullptr);
    CloseHandle(log);
  }
}

LONG WINAPI UnhandledFilter(EXCEPTION_POINTERS* exception) {
  WriteReport(exception);
  return EXCEPTION_EXECUTE_HANDLER;// the process ends
}

// A report from where there's no exception yet: raise one to get its context
constexpr DWORD ProgramErrorCode = 0xE0D5C001;// ours: terminate, purecall, invalid parameter
[[noreturn]] void ReportAndEnd() noexcept {
  __try {
    RaiseException(ProgramErrorCode, EXCEPTION_NONCONTINUABLE, 0, nullptr);
  } __except (WriteReport(GetExceptionInformation()), EXCEPTION_EXECUTE_HANDLER) {
  }
  TerminateProcess(GetCurrentProcess(), ProgramErrorCode);
  std::abort();
}

void OnTerminate() {
  ReportAndEnd();
}

void OnPureCall() {
  ReportAndEnd();
}

void OnInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) {
  ReportAndEnd();
}

}// namespace

void Install(const std::filesystem::path& dumpFolder, std::wstring_view programName) noexcept {
  try {
    std::error_code error;
    std::filesystem::create_directories(dumpFolder, error);
    wcsncpy_s(gFolder, dumpFolder.c_str(), _TRUNCATE);
    const std::wstring program {programName.substr(0, std::size(gProgram) - 1)};
    wcsncpy_s(gProgram, program.c_str(), _TRUNCATE);
  } catch (...) {
    return;
  }
  SetUnhandledExceptionFilter(&UnhandledFilter);
  std::set_terminate(&OnTerminate);
  _set_purecall_handler(&OnPureCall);
  _set_invalid_parameter_handler(&OnInvalidParameter);
  // The dump writer's DLL is loaded now, not while crashing
  LoadLibraryW(L"dbghelp.dll");
}

LONG Report(EXCEPTION_POINTERS* exception) noexcept {
  WriteReport(exception);
  return EXCEPTION_EXECUTE_HANDLER;
}

namespace {
LONG GuardFilter(EXCEPTION_POINTERS* exception) noexcept {
  if (gFolder[0] == L'\0') {
    return EXCEPTION_CONTINUE_SEARCH;// not installed: as if not here
  }
  return Report(exception);
}
}// namespace

LRESULT Guarded(WindowProcedure procedure, HWND window, UINT message, WPARAM wParam, LPARAM lParam) noexcept {
  __try {
    return procedure(window, message, wParam, lParam);
  } __except (GuardFilter(GetExceptionInformation())) {
    TerminateProcess(GetCurrentProcess(), 1);
  }
  return 0;
}

}// namespace OpenDesktopVR::CrashHandler
