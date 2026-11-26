cmake_minimum_required(VERSION 3.20)

# Read the public header so library and package versions share one source.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/include/js8core/version.hpp")
foreach(component MAJOR MINOR PATCH)
  file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/include/js8core/version.hpp"
    version_line REGEX "^#define JS8CORE_VERSION_${component} [0-9]+$")
  if(NOT version_line MATCHES "^#define JS8CORE_VERSION_${component} ([0-9]+)$")
    message(FATAL_ERROR "Missing or invalid JS8CORE_VERSION_${component} in version.hpp")
  endif()
  set(JS8CORE_VERSION_${component} "${CMAKE_MATCH_1}")
endforeach()
project(js8core
  VERSION ${JS8CORE_VERSION_MAJOR}.${JS8CORE_VERSION_MINOR}.${JS8CORE_VERSION_PATCH}
  LANGUAGES CXX)

# js8core — independent JS8 modem library.
# On-air protocol tables are retained at their canonical bit/index values.
# Derived JSC search and LDPC adjacency tables are generated; receive FIR
# coefficients are independently designed. See docs/protocol-data-provenance.md.

option(JS8CORE_WITH_VARI "Build varicode.cpp (required by the engine)" ON)
if(NOT JS8CORE_WITH_VARI)
  message(FATAL_ERROR "JS8CORE_WITH_VARI=OFF is not supported while the engine depends on varicode")
endif()

include(CTest)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
find_package(Threads REQUIRED)

add_library(js8core STATIC
  src/version.cpp
  src/placeholder.cpp
  src/engine/engine.cpp
  src/decoder/legacy_decoder.cpp
  src/decoder/ldpc.cpp
  src/decoder/symbol_tracking.cpp
  src/decoder/tracked_reference.cpp
  src/dsp/flatten.cpp
  src/dsp/resampler.cpp
  src/tx/modulator.cpp
  src/protocol/submode.cpp
  src/protocol/costas.cpp
  src/protocol/constants.cpp
  src/protocol/jsc.cpp
  src/protocol/jsc_map.cpp
)

if(JS8CORE_WITH_VARI)
  target_sources(js8core PRIVATE
    src/protocol/varicode_bits.cpp
    src/protocol/varicode_callsign.cpp
    src/protocol/varicode_commands.cpp
    src/protocol/varicode_frames.cpp
    src/protocol/varicode_support.cpp
  )
endif()

add_library(js8core::js8core ALIAS js8core)

target_compile_features(js8core PUBLIC cxx_std_20)

target_include_directories(js8core
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor
)
target_link_libraries(js8core PUBLIC Threads::Threads)

# Android: resolve the guarded __android_log_print() calls in the decoder.
if(ANDROID)
  target_link_libraries(js8core PUBLIC log)
endif()

# External dependencies used by specific TUs. These are header-only / system
# libs; discover them via common local include paths (e.g. Homebrew).
# NOTE: these are needed only to build the full library in-tree here; the
# consuming project normally provides them via its own build system.

# Eigen -> src/dsp/flatten.cpp and src/decoder/legacy_decoder.cpp
find_path(EIGEN3_INCLUDE_DIR Eigen/Dense
  PATHS /opt/homebrew/include/eigen3
         /opt/homebrew/anaconda3/include/eigen3
         /usr/local/include/eigen3
         /usr/include/eigen3
         ${CMAKE_CURRENT_SOURCE_DIR}/../vendor
         ${CMAKE_CURRENT_SOURCE_DIR}/../vendor/eigen3
         /opt/homebrew/include /opt/homebrew/anaconda3/include /usr/local/include /usr/include)
if(EIGEN3_INCLUDE_DIR)
  target_include_directories(js8core PRIVATE ${EIGEN3_INCLUDE_DIR})
endif()

# CRCpp -> src/protocol/varicode_bits.cpp (#include <vendor/CRCpp/CRC.h>).
# A minimal standard-CRC shim is vendored under vendor/CRCpp/ for in-tree builds.

# Test tools (EXCLUDE_FROM_ALL): build manually when needed.
add_executable(js8core-version-test EXCLUDE_FROM_ALL tools/version_test.cpp)
target_link_libraries(js8core-version-test PRIVATE js8core)
if(BUILD_TESTING)
  add_test(NAME js8core-version COMMAND js8core-version-test)
endif()

if(JS8CORE_WITH_VARI)
  add_executable(js8core-varicode-test EXCLUDE_FROM_ALL
    tools/varicode_roundtrip.cpp
  )
  target_link_libraries(js8core-varicode-test PRIVATE js8core)
endif()

add_executable(js8core-policy-test EXCLUDE_FROM_ALL
  tools/varicode_policy_test.cpp
)
target_compile_features(js8core-policy-test PRIVATE cxx_std_20)
target_link_libraries(js8core-policy-test PRIVATE js8core)

add_executable(js8core-vox-test EXCLUDE_FROM_ALL
  tools/vox_modulator_test.cpp
  src/tx/modulator.cpp
)

add_executable(js8core-clock-math-test EXCLUDE_FROM_ALL
  tools/clock_math_test.cpp
)
target_compile_features(js8core-clock-math-test PRIVATE cxx_std_20)
target_compile_features(js8core-vox-test PRIVATE cxx_std_20)
target_link_libraries(js8core-vox-test PRIVATE js8core)

# Standalone protocol checks (excluded from the default build, but registered in CTest).
add_executable(verify_jsc_roundtrip EXCLUDE_FROM_ALL
  tools/verify_jsc_roundtrip.cpp
)
target_include_directories(verify_jsc_roundtrip PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/protocol)
target_link_libraries(verify_jsc_roundtrip PRIVATE js8core)

add_executable(js8core-crc-test EXCLUDE_FROM_ALL tools/crc_test.cpp)
target_include_directories(js8core-crc-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(js8core-crc-test PRIVATE cxx_std_20)

add_executable(js8core-fractional-rx-test EXCLUDE_FROM_ALL
  tools/fractional_resampler_test.cpp
  src/dsp/resampler.cpp
)
target_compile_features(js8core-fractional-rx-test PRIVATE cxx_std_20)
target_include_directories(js8core-fractional-rx-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_executable(js8core-decoder-roundtrip-test EXCLUDE_FROM_ALL
  tools/decoder_roundtrip_test.cpp
)
target_compile_features(js8core-decoder-roundtrip-test PRIVATE cxx_std_20)
target_include_directories(js8core-decoder-roundtrip-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(js8core-decoder-roundtrip-test PRIVATE js8core)

add_executable(js8core-tracking-waveform-test EXCLUDE_FROM_ALL
  tools/tracking_waveform_test.cpp
)
target_compile_features(js8core-tracking-waveform-test PRIVATE cxx_std_20)

add_executable(js8core-symbol-tracking-test EXCLUDE_FROM_ALL
  tools/symbol_tracking_test.cpp
)
target_include_directories(js8core-symbol-tracking-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(js8core-symbol-tracking-test PRIVATE js8core)

add_executable(js8core-tracked-extraction-test EXCLUDE_FROM_ALL
  tools/tracked_extraction_test.cpp
)
target_include_directories(js8core-tracked-extraction-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(js8core-tracked-extraction-test PRIVATE js8core)

add_executable(js8core-tracked-reference-test EXCLUDE_FROM_ALL
  tools/tracked_reference_test.cpp
)
target_include_directories(js8core-tracked-reference-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(js8core-tracked-reference-test PRIVATE js8core)

add_executable(js8core-engine-tracking-test EXCLUDE_FROM_ALL tools/engine_tracking_test.cpp)
target_link_libraries(js8core-engine-tracking-test PRIVATE js8core)
add_executable(js8core-decode-control-test EXCLUDE_FROM_ALL tools/decode_control_test.cpp)
target_include_directories(js8core-decode-control-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(js8core-decode-control-test PRIVATE js8core)

add_executable(js8core-tracking-baseline EXCLUDE_FROM_ALL
  tools/tracking_baseline.cpp
)
target_link_libraries(js8core-tracking-baseline PRIVATE js8core)

if(BUILD_TESTING)
  add_test(NAME js8core-fractional-rx COMMAND js8core-fractional-rx-test)
  add_test(NAME js8core-crc COMMAND js8core-crc-test)
  add_test(NAME js8core-varicode COMMAND js8core-varicode-test)
  add_test(NAME js8core-policy COMMAND js8core-policy-test)
  add_test(NAME js8core-jsc COMMAND verify_jsc_roundtrip)
  add_test(NAME js8core-vox COMMAND js8core-vox-test)
  add_test(NAME js8core-clock-math COMMAND js8core-clock-math-test)
  add_test(NAME js8core-decoder-roundtrip COMMAND js8core-decoder-roundtrip-test)
  add_test(NAME js8core-tracking-waveform COMMAND js8core-tracking-waveform-test)
  add_test(NAME js8core-symbol-tracking COMMAND js8core-symbol-tracking-test)
  add_test(NAME js8core-tracked-extraction COMMAND js8core-tracked-extraction-test)
  add_test(NAME js8core-tracked-reference COMMAND js8core-tracked-reference-test)
  add_test(NAME js8core-tracking-baseline COMMAND js8core-tracking-baseline)
  add_test(NAME js8core-tracking-enabled COMMAND js8core-tracking-baseline --tracking both)
  add_test(NAME js8core-tracking-recovery COMMAND js8core-tracking-baseline
    --tracking both --sweep 3 --case tracking-drift --require-tracked)
  add_test(NAME js8core-tracking-negative COMMAND js8core-tracking-baseline
    --tracking both --sweep 3 --case single-tone)
  add_test(NAME js8core-tracking-wrap COMMAND js8core-tracking-baseline
    --tracking both --sweep 3 --case tracking-drift --require-tracked --wrap)
  add_test(NAME js8core-tracked-sic COMMAND js8core-tracking-baseline
    --tracking both --tracked-sic --sweep 3 --case tracking-drift --require-cancellation)
  add_test(NAME js8core-tracked-sic-followup COMMAND js8core-tracking-baseline
    --tracking both --tracked-sic --sweep 8 --mode I --case tracking-overlap --require-followup)
  add_test(NAME js8core-tracked-sic-wrap COMMAND js8core-tracking-baseline
    --tracking both --tracked-sic --sweep 8 --mode I --case tracking-overlap --require-followup --wrap)
  add_test(NAME js8core-tracked-sic-inert COMMAND js8core-tracking-baseline
    --tracking frequency --tracked-sic --sweep 3 --case tracking-drift --require-tracked)
  add_test(NAME js8core-tracking-defaults COMMAND js8core-tracking-baseline
    --library-defaults --sweep 8 --mode I --case tracking-overlap --require-followup)
  add_test(NAME js8core-engine-tracking COMMAND js8core-engine-tracking-test)
  add_test(NAME js8core-decode-control COMMAND js8core-decode-control-test)
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(NAME js8core-wav-tracking-tool
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/wav_tracking_test.py
        $<TARGET_FILE:js8core-wav-decode-test> --work-dir ${CMAKE_CURRENT_BINARY_DIR})
    add_test(NAME js8core-wav-reference-comparator
      COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/tools/reference/compare_wav_results_test.py)
  endif()
  add_test(NAME js8core-engine-smoke COMMAND js8core-engine-smoke-test)
endif()

add_executable(js8core-wav-decode-test EXCLUDE_FROM_ALL
  tools/wav_decode_test.cpp
)
target_compile_features(js8core-wav-decode-test PRIVATE cxx_std_20)
target_link_libraries(js8core-wav-decode-test PRIVATE js8core)

add_executable(js8core-engine-smoke-test EXCLUDE_FROM_ALL
  tools/engine_smoke_test.cpp
)
target_compile_features(js8core-engine-smoke-test PRIVATE cxx_std_20)
target_link_libraries(js8core-engine-smoke-test PRIVATE js8core)

add_executable(js8core-rig-test EXCLUDE_FROM_ALL tools/rig_integration_test.cpp)
target_link_libraries(js8core-rig-test PRIVATE js8core)
if(BUILD_TESTING)
  add_test(NAME js8core-rig COMMAND js8core-rig-test)
endif()

add_executable(js8core-callback-reentrancy-test EXCLUDE_FROM_ALL
  tools/callback_reentrancy_test.cpp)
target_link_libraries(js8core-callback-reentrancy-test PRIVATE js8core)
if(BUILD_TESTING)
  add_test(NAME js8core-callback-reentrancy COMMAND js8core-callback-reentrancy-test)
endif()

add_executable(js8core-tx-threaded-lifecycle-test EXCLUDE_FROM_ALL
  tools/tx_threaded_lifecycle_test.cpp)
target_link_libraries(js8core-tx-threaded-lifecycle-test PRIVATE js8core)
if(BUILD_TESTING)
  add_test(NAME js8core-tx-threaded-lifecycle COMMAND js8core-tx-threaded-lifecycle-test)
endif()

add_executable(js8core-session-lifecycle-test EXCLUDE_FROM_ALL
  tools/session_lifecycle_test.cpp)
target_link_libraries(js8core-session-lifecycle-test PRIVATE js8core)
if(BUILD_TESTING)
  add_test(NAME js8core-session-lifecycle COMMAND js8core-session-lifecycle-test)
endif()

add_executable(js8core-rig-generation-test EXCLUDE_FROM_ALL
  tools/rig_command_generation_test.cpp)
target_link_libraries(js8core-rig-generation-test PRIVATE js8core)
if(BUILD_TESTING)
  add_test(NAME js8core-rig-generation COMMAND js8core-rig-generation-test)
endif()

add_executable(js8core-host-callback-hardening-test EXCLUDE_FROM_ALL
  tools/host_callback_hardening_test.cpp)
target_link_libraries(js8core-host-callback-hardening-test PRIVATE js8core)
if(BUILD_TESTING)
  add_test(NAME js8core-host-callback-hardening COMMAND js8core-host-callback-hardening-test)
endif()

add_executable(js8core-compat-header-test EXCLUDE_FROM_ALL
  tools/compat_header_test.cpp)
target_link_libraries(js8core-compat-header-test PRIVATE js8core)
if(BUILD_TESTING)
  add_test(NAME js8core-compat-header COMMAND js8core-compat-header-test)
endif()

add_executable(js8core-android-compat-test EXCLUDE_FROM_ALL
  tools/android_compat_test.cpp)
target_link_libraries(js8core-android-compat-test PRIVATE js8core)
if(BUILD_TESTING)
  add_test(NAME js8core-android-compat COMMAND js8core-android-compat-test)
endif()

# Relocatable package for Android/iOS/desktop consumers.
install(TARGETS js8core EXPORT js8coreTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/js8core DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT js8coreTargets FILE js8coreTargets.cmake
  NAMESPACE js8core:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/js8core)
configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/js8coreConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/js8coreConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/js8core)
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/js8coreConfigVersion.cmake
  VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/js8coreConfig.cmake
              ${CMAKE_CURRENT_BINARY_DIR}/js8coreConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/js8core)
