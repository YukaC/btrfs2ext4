cmake_minimum_required(VERSION 3.16)
project(btrfs2ext4 C)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Compiler warnings
add_compile_options(-Wall -Wextra -Wpedantic -Werror=implicit-function-declaration)

# Debug build by default
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# Find required libraries
find_package(PkgConfig REQUIRED)
pkg_check_modules(UUID REQUIRED uuid)
pkg_check_modules(ZLIB REQUIRED zlib)

# Optional checksum libraries
pkg_check_modules(CRYPTO libcrypto)
if(CRYPTO_FOUND)
    add_definitions(-DHAVE_LIBCRYPTO=1)
    message(STATUS "Found libcrypto: enabling SHA256 and BLAKE2b support")
endif()

pkg_check_modules(XXHASH libxxhash)
if(XXHASH_FOUND)
    add_definitions(-DHAVE_LIBXXHASH=1)
    message(STATUS "Found libxxhash: enabling XXHASH64 support")
endif()

# Optional compression libraries
pkg_check_modules(LZO lzo2)
pkg_check_modules(ZSTD libzstd)

# Include directories
include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${UUID_INCLUDE_DIRS}
    ${ZLIB_INCLUDE_DIRS}
)

if(CRYPTO_FOUND)
    include_directories(${CRYPTO_INCLUDE_DIRS})
endif()

if(XXHASH_FOUND)
    include_directories(${XXHASH_INCLUDE_DIRS})
endif()

if(LZO_FOUND)
    include_directories(${LZO_INCLUDE_DIRS})
    add_definitions(-DHAVE_LZO=1)
endif()

if(ZSTD_FOUND)
    include_directories(${ZSTD_INCLUDE_DIRS})
    add_definitions(-DHAVE_ZSTD=1)
endif()

# Source files
set(BTRFS2EXT4_SOURCES
    src/main.c
    src/device_io.c
    src/btrfs/checksum.c
    src/btrfs/superblock.c
    src/btrfs/chunk_tree.c
    src/btrfs/btree.c
    src/btrfs/fs_tree.c
    src/btrfs/decompress.c
    src/ext4/planner.c
    src/ext4/superblock_writer.c
    src/ext4/gdt_writer.c
    src/ext4/bitmap_writer.c
    src/ext4/inode_writer.c
    src/ext4/dir_writer.c
    src/ext4/extent_writer.c
    src/ext4/journal_writer.c
    src/relocator.c
    src/bloom.c
    src/journal.c
    src/mem_tracker.c
    src/migration_map.c
)

# Main executable
add_executable(btrfs2ext4 ${BTRFS2EXT4_SOURCES})

target_link_libraries(btrfs2ext4
    ${UUID_LIBRARIES}
    ${ZLIB_LIBRARIES}
)

if(CRYPTO_FOUND)
    target_link_libraries(btrfs2ext4 ${CRYPTO_LIBRARIES})
endif()

if(XXHASH_FOUND)
    target_link_libraries(btrfs2ext4 ${XXHASH_LIBRARIES})
endif()

if(LZO_FOUND)
    target_link_libraries(btrfs2ext4 ${LZO_LIBRARIES})
endif()

if(ZSTD_FOUND)
    target_link_libraries(btrfs2ext4 ${ZSTD_LIBRARIES})
endif()

# Install target
install(TARGETS btrfs2ext4 RUNTIME DESTINATION sbin)
install(FILES btrfs2ext4.8 DESTINATION share/man/man8)

# --- Tests ---
enable_testing()

# Shared source files (everything except main.c)
set(BTRFS2EXT4_LIB_SOURCES
    src/device_io.c
    src/btrfs/checksum.c
    src/btrfs/superblock.c
    src/btrfs/chunk_tree.c
    src/btrfs/btree.c
    src/btrfs/fs_tree.c
    src/btrfs/decompress.c
    src/ext4/planner.c
    src/ext4/superblock_writer.c
    src/ext4/gdt_writer.c
    src/ext4/bitmap_writer.c
    src/ext4/inode_writer.c
    src/ext4/dir_writer.c
    src/ext4/extent_writer.c
    src/ext4/journal_writer.c
    src/relocator.c
    src/bloom.c
    src/journal.c
    src/mem_tracker.c
    src/migration_map.c
)

# Stress / vulnerability / performance test suite
add_executable(test_stress tests/test_stress.c ${BTRFS2EXT4_LIB_SOURCES})
target_link_libraries(test_stress
    ${UUID_LIBRARIES}
    ${ZLIB_LIBRARIES}
)

add_executable(test_fuzz tests/test_fuzz.c ${BTRFS2EXT4_LIB_SOURCES})
target_link_libraries(test_fuzz
    ${UUID_LIBRARIES}
    ${ZLIB_LIBRARIES}
)

if(CRYPTO_FOUND)
    target_link_libraries(test_stress ${CRYPTO_LIBRARIES})
    target_link_libraries(test_fuzz ${CRYPTO_LIBRARIES})
endif()
if(XXHASH_FOUND)
    target_link_libraries(test_stress ${XXHASH_LIBRARIES})
    target_link_libraries(test_fuzz ${XXHASH_LIBRARIES})
endif()
if(LZO_FOUND)
    target_link_libraries(test_stress ${LZO_LIBRARIES})
    target_link_libraries(test_fuzz ${LZO_LIBRARIES})
endif()
if(ZSTD_FOUND)
    target_link_libraries(test_stress ${ZSTD_LIBRARIES})
    target_link_libraries(test_fuzz ${ZSTD_LIBRARIES})
endif()

add_executable(test_checksum tests/test_checksum.c ${BTRFS2EXT4_LIB_SOURCES})
target_link_libraries(test_checksum
    ${UUID_LIBRARIES}
    ${ZLIB_LIBRARIES}
)
if(CRYPTO_FOUND)
    target_link_libraries(test_checksum ${CRYPTO_LIBRARIES})
endif()
if(XXHASH_FOUND)
    target_link_libraries(test_checksum ${XXHASH_LIBRARIES})
endif()
if(LZO_FOUND)
    target_link_libraries(test_checksum ${LZO_LIBRARIES})
endif()
if(ZSTD_FOUND)
    target_link_libraries(test_checksum ${ZSTD_LIBRARIES})
endif()

add_test(NAME stress_test COMMAND test_stress)
add_test(NAME fuzz_test COMMAND test_fuzz)
add_test(NAME checksum_test COMMAND test_checksum)

# Enable AddressSanitizer in debug builds for memory bug detection (if available)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    include(CheckCCompilerFlag)
    check_c_compiler_flag(-fsanitize=address HAS_ASAN)
    if(HAS_ASAN)
        target_compile_options(test_stress PRIVATE -fsanitize=address -fno-omit-frame-pointer)
        target_link_options(test_stress PRIVATE -fsanitize=address)
    endif()
endif()
