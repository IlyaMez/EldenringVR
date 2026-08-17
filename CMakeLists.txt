cmake_minimum_required(VERSION 3.21)
project(EldenRingVR VERSION 0.3.0 LANGUAGES C CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "ELDEN RING is 64-bit; configure with a 64-bit toolchain (e.g. -A x64).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)

include(FetchContent)

# ---- MinHook (inline hooking) -------------------------------------------------
FetchContent_Declare(minhook
  GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
  GIT_TAG        v1.3.4
  GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(minhook)

# ---- OpenVR headers (we bind to the openvr_api.dll R.E.A.L. already loads) --
FetchContent_Declare(openvr
  GIT_REPOSITORY https://github.com/ValveSoftware/openvr.git
  GIT_TAG        v1.16.8
  GIT_SHALLOW    TRUE
  SOURCE_SUBDIR  headers_only_do_not_build)   # non-existent subdir => headers only, no targets
FetchContent_MakeAvailable(openvr)
set(OPENVR_INCLUDE_DIR "${openvr_SOURCE_DIR}/headers")

# ---- ERVR.dll: the mod --------------------------------------------------------
add_library(ERVR SHARED
  src/ervr/main.cpp
  src/ervr/log.cpp
  src/ervr/config.cpp
  src/ervr/vr_oculus.cpp
  src/ervr/vr_openvr.cpp
  src/ervr/vr_backend.cpp
  src/ervr/xinput_hook.cpp
  src/ervr/virtual_pad.cpp
  src/ervr/gestures.cpp
  src/ervr/game_memory.cpp
  src/ervr/midhook.cpp
  src/ervr/rtti.cpp
  src/ervr/game_er.cpp
  src/ervr/combat.cpp
  ${minhook_SOURCE_DIR}/src/hde/hde64.c)          # length disassembler bundled with MinHook, reused for mid-function hooks
target_include_directories(ERVR PRIVATE src/ervr ${OPENVR_INCLUDE_DIR} ${minhook_SOURCE_DIR}/src/hde)
target_link_libraries(ERVR PRIVATE minhook)
target_compile_definitions(ERVR PRIVATE ERVR_VERSION="${PROJECT_VERSION}")
set_target_properties(ERVR PROPERTIES OUTPUT_NAME "ERVR" PREFIX "")

# ---- dinput8.dll: proxy loader ----------------------------------------------
add_library(dinput8_proxy SHARED
  src/proxy/dinput8.cpp
  src/proxy/dinput8.def)
set_target_properties(dinput8_proxy PROPERTIES OUTPUT_NAME "dinput8" PREFIX "")

# ---- packaging ------------------------------------------------------------
set(DIST_DIR ${CMAKE_BINARY_DIR}/dist)
add_custom_target(dist ALL
  COMMAND ${CMAKE_COMMAND} -E make_directory ${DIST_DIR}/ERVR
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:dinput8_proxy> ${DIST_DIR}/dinput8.dll
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:ERVR>         ${DIST_DIR}/ERVR/ERVR.dll
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/config/ERVR.ini ${DIST_DIR}/ERVR/ERVR.ini
  DEPENDS ERVR dinput8_proxy
  COMMENT "Assembling redistributable in ${DIST_DIR}")
