cmake_minimum_required(VERSION 3.10)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake"
        CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Set the VITASDK environment variable first.")
  endif()
endif()

project(vitastremio C)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "vitastremio")
set(VITA_TITLEID  "VSTR00001")
set(VITA_VERSION  "01.02")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -O2 -std=gnu11")

add_executable(${PROJECT_NAME}
  src/main.c
  src/player.c
)

# -Werror is deliberately NOT set: the vitasdk headers emit warnings we
# cannot fix, so it would break the build for reasons unrelated to our code.
# The host tests under test/ carry -fsanitize instead, which catches more.

target_include_directories(${PROJECT_NAME} PRIVATE src)

# Header-only modules used by the sources above: annexb.h lineproto.h
# ring.h config.h ime.h log.h http.h

# Link order matters on vitasdk: vita2d pulls in the graphics stack, and
# the codec stubs must follow the libs that reference them.
target_link_libraries(${PROJECT_NAME}
  vita2d
  SceLibKernel_stub
  SceDisplay_stub
  SceGxm_stub
  SceSysmodule_stub
  SceCtrl_stub
  SceTouch_stub
  SceCommonDialog_stub
  SceAppMgr_stub
  SceAudio_stub
  SceVideodec_stub
  SceIofilemgr_stub      # sceIoOpen/Write -- config + log files
  SceRtc_stub            # sceRtcGetCurrentTick -- log timestamps
  ScePower_stub          # sceKernelPowerTick -- keeps the screen awake
  SceNet_stub
  SceNetCtl_stub
  ScePgf_stub
  freetype
  png
  jpeg
  z
  m
)

vita_create_self(${PROJECT_NAME}.self ${PROJECT_NAME} UNSAFE)

vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} ${PROJECT_NAME}.self
  VERSION ${VITA_VERSION}
  NAME     ${VITA_APP_NAME}
  FILE       ${CMAKE_SOURCE_DIR}/assets/icon0.png   icon0.png ${CMAKE_SOURCE_DIR}/assets/bg0.png   bg0.png ${CMAKE_SOURCE_DIR}/assets/startup.png   startup.png
)
