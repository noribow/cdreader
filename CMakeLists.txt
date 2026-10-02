# 3.18: FetchContent SOURCE_SUBDIR (cmake/Codecs.cmake)
cmake_minimum_required(VERSION 3.18)

project(cdreader VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CDREADER_BUILD_TESTS "Build unit tests" ON)
# The Android app builds this project through Gradle (android/app) with ANDROID set.
option(CDREADER_BUILD_JNI "Build the JNI library for the Android app" ${ANDROID})

if(CDREADER_BUILD_JNI)
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)  # static libraries end up in a shared library
endif()

# Ship a self-contained executable on Windows (no VC++ redistributable needed).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Lossy codec libraries (Opus, Vorbis), before our warning flags are set.
include(cmake/Codecs.cmake)

if(MSVC)
  add_compile_options(/W4 /utf-8)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# --- Platform independent core (shared by Windows and Android) --------------
add_library(cdreader_core STATIC
  core/src/accuraterip.cpp
  core/src/alac_encoder.cpp
  core/src/alac_writer.cpp
  core/src/audio_writer.cpp
  core/src/cddb.cpp
  core/src/cd_drive.cpp
  core/src/clock.cpp
  core/src/crc32.cpp
  core/src/cue_sheet.cpp
  core/src/drive_cache.cpp
  core/src/file_naming.cpp
  core/src/flac_encoder.cpp
  core/src/flac_writer.cpp
  core/src/gaps.cpp
  core/src/matroska.cpp
  core/src/md5.cpp
  core/src/metadata.cpp
  core/src/mka_writer.cpp
  core/src/mp4.cpp
  core/src/offset_detect.cpp
  core/src/ogg.cpp
  core/src/ogg_flac_writer.cpp
  core/src/resampler.cpp
  core/src/ripper.cpp
  core/src/scsi.cpp
  core/src/settings_store.cpp
  core/src/subchannel.cpp
  core/src/tags.cpp
  core/src/toc.cpp
  core/src/wav_writer.cpp
)
target_include_directories(cdreader_core PUBLIC core/include)
target_compile_definitions(cdreader_core PRIVATE CDREADER_VERSION="${PROJECT_VERSION}")
# PUBLIC: users see CDREADER_HAVE_OPUS / CDREADER_HAVE_VORBIS (and the tests the libraries).
target_link_libraries(cdreader_core PUBLIC cdreader_codecs)
if(CDREADER_WITH_OPUS)
  target_sources(cdreader_core PRIVATE core/src/opus_writer.cpp)
endif()
if(CDREADER_WITH_VORBIS)
  target_sources(cdreader_core PRIVATE core/src/vorbis_writer.cpp)
endif()

# --- Windows: SCSI pass-through transport + command line front end ---------
if(WIN32)
  add_library(cdreader_windows STATIC
    platform/windows/spti_transport.cpp
    platform/windows/winhttp_client.cpp
  )
  target_include_directories(cdreader_windows PUBLIC platform/windows)
  target_link_libraries(cdreader_windows PUBLIC cdreader_core winhttp)
  target_compile_definitions(cdreader_windows PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

  add_executable(cdreader app/cli/main.cpp)
  target_link_libraries(cdreader PRIVATE cdreader_windows)
  if(MINGW)
    target_link_options(cdreader PRIVATE -static)
  endif()
  install(TARGETS cdreader RUNTIME DESTINATION .)
endif()

# --- Android: USB mass storage Bulk-Only Transport + JNI bridge ------------
# The BOT protocol is portable and unit tested on every platform; the
# usbdevfs endpoint I/O needs Linux / Android headers.
add_library(cdreader_usb STATIC platform/android/bot_transport.cpp)
target_include_directories(cdreader_usb PUBLIC platform/android)
target_link_libraries(cdreader_usb PUBLIC cdreader_core)
if(CMAKE_SYSTEM_NAME MATCHES "^(Linux|Android)$")
  target_sources(cdreader_usb PRIVATE platform/android/usbdevfs_endpoints.cpp)
endif()

# The app's ripping workflow (CDDB, WAV / FLAC, AccurateRip, rip.log) without
# JNI types, so that it is unit tested on the host as well.
add_library(cdreader_rip_session STATIC platform/android/rip_session.cpp)
target_include_directories(cdreader_rip_session PUBLIC platform/android)
target_link_libraries(cdreader_rip_session PUBLIC cdreader_core)
target_compile_definitions(cdreader_rip_session PRIVATE CDREADER_VERSION="${PROJECT_VERSION}")

if(CDREADER_BUILD_JNI)
  add_library(cdreader_jni SHARED platform/android/jni_bridge.cpp)
  target_link_libraries(cdreader_jni PRIVATE cdreader_rip_session cdreader_usb)
  target_compile_definitions(cdreader_jni PRIVATE CDREADER_VERSION="${PROJECT_VERSION}")
  if(NOT ANDROID)
    # Host build (Linux), only to check that the bridge compiles.
    find_package(JNI REQUIRED)
    target_include_directories(cdreader_jni PRIVATE ${JNI_INCLUDE_DIRS})
  endif()
endif()

if(CDREADER_BUILD_TESTS)
  enable_testing()
  add_executable(cdreader_tests
    tests/alac_decoder.cpp
    tests/fake_drive.cpp
    tests/flac_decoder.cpp
    tests/test_main.cpp
  )
  target_include_directories(cdreader_tests PRIVATE tests)
  target_link_libraries(cdreader_tests PRIVATE cdreader_core)
  add_test(NAME cdreader_tests COMMAND cdreader_tests)

  add_executable(cdreader_usb_tests
    tests/fake_drive.cpp
    tests/fake_usb_device.cpp
    tests/usb_transport_tests.cpp
  )
  target_include_directories(cdreader_usb_tests PRIVATE tests)
  target_link_libraries(cdreader_usb_tests PRIVATE cdreader_usb)
  add_test(NAME cdreader_usb_tests COMMAND cdreader_usb_tests)

  # The Android rip workflow over the BOT fake USB device and canned HTTP.
  add_executable(cdreader_rip_session_tests
    tests/alac_decoder.cpp
    tests/fake_drive.cpp
    tests/fake_usb_device.cpp
    tests/flac_decoder.cpp
    tests/rip_session_tests.cpp
  )
  target_include_directories(cdreader_rip_session_tests PRIVATE tests)
  target_link_libraries(cdreader_rip_session_tests PRIVATE cdreader_rip_session cdreader_usb)
  add_test(NAME cdreader_rip_session_tests COMMAND cdreader_rip_session_tests)

  # Matroska writer (#23): EBML layout and the codecs inside, decoded in process.
  add_executable(cdreader_mka_tests tests/flac_decoder.cpp tests/mka_tests.cpp)
  target_include_directories(cdreader_mka_tests PRIVATE tests)
  target_link_libraries(cdreader_mka_tests PRIVATE cdreader_core)
  add_test(NAME cdreader_mka_tests COMMAND cdreader_mka_tests)

  # Round trip through the reference FLAC tool (skipped when `flac` is not installed).
  add_executable(cdreader_flac_roundtrip tests/flac_roundtrip.cpp)
  target_include_directories(cdreader_flac_roundtrip PRIVATE tests)
  target_link_libraries(cdreader_flac_roundtrip PRIVATE cdreader_core)
  find_program(FLAC_EXECUTABLE flac)
  if(FLAC_EXECUTABLE)
    add_test(NAME flac_roundtrip COMMAND cdreader_flac_roundtrip "${FLAC_EXECUTABLE}")
  else()
    message(STATUS "flac not found: the FLAC reference round trip test will be skipped")
    add_test(NAME flac_roundtrip COMMAND cdreader_flac_roundtrip)
  endif()
  set_tests_properties(flac_roundtrip PROPERTIES SKIP_RETURN_CODE 77)

  # Embedded CUE sheet checked with flac / metaflac (skipped when not installed).
  add_executable(cdreader_flac_cuesheet_check tests/flac_cuesheet_check.cpp)
  target_link_libraries(cdreader_flac_cuesheet_check PRIVATE cdreader_core)
  find_program(METAFLAC_EXECUTABLE metaflac)
  find_program(FFPROBE_EXECUTABLE ffprobe)
  if(FLAC_EXECUTABLE AND METAFLAC_EXECUTABLE)
    # ffprobe is optional (tag check of single tracks, #22).
    set(_ffprobe_arg)
    if(FFPROBE_EXECUTABLE)
      set(_ffprobe_arg "${FFPROBE_EXECUTABLE}")
    endif()
    add_test(NAME flac_cuesheet COMMAND cdreader_flac_cuesheet_check "${FLAC_EXECUTABLE}" "${METAFLAC_EXECUTABLE}"
             ${_ffprobe_arg})
  else()
    add_test(NAME flac_cuesheet COMMAND cdreader_flac_cuesheet_check)
  endif()
  set_tests_properties(flac_cuesheet PROPERTIES SKIP_RETURN_CODE 77)

  # Opus / Vorbis output decoded by the reference tools (opus-tools,
  # vorbis-tools; ffprobe for the tags). Skipped when none is installed.
  add_executable(cdreader_lossy_decode_check tests/lossy_decode_check.cpp)
  target_include_directories(cdreader_lossy_decode_check PRIVATE tests)
  target_link_libraries(cdreader_lossy_decode_check PRIVATE cdreader_core)
  set(_lossy_tools)
  foreach(_tool opusdec opusinfo oggdec ogginfo ffprobe)
    string(TOUPPER "${_tool}" _var)
    find_program(${_var}_EXECUTABLE ${_tool})
    if(${_var}_EXECUTABLE)
      list(APPEND _lossy_tools --${_tool} "${${_var}_EXECUTABLE}")
    endif()
  endforeach()
  add_test(NAME lossy_decode COMMAND cdreader_lossy_decode_check ${_lossy_tools})
  set_tests_properties(lossy_decode PROPERTIES SKIP_RETURN_CODE 77)

  # Ogg FLAC output checked with flac (decoding, CUESHEET through --cue),
  # ogginfo, ffmpeg and ffprobe; the missing tools are skipped (all: 77).
  add_executable(cdreader_ogg_flac_check tests/ogg_flac_check.cpp)
  target_include_directories(cdreader_ogg_flac_check PRIVATE tests)
  target_link_libraries(cdreader_ogg_flac_check PRIVATE cdreader_core)
  find_program(FFMPEG_EXECUTABLE ffmpeg)
  set(_oggflac_tools)
  foreach(_tool flac ogginfo ffmpeg ffprobe)
    string(TOUPPER "${_tool}" _var)
    if(${_var}_EXECUTABLE)
      list(APPEND _oggflac_tools --${_tool} "${${_var}_EXECUTABLE}")
    endif()
  endforeach()
  add_test(NAME ogg_flac_check COMMAND cdreader_ogg_flac_check ${_oggflac_tools})
  set_tests_properties(ogg_flac_check PROPERTIES SKIP_RETURN_CODE 77)

  # Matroska output checked with mkvinfo (mkvtoolnix), ffprobe and ffmpeg.
  # Skipped when none is installed.
  add_executable(cdreader_mka_check tests/mka_check.cpp)
  target_include_directories(cdreader_mka_check PRIVATE tests)
  target_link_libraries(cdreader_mka_check PRIVATE cdreader_core)
  set(_mka_tools)
  foreach(_tool mkvinfo ffprobe ffmpeg)
    string(TOUPPER "${_tool}" _var)
    find_program(${_var}_EXECUTABLE ${_tool})
    if(${_var}_EXECUTABLE)
      list(APPEND _mka_tools --${_tool} "${${_var}_EXECUTABLE}")
    endif()
  endforeach()
  add_test(NAME mka_check COMMAND cdreader_mka_check ${_mka_tools})
  set_tests_properties(mka_check PROPERTIES SKIP_RETURN_CODE 77)
  # ALAC (.m4a) decoded by FFmpeg (bit-exact PCM, tags via ffprobe); also
  # compares the size with our FLAC and FFmpeg's ALAC encoder. Skipped without ffmpeg.
  add_executable(cdreader_alac_ffmpeg_check tests/alac_ffmpeg_check.cpp tests/alac_decoder.cpp)
  target_include_directories(cdreader_alac_ffmpeg_check PRIVATE tests)
  target_link_libraries(cdreader_alac_ffmpeg_check PRIVATE cdreader_core)
  set(_alac_tools)
  foreach(_tool ffmpeg ffprobe)
    string(TOUPPER "${_tool}" _var)
    find_program(${_var}_EXECUTABLE ${_tool})
    if(${_var}_EXECUTABLE)
      list(APPEND _alac_tools --${_tool} "${${_var}_EXECUTABLE}")
    endif()
  endforeach()
  add_test(NAME alac_ffmpeg COMMAND cdreader_alac_ffmpeg_check ${_alac_tools})
  set_tests_properties(alac_ffmpeg PROPERTIES SKIP_RETURN_CODE 77)
endif()
