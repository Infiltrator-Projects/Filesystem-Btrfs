# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" FILESYSTEM_VERSION LIMIT_COUNT 1)
project(FilesystemBtrfs VERSION ${FILESYSTEM_VERSION} LANGUAGES C)
include(CTest)
include(FetchContent)
find_package(OpenSSL REQUIRED)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(INFILTRATR_COMMON_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(INFILTRATR_COMMON_BUILD_SHARED OFF CACHE BOOL "" FORCE)
FetchContent_Declare(infiltratr_common
  GIT_REPOSITORY https://github.com/Infiltrator-Projects/Libraries.git
  GIT_TAG d23a52693fd3212368d327e1c1302e07ae9510ec)
FetchContent_MakeAvailable(infiltratr_common)
set_property(DIRECTORY "${infiltratr_common_SOURCE_DIR}" PROPERTY EXCLUDE_FROM_ALL TRUE)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/version.h"
  "#define LD_VERSION \"${PROJECT_VERSION}\"\n")

# Compile the same bounded on-disk reader in both execution profiles. The
# child build excludes hosted linker, instrumentation and startup objects.
set(BTRFS_FIRMWARE_ELF "${CMAKE_CURRENT_BINARY_DIR}/firmware/BTRFS.so")
add_custom_command(OUTPUT "${BTRFS_FIRMWARE_ELF}"
  COMMAND "${CMAKE_COMMAND}" -S "${CMAKE_CURRENT_SOURCE_DIR}/module/firmware"
    -B "${CMAKE_CURRENT_BINARY_DIR}/firmware" -DCMAKE_BUILD_TYPE=Release
    "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}" "-DCOMMON_INCLUDE_DIR=${infiltratr_common_SOURCE_DIR}/include"
  COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_CURRENT_BINARY_DIR}/firmware"
  DEPENDS module/firmware/CMakeLists.txt module/firmware/runtime.c module/btrfs_v2_probe.c
    native/btrfs_reader.c native/btrfs_reader.h native/btrfs_checksum.c
    "${infiltratr_common_SOURCE_DIR}/include/infiltratr/filesystem_module_v2.h"
  VERBATIM)
add_custom_target(btrfs-firmware DEPENDS "${BTRFS_FIRMWARE_ELF}")
add_library(btrfs-driver SHARED native/btrfs_reader.c native/btrfs_checksum.c module/btrfs_v2_probe.c)
add_dependencies(btrfs-driver btrfs-firmware)
set_property(TARGET btrfs-driver APPEND PROPERTY LINK_DEPENDS "${BTRFS_FIRMWARE_ELF}")
add_custom_command(TARGET btrfs-driver POST_BUILD
  COMMAND "${CMAKE_OBJCOPY}" --add-section ".infiltrator_boot=${BTRFS_FIRMWARE_ELF}"
    --set-section-flags .infiltrator_boot=readonly "$<TARGET_FILE:btrfs-driver>" VERBATIM)
set_target_properties(btrfs-driver PROPERTIES
  PREFIX "" OUTPUT_NAME "BTRFS" SUFFIX ".so"
  C_VISIBILITY_PRESET hidden)
target_compile_definitions(btrfs-driver PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
target_compile_options(btrfs-driver PRIVATE
  -Wall -Wextra -Wpedantic -Werror -ffunction-sections -fdata-sections)
target_link_options(btrfs-driver PRIVATE
  -Wl,-z,defs -Wl,--gc-sections
  "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/module/v2-only.map")
target_include_directories(btrfs-driver PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/native"
  "${CMAKE_CURRENT_SOURCE_DIR}/compat"
  "${infiltratr_common_SOURCE_DIR}/include")
target_link_libraries(btrfs-driver PRIVATE InfiltratrCommon::Common OpenSSL::Crypto)

if(BUILD_TESTING)
  add_library(btrfs-legacy-test-support SHARED
    native/btrfs_native.c native/btrfs_checksum.c native/btrfs_worker.c
    compat/legacy_services.c module/btrfs_module.c
    fixtures/btrfs_codec.c module/btrfs_fixture_bridge.c)
  set_source_files_properties(native/btrfs_worker.c PROPERTIES
    COMPILE_DEFINITIONS "main=ifs_btrfs_worker_main")
  set_source_files_properties(fixtures/btrfs_codec.c PROPERTIES
    COMPILE_DEFINITIONS "ldtm_raw_btrfs=ifs_btrfs_raw_fixture")
  target_compile_definitions(btrfs-legacy-test-support PRIVATE
    _GNU_SOURCE _FILE_OFFSET_BITS=64)
  target_compile_options(btrfs-legacy-test-support PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
  target_include_directories(btrfs-legacy-test-support PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/native"
    "${CMAKE_CURRENT_SOURCE_DIR}/compat"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/include"
    "${CMAKE_CURRENT_BINARY_DIR}"
    "${infiltratr_common_SOURCE_DIR}/include")
  target_link_libraries(btrfs-legacy-test-support PRIVATE
    InfiltratrCommon::Common OpenSSL::Crypto)

  add_executable(btrfs-readonly-media tests/test_readonly_media.c)
  target_include_directories(btrfs-readonly-media PRIVATE "${infiltratr_common_SOURCE_DIR}/include")
  target_compile_options(btrfs-readonly-media PRIVATE -Wall -Wextra -Wpedantic -Werror)
  target_link_libraries(btrfs-readonly-media PRIVATE ${CMAKE_DL_LIBS})
  add_test(NAME btrfs-carrier-and-firmware-real-media
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/readonly_media.sh"
      "$<TARGET_FILE:btrfs-readonly-media>" "$<TARGET_FILE:btrfs-driver>" "${BTRFS_FIRMWARE_ELF}")
  add_executable(btrfs-module-test tests/test_module.c)
  target_include_directories(btrfs-module-test PRIVATE
    "${infiltratr_common_SOURCE_DIR}/include")
  target_link_libraries(btrfs-module-test PRIVATE
    btrfs-driver InfiltratrCommon::Common)
  add_test(NAME btrfs-module-interface COMMAND btrfs-module-test)

  add_executable(btrfs-v2-probe-test tests/test_btrfs_v2_probe.c)
  target_include_directories(btrfs-v2-probe-test PRIVATE
    "${infiltratr_common_SOURCE_DIR}/include")
  target_link_libraries(btrfs-v2-probe-test PRIVATE
    btrfs-driver InfiltratrCommon::Common)
  target_compile_options(btrfs-v2-probe-test PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME btrfs-common-v2-probe COMMAND btrfs-v2-probe-test)

  add_test(NAME btrfs-v2-only-dynamic-export
    COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_v2_exports.sh"
      "$<TARGET_FILE:btrfs-driver>")
  # Only the production V2 descriptor, in-memory probe and export check
  # run during native installation; CI retains all real-media regressions.
  set_tests_properties(btrfs-module-interface btrfs-common-v2-probe
    btrfs-v2-only-dynamic-export PROPERTIES LABELS "install-smoke" TIMEOUT 30)
  add_executable(btrfs-native-corruption-test
    tests/test_btrfs_native_corruption.c)
  target_include_directories(btrfs-native-corruption-test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/native")
  target_compile_definitions(btrfs-native-corruption-test PRIVATE
    _POSIX_C_SOURCE=200809L)
  target_compile_options(btrfs-native-corruption-test PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
  target_link_libraries(btrfs-native-corruption-test PRIVATE
    btrfs-legacy-test-support)
  add_test(NAME btrfs-damaged-media-fail-closed
    COMMAND btrfs-native-corruption-test)
  # Test the very same native directory decoder compiled into both V2
  # providers. The real-image Common V2 tests above remain mandatory.
  add_executable(btrfs-duplicate-directory-test
    tests/test_btrfs_duplicate_directory.c
    native/btrfs_checksum.c)
  target_include_directories(btrfs-duplicate-directory-test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/native"
    "${infiltratr_common_SOURCE_DIR}/include")
  target_compile_options(btrfs-duplicate-directory-test PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME btrfs-v2-duplicate-directory-corruption
    COMMAND btrfs-duplicate-directory-test)
endif()
install(TARGETS btrfs-driver LIBRARY DESTINATION /System/Filesystems)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/docs/Btrfs.pdf"
  DESTINATION /System/Resources/Documentation
  RENAME Filesystem-Btrfs.pdf)
