cmake_minimum_required(VERSION 3.20)
project(FilesystemBtrfs VERSION 1.0.0 LANGUAGES C)
include(CTest)
include(FetchContent)
find_package(OpenSSL REQUIRED)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(INFILTRATR_COMMON_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(INFILTRATR_COMMON_BUILD_SHARED OFF CACHE BOOL "" FORCE)
FetchContent_Declare(infiltratr_common GIT_REPOSITORY https://github.com/Infiltrator-Projects/Libraries.git
 GIT_TAG c76ec95ef66fc728df6a906ad75a6a4dd63b419b)
FetchContent_MakeAvailable(infiltratr_common)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/version.h" "#define LD_VERSION \"1.0.0\"\n")
add_library(btrfs-driver SHARED native/btrfs_native.c native/btrfs_worker.c
 compat/legacy_services.c module/btrfs_module.c
 fixtures/btrfs_codec.c module/btrfs_fixture_bridge.c)
set_source_files_properties(native/btrfs_worker.c PROPERTIES
 COMPILE_DEFINITIONS "main=ifs_btrfs_worker_main")
set_source_files_properties(fixtures/btrfs_codec.c PROPERTIES
 COMPILE_DEFINITIONS "ldtm_raw_btrfs=ifs_btrfs_raw_fixture")
set_target_properties(btrfs-driver PROPERTIES PREFIX "" OUTPUT_NAME "BTRFS" SUFFIX ".so")
target_compile_definitions(btrfs-driver PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
target_compile_options(btrfs-driver PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_options(btrfs-driver PRIVATE -Wl,-z,defs)
target_include_directories(btrfs-driver PRIVATE
 "${CMAKE_CURRENT_SOURCE_DIR}/native" "${CMAKE_CURRENT_SOURCE_DIR}/compat"
 "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/include"
 "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(btrfs-driver PRIVATE InfiltratrCommon::Common OpenSSL::Crypto)
if(BUILD_TESTING)
 add_executable(btrfs-module-test tests/test_module.c)
 target_include_directories(btrfs-module-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
 target_link_libraries(btrfs-module-test PRIVATE btrfs-driver)
 add_test(NAME btrfs-module-interface COMMAND btrfs-module-test)
endif()
install(TARGETS btrfs-driver LIBRARY DESTINATION /System/Filesystems)
# Install only this filesystem's illustrated PDF beside its native shared object.
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/docs/Btrfs.pdf"
        DESTINATION /System/Resources/Documentation/Filesystem-Btrfs)
