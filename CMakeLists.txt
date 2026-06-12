# BTRFS2EXT4_ENABLE_SANITIZERS: enable AddressSanitizer + UBSan on all test
# targets. Defaults to ON for Debug builds and OFF for Release. Override with
# -DBTRFS2EXT4_ENABLE_SANITIZERS=ON|OFF.
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

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(BTRFS2EXT4_ENABLE_SANITIZERS_DEFAULT ON)
else()
    set(BTRFS2EXT4_ENABLE_SANITIZERS_DEFAULT OFF)
endif()
option(BTRFS2EXT4_ENABLE_SANITIZERS
       "Enable ASan+UBSan on test targets"
       ${BTRFS2EXT4_ENABLE_SANITIZERS_DEFAULT})

# Find required libraries
find_package(PkgConfig REQUIRED)
pkg_check_modules(UUID REQUIRED uuid)
pkg_check_modules(ZLIB REQUIRED zlib)

# Optional checksum libraries
pkg_check_modules(CRYPTO libcrypto)
if(CRYPTO_FOUND)
    include(CheckSymbolExists)
    set(CMAKE_REQUIRED_INCLUDES ${CRYPTO_INCLUDE_DIRS})
    set(CMAKE_REQUIRED_LIBRARIES ${CRYPTO_LIBRARIES})
    check_symbol_exists(EVP_sha256 "openssl/evp.h" HAVE_OPENSSL_SHA256)
    check_symbol_exists(EVP_blake2b256 "openssl/evp.h" HAVE_OPENSSL_BLAKE2B256)
    if(HAVE_OPENSSL_SHA256 OR HAVE_OPENSSL_BLAKE2B256)
        add_definitions(-DHAVE_LIBCRYPTO=1)
        message(STATUS "Found libcrypto: enabling OpenSSL checksum support")
    endif()
    if(HAVE_OPENSSL_BLAKE2B256)
        add_definitions(-DHAVE_OPENSSL_BLAKE2B256=1)
        message(STATUS "Found EVP_blake2b256: enabling BLAKE2b support")
    endif()
endif()

pkg_check_modules(XXHASH libxxhash)
if(XXHASH_FOUND)
    add_definitions(-DHAVE_LIBXXHASH=1)
    message(STATUS "Found libxxhash: enabling XXHASH64 support")
endif()

# Optional compression libraries
pkg_check_modules(LZO lzo2)
pkg_check_modules(ZSTD libzstd)

# Optional io_uring for async batch I/O
pkg_check_modules(URING liburing)
if(URING_FOUND)
    add_definitions(-DHAVE_IO_URING=1)
    message(STATUS "Found liburing: enabling io_uring batch I/O")
endif()

find_package(Threads REQUIRED)

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

if(URING_FOUND)
    include_directories(${URING_INCLUDE_DIRS})
endif()

# Source files
set(BTRFS2EXT4_SOURCES
    src/main.c
    src/device_io.c
    src/btrfs/checksum.c
    src/btrfs/superblock.c
    src/btrfs/chunk_tree.c
    src/btrfs/tree_walk.c
    src/btrfs/btree.c
    src/btrfs/fs_tree.c
    src/btrfs/decompress.c
    src/ext4/planner.c
    src/ext4/superblock_writer.c
    src/ext4/gdt_writer.c
    src/ext4/bitmap_writer.c
    src/ext4/metadata_csum.c
    src/ext4/inode_checksum.c
    src/ext4/inode_writer.c
    src/ext4/dir_writer.c
    src/ext4/extent_writer.c
    src/ext4/journal_writer.c
    src/ext4/ext4_crc16.c
    src/relocator.c
    src/bloom.c
    src/mem_tracker.c
    src/migration_map.c
    src/thread_pool.c
)

# Main executable
add_executable(btrfs2ext4 ${BTRFS2EXT4_SOURCES})
target_compile_definitions(btrfs2ext4 PRIVATE BTRFS2EXT4_NO_RELOC_JOURNAL=1)

target_link_libraries(btrfs2ext4
    ${UUID_LIBRARIES}
    ${ZLIB_LIBRARIES}
    Threads::Threads
    m
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

if(URING_FOUND)
    target_link_libraries(btrfs2ext4 ${URING_LIBRARIES})
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
    src/btrfs/tree_walk.c
    src/btrfs/btree.c
    src/btrfs/fs_tree.c
    src/btrfs/decompress.c
    src/ext4/planner.c
    src/ext4/superblock_writer.c
    src/ext4/gdt_writer.c
    src/ext4/bitmap_writer.c
    src/ext4/metadata_csum.c
    src/ext4/inode_checksum.c
    src/ext4/inode_writer.c
    src/ext4/dir_writer.c
    src/ext4/extent_writer.c
    src/ext4/journal_writer.c
    src/ext4/ext4_crc16.c
    src/relocator.c
    src/bloom.c
    src/mem_tracker.c
    src/migration_map.c
    src/thread_pool.c
)

# Stress / vulnerability / performance test suite
add_executable(test_stress tests/test_stress.c ${BTRFS2EXT4_LIB_SOURCES})
target_compile_definitions(test_stress PRIVATE BTRFS_TESTING=1)
target_link_libraries(test_stress
    ${UUID_LIBRARIES}
    ${ZLIB_LIBRARIES}
    Threads::Threads
)

add_executable(test_fuzz tests/test_fuzz.c ${BTRFS2EXT4_LIB_SOURCES})
target_compile_definitions(test_fuzz PRIVATE BTRFS_TESTING=1)
target_link_libraries(test_fuzz
    ${UUID_LIBRARIES}
    ${ZLIB_LIBRARIES}
    Threads::Threads
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
if(URING_FOUND)
    target_link_libraries(test_stress ${URING_LIBRARIES})
    target_link_libraries(test_fuzz ${URING_LIBRARIES})
endif()

# Checksum test
add_executable(test_checksum tests/test_checksum.c ${BTRFS2EXT4_LIB_SOURCES})
target_link_libraries(test_checksum
    ${UUID_LIBRARIES}
    ${ZLIB_LIBRARIES}
    Threads::Threads
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
if(URING_FOUND)
    target_link_libraries(test_checksum ${URING_LIBRARIES})
endif()

# --- Integration Test ---
add_executable(test_integration tests/test_integration.c ${BTRFS2EXT4_LIB_SOURCES})
target_compile_definitions(test_integration PRIVATE
    BTRFS_TESTING=1 BTRFS2EXT4_NO_RELOC_JOURNAL=1)
target_link_libraries(test_integration
    ${UUID_LIBRARIES}
    ${ZLIB_LIBRARIES}
    Threads::Threads
)
if(CRYPTO_FOUND)
    target_link_libraries(test_integration ${CRYPTO_LIBRARIES})
endif()
if(XXHASH_FOUND)
    target_link_libraries(test_integration ${XXHASH_LIBRARIES})
endif()
if(LZO_FOUND)
    target_link_libraries(test_integration ${LZO_LIBRARIES})
endif()
if(ZSTD_FOUND)
    target_link_libraries(test_integration ${ZSTD_LIBRARIES})
endif()
if(URING_FOUND)
    target_link_libraries(test_integration ${URING_LIBRARIES})
endif()


add_test(NAME stress_test COMMAND test_stress)
add_test(NAME fuzz_test COMMAND test_fuzz)
add_test(NAME checksum_test COMMAND test_checksum)
add_test(NAME integration_test COMMAND test_integration)


function(btrfs2ext4_enable_sanitizers target)
    if(NOT BTRFS2EXT4_ENABLE_SANITIZERS)
        return()
    endif()
    include(CheckCCompilerFlag)
    if(NOT DEFINED HAS_BTRFS2EXT4_SANITIZERS)
        set(CMAKE_REQUIRED_LINK_OPTIONS -fsanitize=address,undefined)
        check_c_compiler_flag(-fsanitize=address,undefined HAS_BTRFS2EXT4_SANITIZERS)
        unset(CMAKE_REQUIRED_LINK_OPTIONS)
        if(HAS_BTRFS2EXT4_SANITIZERS)
            message(STATUS "Enabling ASan+UBSan on test targets")
        endif()
    endif()
    if(HAS_BTRFS2EXT4_SANITIZERS)
        target_compile_options(${target} PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
endfunction()

btrfs2ext4_enable_sanitizers(test_stress)
btrfs2ext4_enable_sanitizers(test_fuzz)
btrfs2ext4_enable_sanitizers(test_checksum)
btrfs2ext4_enable_sanitizers(test_integration)
