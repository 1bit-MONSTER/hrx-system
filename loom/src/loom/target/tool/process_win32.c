// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/tool/process_platform.h"

#if defined(IREE_PLATFORM_WINDOWS)

#include <limits.h>
#include <string.h>

#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

typedef struct loom_tool_capture_file_t {
  // Capturing file handle passed to the child process.
  HANDLE handle;
} loom_tool_capture_file_t;

struct loom_tool_process_session_t {
  // Spawned child process awaiting collection.
  HANDLE process_handle;
  // Parent handle writing the child stdin pipe.
  HANDLE input_handle;
  // Parent handle reading the child stdout pipe.
  HANDLE output_handle;
  // File capturing child stderr without risking a pipe deadlock.
  loom_tool_capture_file_t stderr_file;
  // Allocator owning this session and captured terminal output.
  iree_allocator_t allocator;
  // Whether |process_handle| has been waited and released.
  bool waited;
};

static iree_status_t loom_tool_win32_status(DWORD error, const char* message) {
  return iree_make_status(iree_status_code_from_win32_error(error),
                          "%s (GetLastError=%lu)", message,
                          (unsigned long)error);
}

static iree_status_t loom_tool_win32_utf8_to_wide(iree_string_view_t value,
                                                  iree_allocator_t allocator,
                                                  wchar_t** out_value) {
  *out_value = NULL;
  if (value.size > INT_MAX) {
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                            "UTF-8 value exceeds Win32 character limits");
  }
  int wide_length = 0;
  if (!iree_string_view_is_empty(value)) {
    wide_length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data,
                                      (int)value.size, NULL, 0);
    if (wide_length == 0) {
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "value is not valid UTF-8 (Win32 error %lu)",
                              (unsigned long)GetLastError());
    }
  }

  iree_host_size_t allocation_size = 0;
  if (!iree_host_size_checked_mul((iree_host_size_t)wide_length + 1,
                                  sizeof(wchar_t), &allocation_size)) {
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                            "UTF-16 allocation size overflow");
  }
  wchar_t* wide_value = NULL;
  IREE_RETURN_IF_ERROR(
      iree_allocator_malloc(allocator, allocation_size, (void**)&wide_value));
  if (wide_length != 0 &&
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data,
                          (int)value.size, wide_value, wide_length) == 0) {
    DWORD error = GetLastError();
    iree_allocator_free(allocator, wide_value);
    return loom_tool_win32_status(error, "failed to convert UTF-8 to UTF-16");
  }
  wide_value[wide_length] = L'\0';
  *out_value = wide_value;
  return iree_ok_status();
}

static iree_status_t loom_tool_win32_wide_to_utf8(const wchar_t* value,
                                                  iree_host_size_t capacity,
                                                  char* out_value) {
  if (capacity > INT_MAX) {
    capacity = INT_MAX;
  }
  int utf8_length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value,
                                        -1, NULL, 0, NULL, NULL);
  if (utf8_length == 0) {
    return loom_tool_win32_status(GetLastError(),
                                  "failed to measure UTF-16 path");
  }
  if ((iree_host_size_t)utf8_length > capacity) {
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                            "UTF-8 path exceeds storage capacity");
  }
  if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, out_value,
                          (int)capacity, NULL, NULL) == 0) {
    return loom_tool_win32_status(GetLastError(),
                                  "failed to convert UTF-16 path to UTF-8");
  }
  return iree_ok_status();
}

static iree_status_t loom_tool_win32_delete_utf8_file(const char* path) {
  wchar_t wide_path[4096];
  int wide_length =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide_path,
                          (int)IREE_ARRAYSIZE(wide_path));
  if (wide_length == 0) {
    return loom_tool_win32_status(GetLastError(),
                                  "failed to convert temporary file path");
  }
  if (DeleteFileW(wide_path)) {
    return iree_ok_status();
  }
  DWORD error = GetLastError();
  if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
    return iree_ok_status();
  }
  return loom_tool_win32_status(error, "failed to delete temporary file");
}

static iree_status_t loom_tool_capture_file_open(
    const wchar_t* prefix, loom_tool_capture_file_t* out_file) {
  memset(out_file, 0, sizeof(*out_file));
  wchar_t temp_directory[4096] = {0};
  DWORD temp_directory_length =
      GetTempPathW(IREE_ARRAYSIZE(temp_directory), temp_directory);
  if (temp_directory_length == 0 ||
      temp_directory_length >= IREE_ARRAYSIZE(temp_directory)) {
    return loom_tool_win32_status(GetLastError(),
                                  "failed to resolve temporary directory");
  }

  SECURITY_ATTRIBUTES security_attributes;
  memset(&security_attributes, 0, sizeof(security_attributes));
  security_attributes.nLength = sizeof(security_attributes);
  security_attributes.bInheritHandle = TRUE;

  static volatile LONG path_sequence = 0;
  for (int attempt = 0; attempt < 16; ++attempt) {
    wchar_t path[4096] = {0};
    memcpy(path, temp_directory,
           (iree_host_size_t)temp_directory_length * sizeof(wchar_t));
    iree_host_size_t path_length = temp_directory_length;
    static const wchar_t kStem[] = L"loom_tool_";
    if (path_length + IREE_ARRAYSIZE(kStem) >= IREE_ARRAYSIZE(path)) {
      return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                              "temporary capture path is too long");
    }
    memcpy(path + path_length, kStem, sizeof(kStem) - sizeof(kStem[0]));
    path_length += IREE_ARRAYSIZE(kStem) - 1;
    for (const wchar_t* p = prefix; *p != L'\0'; ++p) {
      if (path_length + 1 >= IREE_ARRAYSIZE(path)) {
        return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                                "temporary capture path is too long");
      }
      path[path_length++] = *p;
    }
    path[path_length++] = L'_';

    char unique_suffix[96] = {0};
    int suffix_length =
        iree_snprintf(unique_suffix, sizeof(unique_suffix), "%lu_%llu_%ld.tmp",
                      (unsigned long)GetCurrentProcessId(),
                      (unsigned long long)GetTickCount64(),
                      (long)InterlockedIncrement(&path_sequence));
    if (suffix_length < 0 ||
        (iree_host_size_t)suffix_length >= sizeof(unique_suffix) ||
        path_length + (iree_host_size_t)suffix_length >= IREE_ARRAYSIZE(path)) {
      return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                              "temporary capture path is too long");
    }
    for (int i = 0; i < suffix_length; ++i) {
      path[path_length++] = (wchar_t)unique_suffix[i];
    }
    path[path_length] = L'\0';

    HANDLE handle = CreateFileW(
        path, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        &security_attributes, CREATE_NEW,
        FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED |
            FILE_FLAG_DELETE_ON_CLOSE,
        NULL);
    if (handle != INVALID_HANDLE_VALUE) {
      out_file->handle = handle;
      return iree_ok_status();
    }
    DWORD error = GetLastError();
    if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) {
      return loom_tool_win32_status(error, "failed to open capture file");
    }
  }
  return loom_tool_win32_status(ERROR_ALREADY_EXISTS,
                                "failed to reserve a unique capture file path");
}

static void loom_tool_capture_file_deinitialize(
    loom_tool_capture_file_t* file) {
  if (file->handle != NULL && file->handle != INVALID_HANDLE_VALUE) {
    CloseHandle(file->handle);
  }
  memset(file, 0, sizeof(*file));
}

static iree_status_t loom_tool_capture_file_read(
    loom_tool_capture_file_t* file, iree_allocator_t allocator,
    loom_tool_output_t* out_output) {
  *out_output = (loom_tool_output_t){0};
  if (SetFilePointer(file->handle, 0, NULL, FILE_BEGIN) ==
      INVALID_SET_FILE_POINTER) {
    DWORD error = GetLastError();
    if (error != NO_ERROR) {
      return loom_tool_win32_status(error, "failed to rewind capture file");
    }
  }

  iree_string_builder_t builder;
  iree_string_builder_initialize(allocator, &builder);
  iree_status_t status = iree_ok_status();
  uint8_t buffer[4096];
  bool reading = true;
  while (reading && iree_status_is_ok(status)) {
    DWORD bytes_read = 0;
    if (!ReadFile(file->handle, buffer, sizeof(buffer), &bytes_read, NULL)) {
      status =
          loom_tool_win32_status(GetLastError(), "failed to read capture file");
    } else if (bytes_read == 0) {
      reading = false;
    } else {
      status = iree_string_builder_append_string(
          &builder, iree_make_string_view((const char*)buffer,
                                          (iree_host_size_t)bytes_read));
    }
  }
  if (iree_status_is_ok(status)) {
    out_output->length = iree_string_builder_size(&builder);
    out_output->data = iree_string_builder_take_storage(&builder);
  }
  if (!iree_status_is_ok(status)) {
    loom_tool_output_deinitialize(out_output, allocator);
  }
  iree_string_builder_deinitialize(&builder);
  return status;
}

static bool loom_tool_win32_arg_needs_quotes(const char* argument) {
  if (argument[0] == '\0') {
    return true;
  }
  for (const char* p = argument; *p != '\0'; ++p) {
    if (*p == ' ' || *p == '\t' || *p == '"') {
      return true;
    }
  }
  return false;
}

static iree_status_t loom_tool_win32_append_char(iree_string_builder_t* builder,
                                                 char value) {
  return iree_string_builder_append_string(builder,
                                           iree_make_string_view(&value, 1));
}

static iree_status_t loom_tool_win32_append_repeated_char(
    iree_string_builder_t* builder, char value, iree_host_size_t count) {
  iree_status_t status = iree_ok_status();
  for (iree_host_size_t i = 0; i < count && iree_status_is_ok(status); ++i) {
    status = loom_tool_win32_append_char(builder, value);
  }
  return status;
}

static iree_status_t loom_tool_win32_append_quoted_arg(
    iree_string_builder_t* builder, const char* argument) {
  if (!loom_tool_win32_arg_needs_quotes(argument)) {
    return iree_string_builder_append_cstring(builder, argument);
  }

  iree_status_t status = loom_tool_win32_append_char(builder, '"');
  iree_host_size_t backslash_count = 0;
  for (const char* p = argument; *p != '\0' && iree_status_is_ok(status); ++p) {
    if (*p == '\\') {
      ++backslash_count;
    } else if (*p == '"') {
      status = loom_tool_win32_append_repeated_char(builder, '\\',
                                                    backslash_count * 2 + 1);
      if (iree_status_is_ok(status)) {
        status = loom_tool_win32_append_char(builder, '"');
      }
      backslash_count = 0;
    } else {
      status =
          loom_tool_win32_append_repeated_char(builder, '\\', backslash_count);
      if (iree_status_is_ok(status)) {
        status = loom_tool_win32_append_char(builder, *p);
      }
      backslash_count = 0;
    }
  }
  if (iree_status_is_ok(status)) {
    status = loom_tool_win32_append_repeated_char(builder, '\\',
                                                  backslash_count * 2);
  }
  if (iree_status_is_ok(status)) {
    status = loom_tool_win32_append_char(builder, '"');
  }
  return status;
}

static iree_status_t loom_tool_win32_make_command_line(
    char** argv, iree_allocator_t allocator, wchar_t** out_command_line) {
  *out_command_line = NULL;
  iree_string_builder_t builder;
  iree_string_builder_initialize(allocator, &builder);
  iree_status_t status = iree_ok_status();
  for (iree_host_size_t i = 0; argv[i] != NULL && iree_status_is_ok(status);
       ++i) {
    if (i != 0) {
      status = loom_tool_win32_append_char(&builder, ' ');
    }
    if (iree_status_is_ok(status)) {
      status = loom_tool_win32_append_quoted_arg(&builder, argv[i]);
    }
  }
  if (iree_status_is_ok(status)) {
    status = loom_tool_win32_utf8_to_wide(iree_string_builder_view(&builder),
                                          allocator, out_command_line);
  }
  iree_string_builder_deinitialize(&builder);
  return status;
}

enum {
  LOOM_TOOL_STDIN_HANDLE_INDEX = 0,
  LOOM_TOOL_STDOUT_HANDLE_INDEX,
  LOOM_TOOL_STDERR_HANDLE_INDEX,
  LOOM_TOOL_INHERITED_HANDLE_COUNT,
};

typedef struct loom_tool_spawn_options_t {
  // Extended process startup information passed to CreateProcessW.
  STARTUPINFOEXW startup_info;
  // Handle values retained for the lifetime of the process attribute list.
  HANDLE inherited_handles[LOOM_TOOL_INHERITED_HANDLE_COUNT];
  // Storage for the explicit inherited-handle list.
  LPPROC_THREAD_ATTRIBUTE_LIST attribute_list;
  // Whether |attribute_list| was initialized.
  bool attribute_list_initialized;
} loom_tool_spawn_options_t;

static void loom_tool_spawn_options_deinitialize(
    loom_tool_spawn_options_t* options, iree_allocator_t allocator) {
  if (options->attribute_list_initialized) {
    DeleteProcThreadAttributeList(options->attribute_list);
  }
  iree_allocator_free(allocator, options->attribute_list);
  memset(options, 0, sizeof(*options));
}

static iree_status_t loom_tool_spawn_options_initialize(
    HANDLE stdin_handle, HANDLE stdout_handle, HANDLE stderr_handle,
    iree_allocator_t allocator, loom_tool_spawn_options_t* out_options) {
  memset(out_options, 0, sizeof(*out_options));
  out_options->inherited_handles[LOOM_TOOL_STDIN_HANDLE_INDEX] = stdin_handle;
  out_options->inherited_handles[LOOM_TOOL_STDOUT_HANDLE_INDEX] = stdout_handle;
  out_options->inherited_handles[LOOM_TOOL_STDERR_HANDLE_INDEX] = stderr_handle;

  SIZE_T attribute_list_size = 0;
  BOOL measure_result =
      InitializeProcThreadAttributeList(NULL, 1, 0, &attribute_list_size);
  if (measure_result) {
    return iree_make_status(
        IREE_STATUS_INTERNAL,
        "process startup attribute measurement unexpectedly succeeded");
  }
  DWORD measure_error = GetLastError();
  if (measure_error != ERROR_INSUFFICIENT_BUFFER) {
    return loom_tool_win32_status(
        measure_error, "failed to measure process startup attributes");
  }
  IREE_RETURN_IF_ERROR(iree_allocator_malloc(
      allocator, attribute_list_size, (void**)&out_options->attribute_list));
  if (!InitializeProcThreadAttributeList(out_options->attribute_list, 1, 0,
                                         &attribute_list_size)) {
    return loom_tool_win32_status(
        GetLastError(), "failed to initialize process startup attributes");
  }
  out_options->attribute_list_initialized = true;

  if (!UpdateProcThreadAttribute(
          out_options->attribute_list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
          out_options->inherited_handles,
          sizeof(out_options->inherited_handles), NULL, NULL)) {
    return loom_tool_win32_status(
        GetLastError(), "failed to restrict inherited process handles");
  }

  out_options->startup_info.StartupInfo.cb = sizeof(out_options->startup_info);
  out_options->startup_info.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  out_options->startup_info.StartupInfo.hStdInput = stdin_handle;
  out_options->startup_info.StartupInfo.hStdOutput = stdout_handle;
  out_options->startup_info.StartupInfo.hStdError = stderr_handle;
  out_options->startup_info.lpAttributeList = out_options->attribute_list;
  return iree_ok_status();
}

static iree_status_t loom_tool_win32_open_null_input(HANDLE* out_handle) {
  *out_handle = INVALID_HANDLE_VALUE;
  SECURITY_ATTRIBUTES security_attributes;
  memset(&security_attributes, 0, sizeof(security_attributes));
  security_attributes.nLength = sizeof(security_attributes);
  security_attributes.bInheritHandle = TRUE;
  HANDLE handle = CreateFileW(
      L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
      &security_attributes, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (handle == INVALID_HANDLE_VALUE) {
    return loom_tool_win32_status(GetLastError(),
                                  "failed to disconnect child stdin");
  }
  *out_handle = handle;
  return iree_ok_status();
}

iree_status_t loom_tool_process_run_platform(
    char** argv, bool search_path, iree_allocator_t allocator,
    loom_tool_process_result_t* out_result) {
  *out_result = (loom_tool_process_result_t){0};

  loom_tool_capture_file_t stdout_file;
  loom_tool_capture_file_t stderr_file;
  IREE_RETURN_IF_ERROR(loom_tool_capture_file_open(L"out", &stdout_file));
  iree_status_t status = loom_tool_capture_file_open(L"err", &stderr_file);

  wchar_t* application_name = NULL;
  wchar_t* command_line = NULL;
  HANDLE stdin_handle = INVALID_HANDLE_VALUE;
  PROCESS_INFORMATION process_info;
  memset(&process_info, 0, sizeof(process_info));
  loom_tool_spawn_options_t spawn_options;
  memset(&spawn_options, 0, sizeof(spawn_options));
  if (iree_status_is_ok(status)) {
    status = loom_tool_win32_make_command_line(argv, allocator, &command_line);
  }
  if (iree_status_is_ok(status) && !search_path) {
    status = loom_tool_win32_utf8_to_wide(iree_make_cstring_view(argv[0]),
                                          allocator, &application_name);
  }
  if (iree_status_is_ok(status)) {
    status = loom_tool_win32_open_null_input(&stdin_handle);
  }
  if (iree_status_is_ok(status)) {
    status = loom_tool_spawn_options_initialize(
        stdin_handle, stdout_file.handle, stderr_file.handle, allocator,
        &spawn_options);
  }
  if (iree_status_is_ok(status)) {
    if (!CreateProcessW(application_name, command_line, NULL, NULL, TRUE,
                        EXTENDED_STARTUPINFO_PRESENT, NULL, NULL,
                        &spawn_options.startup_info.StartupInfo,
                        &process_info)) {
      status = loom_tool_win32_status(GetLastError(),
                                      "failed to spawn tool process");
    }
  }
  loom_tool_spawn_options_deinitialize(&spawn_options, allocator);
  if (stdin_handle != INVALID_HANDLE_VALUE) {
    CloseHandle(stdin_handle);
  }
  if (iree_status_is_ok(status)) {
    DWORD wait_result = WaitForSingleObject(process_info.hProcess, INFINITE);
    if (wait_result == WAIT_FAILED) {
      status = loom_tool_win32_status(GetLastError(),
                                      "failed to wait for tool process");
    } else if (wait_result != WAIT_OBJECT_0) {
      status = iree_make_status(IREE_STATUS_INTERNAL,
                                "unexpected tool process wait result %lu",
                                (unsigned long)wait_result);
    }
  }
  if (iree_status_is_ok(status)) {
    DWORD exit_code = 1;
    if (!GetExitCodeProcess(process_info.hProcess, &exit_code)) {
      status = loom_tool_win32_status(GetLastError(),
                                      "failed to query tool process exit code");
    } else {
      out_result->exit_code = (int)exit_code;
    }
  }
  if (process_info.hThread != NULL) {
    CloseHandle(process_info.hThread);
  }
  if (process_info.hProcess != NULL) {
    CloseHandle(process_info.hProcess);
  }
  if (iree_status_is_ok(status)) {
    status = loom_tool_capture_file_read(&stdout_file, allocator,
                                         &out_result->stdout_bytes);
  }
  if (iree_status_is_ok(status)) {
    status = loom_tool_capture_file_read(&stderr_file, allocator,
                                         &out_result->stderr_bytes);
  }
  if (!iree_status_is_ok(status)) {
    loom_tool_process_result_deinitialize(out_result, allocator);
  }
  iree_allocator_free(allocator, application_name);
  iree_allocator_free(allocator, command_line);
  loom_tool_capture_file_deinitialize(&stderr_file);
  loom_tool_capture_file_deinitialize(&stdout_file);
  return status;
}

static iree_status_t loom_tool_win32_create_pipe(HANDLE* out_read_handle,
                                                 HANDLE* out_write_handle) {
  *out_read_handle = INVALID_HANDLE_VALUE;
  *out_write_handle = INVALID_HANDLE_VALUE;
  SECURITY_ATTRIBUTES security_attributes;
  memset(&security_attributes, 0, sizeof(security_attributes));
  security_attributes.nLength = sizeof(security_attributes);
  security_attributes.bInheritHandle = TRUE;
  if (!CreatePipe(out_read_handle, out_write_handle, &security_attributes, 0)) {
    return loom_tool_win32_status(GetLastError(),
                                  "failed to create process stream pipe");
  }
  return iree_ok_status();
}

static iree_status_t loom_tool_win32_make_handle_private(HANDLE handle) {
  if (!SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0)) {
    return loom_tool_win32_status(
        GetLastError(), "failed to restrict parent process stream handle");
  }
  return iree_ok_status();
}

static void loom_tool_win32_close_handle(HANDLE* handle) {
  if (*handle != NULL && *handle != INVALID_HANDLE_VALUE) {
    CloseHandle(*handle);
  }
  *handle = INVALID_HANDLE_VALUE;
}

iree_status_t loom_tool_process_session_create_platform(
    char** argv, bool search_path, iree_allocator_t allocator,
    loom_tool_process_session_t** out_session) {
  *out_session = NULL;
  loom_tool_process_session_t* session = NULL;
  IREE_RETURN_IF_ERROR(
      iree_allocator_malloc(allocator, sizeof(*session), (void**)&session));
  *session = (loom_tool_process_session_t){
      .process_handle = INVALID_HANDLE_VALUE,
      .input_handle = INVALID_HANDLE_VALUE,
      .output_handle = INVALID_HANDLE_VALUE,
      .allocator = allocator,
  };

  HANDLE child_input_handle = INVALID_HANDLE_VALUE;
  HANDLE parent_input_handle = INVALID_HANDLE_VALUE;
  HANDLE parent_output_handle = INVALID_HANDLE_VALUE;
  HANDLE child_output_handle = INVALID_HANDLE_VALUE;
  iree_status_t status =
      loom_tool_win32_create_pipe(&child_input_handle, &parent_input_handle);
  if (iree_status_is_ok(status)) {
    status = loom_tool_win32_make_handle_private(parent_input_handle);
  }
  if (iree_status_is_ok(status)) {
    status = loom_tool_win32_create_pipe(&parent_output_handle,
                                         &child_output_handle);
  }
  if (iree_status_is_ok(status)) {
    status = loom_tool_win32_make_handle_private(parent_output_handle);
  }
  if (iree_status_is_ok(status)) {
    status = loom_tool_capture_file_open(L"session_err", &session->stderr_file);
  }

  wchar_t* application_name = NULL;
  wchar_t* command_line = NULL;
  if (iree_status_is_ok(status)) {
    status = loom_tool_win32_make_command_line(argv, allocator, &command_line);
  }
  if (iree_status_is_ok(status) && !search_path) {
    status = loom_tool_win32_utf8_to_wide(iree_make_cstring_view(argv[0]),
                                          allocator, &application_name);
  }
  loom_tool_spawn_options_t spawn_options;
  memset(&spawn_options, 0, sizeof(spawn_options));
  if (iree_status_is_ok(status)) {
    status = loom_tool_spawn_options_initialize(
        child_input_handle, child_output_handle, session->stderr_file.handle,
        allocator, &spawn_options);
  }
  PROCESS_INFORMATION process_info;
  memset(&process_info, 0, sizeof(process_info));
  if (iree_status_is_ok(status) &&
      !CreateProcessW(application_name, command_line, NULL, NULL, TRUE,
                      EXTENDED_STARTUPINFO_PRESENT, NULL, NULL,
                      &spawn_options.startup_info.StartupInfo, &process_info)) {
    status = loom_tool_win32_status(GetLastError(),
                                    "failed to spawn persistent tool process");
  }
  loom_tool_spawn_options_deinitialize(&spawn_options, allocator);
  iree_allocator_free(allocator, application_name);
  iree_allocator_free(allocator, command_line);
  loom_tool_win32_close_handle(&child_input_handle);
  loom_tool_win32_close_handle(&child_output_handle);

  if (iree_status_is_ok(status)) {
    CloseHandle(process_info.hThread);
    session->process_handle = process_info.hProcess;
    session->input_handle = parent_input_handle;
    parent_input_handle = INVALID_HANDLE_VALUE;
    session->output_handle = parent_output_handle;
    parent_output_handle = INVALID_HANDLE_VALUE;
    *out_session = session;
  } else {
    loom_tool_win32_close_handle(&parent_input_handle);
    loom_tool_win32_close_handle(&parent_output_handle);
    loom_tool_capture_file_deinitialize(&session->stderr_file);
    iree_allocator_free(allocator, session);
  }
  return status;
}

iree_status_t loom_tool_process_session_write_all_platform(
    loom_tool_process_session_t* session, iree_const_byte_span_t data) {
  if (session->input_handle == INVALID_HANDLE_VALUE || session->waited) {
    return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                            "process session input is closed");
  }
  iree_host_size_t offset = 0;
  while (offset < data.data_length) {
    const iree_host_size_t remaining = data.data_length - offset;
    const DWORD write_length =
        remaining > MAXDWORD ? MAXDWORD : (DWORD)remaining;
    DWORD bytes_written = 0;
    if (!WriteFile(session->input_handle, data.data + offset, write_length,
                   &bytes_written, NULL)) {
      return loom_tool_win32_status(GetLastError(),
                                    "failed to write process session input");
    }
    if (bytes_written == 0) {
      return iree_make_status(
          IREE_STATUS_DATA_LOSS,
          "process session stdin accepted no bytes after %zu of %zu", offset,
          data.data_length);
    }
    offset += bytes_written;
  }
  return iree_ok_status();
}

iree_status_t loom_tool_process_session_read_all_platform(
    loom_tool_process_session_t* session, iree_byte_span_t data) {
  if (session->waited) {
    return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                            "process session has already exited");
  }
  iree_host_size_t offset = 0;
  while (offset < data.data_length) {
    const iree_host_size_t remaining = data.data_length - offset;
    const DWORD read_length =
        remaining > MAXDWORD ? MAXDWORD : (DWORD)remaining;
    DWORD bytes_read = 0;
    if (!ReadFile(session->output_handle, data.data + offset, read_length,
                  &bytes_read, NULL)) {
      DWORD error = GetLastError();
      if (error == ERROR_BROKEN_PIPE) {
        return iree_make_status(
            IREE_STATUS_DATA_LOSS,
            "process session stdout ended after %zu of %zu requested bytes",
            offset, data.data_length);
      }
      return loom_tool_win32_status(error,
                                    "failed to read process session output");
    }
    if (bytes_read == 0) {
      return iree_make_status(
          IREE_STATUS_DATA_LOSS,
          "process session stdout ended after %zu of %zu requested bytes",
          offset, data.data_length);
    }
    offset += bytes_read;
  }
  return iree_ok_status();
}

iree_status_t loom_tool_process_session_close_input_platform(
    loom_tool_process_session_t* session) {
  if (session->input_handle == INVALID_HANDLE_VALUE) {
    return iree_ok_status();
  }
  HANDLE input_handle = session->input_handle;
  session->input_handle = INVALID_HANDLE_VALUE;
  if (!CloseHandle(input_handle)) {
    return loom_tool_win32_status(GetLastError(),
                                  "failed to close process session input");
  }
  return iree_ok_status();
}

iree_status_t loom_tool_process_session_wait_platform(
    loom_tool_process_session_t* session,
    loom_tool_process_result_t* out_result) {
  if (session->waited) {
    return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                            "process session has already been waited");
  }
  *out_result = (loom_tool_process_result_t){0};
  iree_status_t status =
      loom_tool_process_session_close_input_platform(session);
  loom_tool_win32_close_handle(&session->output_handle);
  DWORD wait_result = WaitForSingleObject(session->process_handle, INFINITE);
  iree_status_t wait_status = iree_ok_status();
  if (wait_result == WAIT_FAILED) {
    wait_status = loom_tool_win32_status(
        GetLastError(), "failed to wait for persistent tool process");
  } else if (wait_result != WAIT_OBJECT_0) {
    wait_status = iree_make_status(IREE_STATUS_INTERNAL,
                                   "unexpected tool process wait result %lu",
                                   (unsigned long)wait_result);
  }
  if (iree_status_is_ok(wait_status)) {
    DWORD exit_code = 1;
    if (!GetExitCodeProcess(session->process_handle, &exit_code)) {
      wait_status = loom_tool_win32_status(
          GetLastError(), "failed to query persistent tool process exit code");
    } else {
      out_result->exit_code = (int)exit_code;
    }
  }
  const bool wait_succeeded = iree_status_is_ok(wait_status);
  status = iree_status_join(status, wait_status);
  session->waited = true;
  loom_tool_win32_close_handle(&session->process_handle);
  if (wait_succeeded) {
    status = iree_status_join(
        status,
        loom_tool_capture_file_read(&session->stderr_file, session->allocator,
                                    &out_result->stderr_bytes));
  }
  if (!iree_status_is_ok(status)) {
    loom_tool_process_result_deinitialize(out_result, session->allocator);
  }
  return status;
}

void loom_tool_process_session_destroy_platform(
    loom_tool_process_session_t* session) {
  IREE_ASSERT(session->waited);
  IREE_ASSERT(session->process_handle == INVALID_HANDLE_VALUE);
  loom_tool_win32_close_handle(&session->input_handle);
  loom_tool_win32_close_handle(&session->output_handle);
  loom_tool_capture_file_deinitialize(&session->stderr_file);
  iree_allocator_t allocator = session->allocator;
  memset(session, 0, sizeof(*session));
  iree_allocator_free(allocator, session);
}

iree_status_t loom_tool_temp_file_initialize_platform(
    const char* stem_buffer, loom_tool_temp_file_t* out_file) {
  memset(out_file, 0, sizeof(*out_file));

  wchar_t stem_wide[32] = {0};
  for (iree_host_size_t i = 0; stem_buffer[i] != '\0'; ++i) {
    stem_wide[i] = (wchar_t)stem_buffer[i];
  }

  wchar_t temp_directory[4096] = {0};
  DWORD temp_directory_length =
      GetTempPathW(IREE_ARRAYSIZE(temp_directory), temp_directory);
  if (temp_directory_length == 0 ||
      temp_directory_length >= IREE_ARRAYSIZE(temp_directory)) {
    return loom_tool_win32_status(GetLastError(),
                                  "failed to resolve temporary directory");
  }

  wchar_t temp_path[4096] = {0};
  if (GetTempFileNameW(temp_directory, stem_wide, 0, temp_path) == 0) {
    memset(out_file, 0, sizeof(*out_file));
    return loom_tool_win32_status(GetLastError(),
                                  "failed to allocate temporary file path");
  }
  iree_status_t status = loom_tool_win32_wide_to_utf8(
      temp_path, IREE_ARRAYSIZE(out_file->path), out_file->path);
  if (!iree_status_is_ok(status)) {
    DeleteFileW(temp_path);
    memset(out_file, 0, sizeof(*out_file));
  }
  return status;
}

iree_status_t loom_tool_temp_file_deinitialize_platform(
    loom_tool_temp_file_t* file) {
  if (file == NULL) {
    return iree_ok_status();
  }
  iree_status_t status = iree_ok_status();
  if (file->path[0] != '\0') {
    status = loom_tool_win32_delete_utf8_file(file->path);
  }
  memset(file, 0, sizeof(*file));
  return status;
}

#endif  // IREE_PLATFORM_WINDOWS
