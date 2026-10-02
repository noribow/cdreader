cmake_minimum_required(VERSION 3.16)

project(cdreader VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CDREADER_BUILD_TESTS "Build unit tests" ON)

# Ship a self-contained executable on Windows (no VC++ redistributable needed).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(MSVC)
  add_compile_options(/W4 /utf-8)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# --- Platform independent core (shared with the future Android port) --------
add_library(cdreader_core STATIC
  core/src/audio_writer.cpp
  core/src/cd_drive.cpp
  core/src/crc32.cpp
  core/src/cue_sheet.cpp
  core/src/file_name.cpp
  core/src/flac_encoder.cpp
  core/src/flac_writer.cpp
  core/src/md5.cpp
  core/src/metadata.cpp
  core/src/ogg.cpp
  core/src/ripper.cpp
  core/src/scsi.cpp
  core/src/tags.cpp
  core/src/toc.cpp
  core/src/wav_writer.cpp
)
target_include_directories(cdreader_core PUBLIC core/include)
target_compile_definitions(cdreader_core PRIVATE CDREADER_VERSION="${PROJECT_VERSION}")

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

if(CDREADER_BUILD_TESTS)
  enable_testing()
  add_executable(cdreader_tests
    tests/fake_drive.cpp
    tests/flac_decoder.cpp
    tests/test_main.cpp
  )
  target_include_directories(cdreader_tests PRIVATE tests)
  target_link_libraries(cdreader_tests PRIVATE cdreader_core)
  add_test(NAME cdreader_tests COMMAND cdreader_tests)

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
endif()
