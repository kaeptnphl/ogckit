cmake_minimum_required(VERSION 3.16)
project(ogckit LANGUAGES CXX)

# Backend, chosen at configure time: "libusb" (Linux, macOS, Windows; development),
# "iohid" (macOS HID driver, no root) or "ps4" (OpenOrbis/PacBrew toolchain).
set(OGC_BACKEND "libusb" CACHE STRING "USB backend: libusb, iohid or ps4")
set_property(CACHE OGC_BACKEND PROPERTY STRINGS libusb iohid ps4)
if(NOT OGC_BACKEND MATCHES "^(libusb|iohid|ps4)$")
  message(FATAL_ERROR "OGC_BACKEND must be libusb, iohid or ps4")
endif()
if(OGC_BACKEND STREQUAL "iohid" AND NOT APPLE)
  message(FATAL_ERROR "OGC_BACKEND=iohid requires macOS")
endif()

option(OGC_BUILD_LIBRARY "Build the library and terminal example" ON)
include(CTest)

# Keep the test-only mock runtime and queue tests free of hardware dependencies.
if(BUILD_TESTING)
  find_package(Threads REQUIRED)
  add_executable(test_xinput tests/test_xinput.cpp src/drv_xinput.cpp)
  target_include_directories(test_xinput PRIVATE include)
  target_compile_features(test_xinput PRIVATE cxx_std_17)
  add_test(NAME xinput COMMAND test_xinput)

  add_executable(test_core
      tests/test_core.cpp
      tests/backend_test.cpp
      src/core.cpp
      src/drv_xinput.cpp)
  target_include_directories(test_core PRIVATE include)
  target_compile_features(test_core PRIVATE cxx_std_17)
  target_link_libraries(test_core PRIVATE Threads::Threads)
  add_test(NAME core COMMAND test_core)
  set_tests_properties(core PROPERTIES TIMEOUT 20)

  add_executable(test_report_queue tests/test_report_queue.cpp)
  target_compile_features(test_report_queue PRIVATE cxx_std_17)
  target_link_libraries(test_report_queue PRIVATE Threads::Threads)
  add_test(NAME report_queue COMMAND test_report_queue)
  set_tests_properties(report_queue PROPERTIES TIMEOUT 20)
endif()

if(OGC_BUILD_LIBRARY)
  if(OGC_BACKEND STREQUAL "libusb" OR OGC_BACKEND STREQUAL "iohid")
    add_library(ogckit STATIC src/core.cpp src/drv_xinput.cpp src/backend_${OGC_BACKEND}.cpp)
    target_include_directories(ogckit PUBLIC include)
    target_compile_features(ogckit PRIVATE cxx_std_17)
    if(OGC_BACKEND STREQUAL "libusb")
      find_package(PkgConfig REQUIRED)
      pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)
      target_link_libraries(ogckit PUBLIC PkgConfig::LIBUSB)
    elseif(OGC_BACKEND STREQUAL "iohid")
      target_link_libraries(ogckit PUBLIC "-framework IOKit" "-framework CoreFoundation")
    else()
      set_source_files_properties(src/backend_ps4.cpp PROPERTIES
          COMPILE_OPTIONS "-idirafter;${OPENORBIS}/include/orbis/_types")
      target_link_libraries(ogckit PUBLIC SceSysmodule SceUsbd pthread)
    endif()
    find_package(Threads REQUIRED)
    target_link_libraries(ogckit PUBLIC Threads::Threads)

    if(NOT OGC_BACKEND STREQUAL "ps4")
      add_executable(ogc-terminal examples/terminal.cpp)
      target_link_libraries(ogc-terminal ogckit)
    endif()
  elseif(OGC_BACKEND STREQUAL "ps4")
    set_source_files_properties(src/backend_ps4.cpp PROPERTIES
        COMPILE_OPTIONS "-idirafter;${OPENORBIS}/include/orbis/_types")
    add_library(ogckit STATIC src/core.cpp src/drv_xinput.cpp src/backend_ps4.cpp)
    target_include_directories(ogckit PUBLIC include)
    target_compile_features(ogckit PRIVATE cxx_std_17)
    target_link_libraries(ogckit PUBLIC SceSysmodule SceUsbd pthread)
  endif()
endif()
