cmake_minimum_required(VERSION 3.20)
project(js8core VERSION 0.1.0 LANGUAGES CXX)

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
  src/placeholder.cpp
  src/engine/engine.cpp
  src/decoder/legacy_decoder.cpp
  src/decoder/ldpc.cpp
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

if(BUILD_TESTING)
  add_test(NAME js8core-fractional-rx COMMAND js8core-fractional-rx-test)
  add_test(NAME js8core-crc COMMAND js8core-crc-test)
  add_test(NAME js8core-varicode COMMAND js8core-varicode-test)
  add_test(NAME js8core-policy COMMAND js8core-policy-test)
  add_test(NAME js8core-jsc COMMAND verify_jsc_roundtrip)
  add_test(NAME js8core-vox COMMAND js8core-vox-test)
  add_test(NAME js8core-clock-math COMMAND js8core-clock-math-test)
  add_test(NAME js8core-decoder-roundtrip COMMAND js8core-decoder-roundtrip-test)
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
