#.rst:
# FindLibunwind
# -----------
#
# Find Libunwind
#
# Find Libunwind headers and library
#
# ::
#
#   UNWIND_FOUND                     - True if libunwind is found.
#   UNWIND_INCLUDE_DIRS              - Directory where libunwind headers are located.
#   UNWIND_LIBRARIES                 - Unwind libraries to link against.
#   UNWIND_HAS_UNW_GETCONTEXT        - True if unw_getcontext() is found (optional).
#   UNWIND_HAS_UNW_INIT_LOCAL        - True if unw_init_local() is found (optional).
#   UNWIND_HAS_UNW_BACKTRACE         - True if unw_backtrace() is found (required).
#   UNWIND_HAS_UNW_BACKTRACE_SKIP    - True if unw_backtrace_skip() is found (optional).
#   UNWIND_VERSION_STRING            - version number as a string (ex: "5.0.3")

#=============================================================================
# SPDX-FileCopyrightText: 2014 ZBackup contributors
# SPDX-License-Identifier: BSD-3-Clause


find_path(UNWIND_INCLUDE_DIR libunwind.h )
if(NOT EXISTS "${UNWIND_INCLUDE_DIR}/unwind.h")
  MESSAGE("Found libunwind.h but corresponding unwind.h is absent!")
  SET(UNWIND_INCLUDE_DIR "")
endif()

find_library(UNWIND_LIBRARY unwind)

if(UNWIND_INCLUDE_DIR AND EXISTS "${UNWIND_INCLUDE_DIR}/libunwind-common.h")
  file(STRINGS "${UNWIND_INCLUDE_DIR}/libunwind-common.h" UNWIND_HEADER_CONTENTS REGEX "#define UNW_VERSION_[A-Z]+\t[0-9]*")

  string(REGEX REPLACE ".*#define UNW_VERSION_MAJOR\t([0-9]*).*" "\\1" UNWIND_VERSION_MAJOR "${UNWIND_HEADER_CONTENTS}")
  string(REGEX REPLACE ".*#define UNW_VERSION_MINOR\t([0-9]*).*" "\\1" UNWIND_VERSION_MINOR "${UNWIND_HEADER_CONTENTS}")
  string(REGEX REPLACE ".*#define UNW_VERSION_EXTRA\t([0-9]*).*" "\\1" UNWIND_VERSION_EXTRA "${UNWIND_HEADER_CONTENTS}")

  if(UNWIND_VERSION_EXTRA)
    set(UNWIND_VERSION_STRING "${UNWIND_VERSION_MAJOR}.${UNWIND_VERSION_MINOR}.${UNWIND_VERSION_EXTRA}")
  else(not UNWIND_VERSION_EXTRA)
    set(UNWIND_VERSION_STRING "${UNWIND_VERSION_MAJOR}.${UNWIND_VERSION_MINOR}")
  endif()
  unset(UNWIND_HEADER_CONTENTS)
endif()

if (UNWIND_LIBRARY)
  include(CheckCSourceCompiles)
  set(CMAKE_REQUIRED_QUIET_SAVE ${CMAKE_REQUIRED_QUIET})
  set(CMAKE_REQUIRED_QUIET ${Libunwind_FIND_QUIETLY})
  set(CMAKE_REQUIRED_LIBRARIES_SAVE ${CMAKE_REQUIRED_LIBRARIES})
  set(CMAKE_REQUIRED_LIBRARIES ${UNWIND_LIBRARY})
  set(CMAKE_REQUIRED_INCLUDES_SAVE ${CMAKE_REQUIRED_INCLUDES})
  set(CMAKE_REQUIRED_INCLUDES ${UNWIND_INCLUDE_DIR})
  check_c_source_compiles("#define UNW_LOCAL_ONLY 1\n#include <libunwind.h>\nint main() { unw_context_t context; unw_getcontext(&context); return 0; }"
                          UNWIND_HAS_UNW_GETCONTEXT)
  check_c_source_compiles("#define UNW_LOCAL_ONLY 1\n#include <libunwind.h>\nint main() { unw_context_t context; unw_cursor_t cursor; unw_getcontext(&context); unw_init_local(&cursor, &context); return 0; }"
                          UNWIND_HAS_UNW_INIT_LOCAL)
  check_c_source_compiles("#define UNW_LOCAL_ONLY 1\n#include <libunwind.h>\nint main() { void* buf[10]; unw_backtrace(&buf, 10); return 0; }" UNWIND_HAS_UNW_BACKTRACE)
  check_c_source_compiles ("#define UNW_LOCAL_ONLY 1\n#include <libunwind.h>\nint main() { void* buf[10]; unw_backtrace_skip(&buf, 10, 2); return 0; }" UNWIND_HAS_UNW_BACKTRACE_SKIP)
  check_c_source_compiles ("#define UNW_LOCAL_ONLY 1\n#include <libunwind.h>\nint main() { return unw_set_cache_size(unw_local_addr_space, 1024, 0); }" UNWIND_HAS_UNW_SET_CACHE_SIZE)
  set(CMAKE_REQUIRED_QUIET ${CMAKE_REQUIRED_QUIET_SAVE})
  set(CMAKE_REQUIRED_LIBRARIES ${CMAKE_REQUIRED_LIBRARIES_SAVE})
  set(CMAKE_REQUIRED_INCLUDES ${CMAKE_REQUIRED_INCLUDES_SAVE})
endif ()

include(FindPackageHandleStandardArgs)
FIND_PACKAGE_HANDLE_STANDARD_ARGS(Libunwind  REQUIRED_VARS  UNWIND_INCLUDE_DIR
                                                            UNWIND_LIBRARY
                                                            UNWIND_HAS_UNW_BACKTRACE
                                             VERSION_VAR    UNWIND_VERSION_STRING
                                 )

if (UNWIND_FOUND)
  set(UNWIND_LIBRARIES ${UNWIND_LIBRARY})
  set(UNWIND_INCLUDE_DIRS ${UNWIND_INCLUDE_DIR})
endif ()

mark_as_advanced( UNWIND_INCLUDE_DIR UNWIND_LIBRARY )
