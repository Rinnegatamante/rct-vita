cmake_minimum_required(VERSION 2.8)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(rct C CXX)
include("${VITASDK}/share/vita.cmake" REQUIRED)
set(VITA_APP_NAME "RollerCoaster Tycoon Classic")
set(VITA_TITLEID  "RCTCLASS1")
set(VITA_MKSFOEX_FLAGS "-d ATTRIBUTE2=12")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g -Wl,-q,--wrap,abort,--wrap,FMOD_System_CreateSound,--wrap,FMOD_System_CreateStream,--allow-multiple-definition -D_GNU_SOURCE -Wall -O3 -fdiagnostics-color=always -fno-optimize-sibling-calls -mfloat-abi=softfp")
set(CMAKE_CXX_FLAGS "${CMAKE_C_FLAGS} -std=c++11 -Wno-write-strings -fpermissive -fno-rtti")

add_executable(rct
  loader/main.c
  loader/dialog.c
  loader/so_util.c
  loader/sha1.c
  loader/fnmatch.c
  loader/pthr.c
  loader/ctype_patch.c
  loader/ioapi.c
  loader/unzip.c
)

target_link_libraries(rct
  -Wl,--whole-archive pthread -Wl,--no-whole-archive
  freetype
  soloud_static
  png
  webp
  jpeg
  mikmod
  opus
  opusfile
  modplug
  zip
  bz2
  lzma
  ssl
  crypto
  z
  m
  stdc++
  opensles
  sndfile
  vorbis
  vorbisenc
  vorbisfile
  ogg
  FLAC
  openal
  imgui
  vitaGL
  vitashark
  SceShaccCgExt
  mathneon
  mpg123
  fmodpp
  fmodstudio_stub
  taihen_stub
  kubridge_stub
  SceNet_stub
  SceNetCtl_stub
  SceAppMgr_stub
  SceAppUtil_stub
  SceAudio_stub
  SceAudioIn_stub
  SceCtrl_stub
  SceCommonDialog_stub
  SceDisplay_stub
  SceFios2_stub
  SceGxm_stub
  SceIme_stub
  SceLibcBridge_stub
  SceShaccCg_stub
  SceSysmodule_stub
  ScePower_stub
  SceTouch_stub
  SceMotion_stub
  SceHid_stub
  SceVshBridge_stub
  SceKernelDmacmgr_stub
)

vita_create_self(eboot.bin rct UNSAFE)
vita_create_vpk(rct.vpk ${VITA_TITLEID} eboot.bin
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE ${CMAKE_SOURCE_DIR}/livearea/icon0.png sce_sys/icon0.png
		${CMAKE_SOURCE_DIR}/livearea/bg0.png sce_sys/livearea/contents/bg0.png
		${CMAKE_SOURCE_DIR}/livearea/startup.png sce_sys/livearea/contents/startup.png
		${CMAKE_SOURCE_DIR}/livearea/pic0.png sce_sys/pic0.png
		${CMAKE_SOURCE_DIR}/livearea/button.png sce_sys/livearea/contents/button.png
		${CMAKE_SOURCE_DIR}/livearea/template.xml sce_sys/livearea/contents/template.xml
		${CMAKE_SOURCE_DIR}/build/safe.bin safe.bin
		${CMAKE_SOURCE_DIR}/settings.cfg settings.cfg
)
