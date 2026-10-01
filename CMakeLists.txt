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
find_package(PkgConfig REQUIRED)
pkg_check_modules(sndfile REQUIRED IMPORTED_TARGET sndfile)
pkg_check_modules(vorbisfile REQUIRED IMPORTED_TARGET vorbisfile)
pkg_check_modules(freetype2 REQUIRED IMPORTED_TARGET freetype2)
pkg_check_modules(libzip REQUIRED IMPORTED_TARGET libzip)
set(VITA_APP_NAME "RollerCoaster Tycoon Classic")
set(VITA_TITLEID  "RCTCLASS1")
set(VITA_MKSFOEX_FLAGS "-d ATTRIBUTE2=12")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g -Wl,-q,--wrap,_fcntl_r,--wrap,abort,--wrap,FMOD_System_CreateSound,--wrap,FMOD_System_CreateStream,--allow-multiple-definition -D_GNU_SOURCE -Wall -O3 -fdiagnostics-color=always -fno-optimize-sibling-calls -mfloat-abi=softfp")
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

set(RCT_VITAGL_LIBRARY vitaGL CACHE STRING "vitaGL library for the normal executable")
set(RCT_SAFE_VITAGL_LIBRARY vitaGL CACHE STRING "vitaGL library without speedhacks for safe.bin")

set_target_properties(rct PROPERTIES LINKER_LANGUAGE CXX)

target_link_libraries(rct
  -Wl,--whole-archive pthread -Wl,--no-whole-archive
  -Wl,--start-group
  PkgConfig::sndfile
  PkgConfig::vorbisfile
  PkgConfig::freetype2
  PkgConfig::libzip
  freetype
  soloud
  png
  webp
  jpeg
  mikmod
  opus
  opusfile
  modplug
  zip
  zstd
  bz2
  lzma
  ssl
  crypto
  z
  m
  stdc++
  OpenSLES
  sndfile
  vorbis
  vorbisenc
  vorbisfile
  ogg
  FLAC
  openal
  imgui
  ${RCT_VITAGL_LIBRARY}
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
  SceShaccCg_stub
  SceSysmodule_stub
  ScePower_stub
  SceTouch_stub
  SceMotion_stub
  SceHid_stub
  SceVshBridge_stub
  SceKernelDmacMgr_stub
  -Wl,--end-group
)

get_target_property(RCT_SOURCES rct SOURCES)
get_target_property(RCT_SAFE_LIBRARIES rct LINK_LIBRARIES)
list(FIND RCT_SAFE_LIBRARIES "${RCT_VITAGL_LIBRARY}" RCT_VITAGL_INDEX)
list(REMOVE_AT RCT_SAFE_LIBRARIES ${RCT_VITAGL_INDEX})
list(INSERT RCT_SAFE_LIBRARIES ${RCT_VITAGL_INDEX} "${RCT_SAFE_VITAGL_LIBRARY}")
add_executable(rct_safe ${RCT_SOURCES})
set_target_properties(rct_safe PROPERTIES LINKER_LANGUAGE CXX)
target_link_libraries(rct_safe ${RCT_SAFE_LIBRARIES})
vita_create_self(safe.bin rct_safe UNSAFE)
if(CMAKE_VERSION VERSION_LESS "3.20.0")
  add_dependencies(rct safe.bin)
else()
  add_dependencies(rct safe.bin-self)
endif()

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
		${CMAKE_CURRENT_BINARY_DIR}/safe.bin safe.bin
		${CMAKE_SOURCE_DIR}/settings.cfg settings.cfg
)

set_property(SOURCE loader/so_util.c APPEND PROPERTY COMPILE_OPTIONS -Wno-error=return-mismatch -Wno-error=incompatible-pointer-types)
