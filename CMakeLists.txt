cmake_minimum_required(VERSION 3.16)
project(dsf_mmu LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MMU_BUILD_TESTS "Build unit tests" ON)
option(MMU_BUILD_EXAMPLES "Build examples" ON)

add_library(dsf_mmu
    src/mmu_api.cpp
    src/core/block_directory.cpp
    src/core/block_manager.cpp
    src/core/page_table.cpp
    src/core/context.cpp
)

target_include_directories(dsf_mmu
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_options(dsf_mmu PRIVATE -Wall -Wextra -Wpedantic)

if(MMU_BUILD_EXAMPLES)
    add_executable(mmu_example_llm_kv
        examples/llm_kv_prefill_decode.cpp
    )
    target_link_libraries(mmu_example_llm_kv PRIVATE dsf_mmu)
    target_compile_options(mmu_example_llm_kv PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(MMU_BUILD_TESTS)
    enable_testing()
    add_subdirectory(3rdparty/googletest EXCLUDE_FROM_ALL)

    add_executable(mmu_tests
        tests/test_allocator_basic.cpp
        tests/test_api_contract.cpp
        tests/test_fork_shared_block.cpp
        tests/test_mapped_pointer.cpp
        tests/test_p0_metadata_skeleton.cpp
        tests/test_parallel_global_lock.cpp
        tests/test_page_table_mapping.cpp
        tests/test_out_of_bounds.cpp
        tests/test_cascade_fork_and_free.cpp
        tests/test_cow_trigger.cpp
        tests/test_sys_fork_zero_copy.cpp
        
    )

    target_include_directories(mmu_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(mmu_tests PRIVATE dsf_mmu gtest_main)

    include(GoogleTest)
    gtest_discover_tests(mmu_tests)
endif()
