cmake_minimum_required(VERSION 3.20)
project(msdisp_portrait CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
add_compile_options(-Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
# Self-contained binaries (no MinGW runtime DLLs needed).
add_link_options(-static -static-libgcc -static-libstdc++)

include_directories(${CMAKE_SOURCE_DIR}/include)

# --- common ---------------------------------------------------------------
add_library(msdisp_common STATIC src/common/common.cpp)

# --- track A: USB / protocol ----------------------------------------------
add_library(msdisp_usb STATIC
  src/usb/libusb0_api.cpp
  src/usb/hid_control.cpp
  src/usb/ms912x_device.cpp)
target_link_libraries(msdisp_usb PUBLIC msdisp_common hid setupapi)

# --- track B: virtual display + capture ------------------------------------
file(GLOB VDD_SRC CONFIGURE_DEPENDS src/vdd/*.cpp)
file(GLOB CAPTURE_SRC CONFIGURE_DEPENDS src/capture/*.cpp)
add_library(msdisp_vdd STATIC ${VDD_SRC})
target_link_libraries(msdisp_vdd PUBLIC msdisp_common setupapi advapi32 user32)
add_library(msdisp_capture STATIC ${CAPTURE_SRC})
target_link_libraries(msdisp_capture PUBLIC msdisp_common d3d11 dxgi)

# --- track C: GPU ------------------------------------------------------------
file(GLOB GPU_SRC CONFIGURE_DEPENDS src/gpu/*.cpp)
add_library(msdisp_gpu STATIC ${GPU_SRC})
target_link_libraries(msdisp_gpu PUBLIC msdisp_common d3d11 dxgi d3dcompiler)

# --- integration: pipeline -----------------------------------------------------
add_library(msdisp_pipeline STATIC src/pipeline/pipeline.cpp src/pipeline/layout.cpp)
target_link_libraries(msdisp_pipeline PUBLIC msdisp_usb msdisp_vdd msdisp_capture msdisp_gpu winmm)

add_executable(portrait-run tools/portrait-run/main.cpp)
target_link_libraries(portrait-run PRIVATE msdisp_pipeline)

# --- track D: app shell -------------------------------------------------------
file(GLOB APP_CORE_SRC CONFIGURE_DEPENDS src/app/core/*.cpp)
add_library(msdisp_appcore STATIC ${APP_CORE_SRC})
target_link_libraries(msdisp_appcore PUBLIC msdisp_common shell32 ole32 oleaut32 uuid user32 advapi32)

file(GLOB APP_SRC CONFIGURE_DEPENDS src/app/*.cpp src/app/*.rc)
add_executable(msdisp WIN32 ${APP_SRC})
target_link_options(msdisp PRIVATE -municode)
target_link_libraries(msdisp PRIVATE msdisp_appcore msdisp_pipeline msdisp_usb msdisp_vdd msdisp_capture msdisp_gpu
  comctl32 shell32 user32 gdi32)

# --- tools ------------------------------------------------------------------
add_executable(msdisp-probe tools/probe/main.cpp)
target_link_libraries(msdisp-probe PRIVATE msdisp_usb)

file(GLOB TOOL_DIRS LIST_DIRECTORIES true ${CMAKE_SOURCE_DIR}/tools/*)
foreach(dir ${TOOL_DIRS})
  if(IS_DIRECTORY ${dir} AND EXISTS ${dir}/CMakeLists.txt)
    add_subdirectory(${dir})
  endif()
endforeach()

# --- tests ------------------------------------------------------------------
enable_testing()
file(GLOB TEST_SRC CONFIGURE_DEPENDS tests/test_*.cpp)
foreach(src ${TEST_SRC})
  get_filename_component(name ${src} NAME_WE)
  add_executable(${name} ${src})
  target_link_libraries(${name} PRIVATE msdisp_usb msdisp_vdd msdisp_capture msdisp_gpu msdisp_appcore)
  add_test(NAME ${name} COMMAND ${name})
endforeach()
