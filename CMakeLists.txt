cmake_minimum_required(VERSION 3.16)
project(ogckit LANGUAGES CXX)

# Backend: "libusb" (Linux, macOS, Windows; development) or "ps4" (OpenOrbis/PacBrew toolchain).
set(OGC_BACKEND "libusb" CACHE STRING "USB backend: libusb or ps4")

add_library(ogckit STATIC src/core.cpp src/drv_xinput.cpp src/backend_${OGC_BACKEND}.cpp)
target_include_directories(ogckit PUBLIC include)
target_compile_features(ogckit PRIVATE cxx_std_17)

if(OGC_BACKEND STREQUAL "libusb")
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(LIBUSB REQUIRED libusb-1.0)
  target_include_directories(ogckit PRIVATE ${LIBUSB_INCLUDE_DIRS})
  target_link_libraries(ogckit PUBLIC ${LIBUSB_LIBRARIES})
  target_link_directories(ogckit PUBLIC ${LIBUSB_LIBRARY_DIRS})
  find_package(Threads REQUIRED)
  target_link_libraries(ogckit PUBLIC Threads::Threads)

  add_executable(ogc-terminal examples/terminal.cpp)
  target_link_libraries(ogc-terminal ogckit)
endif()

if(OGC_BACKEND STREQUAL "ps4")
  # orbis/Usbd.h includes <libusb.h>, kept in include/orbis/_types, which also has a pthread.h,
  # so it must come after the system headers (-idirafter). OPENORBIS is set by the PacBrew toolchain.
  set_source_files_properties(src/backend_ps4.cpp PROPERTIES
      COMPILE_OPTIONS "-idirafter;${OPENORBIS}/include/orbis/_types")
  target_link_libraries(ogckit PUBLIC SceSysmodule SceUsbd pthread)
endif()

enable_testing()
if(NOT OGC_BACKEND STREQUAL "ps4")
add_executable(test_xinput tests/test_xinput.cpp)
target_link_libraries(test_xinput ogckit)
add_test(NAME xinput COMMAND test_xinput)
endif()
