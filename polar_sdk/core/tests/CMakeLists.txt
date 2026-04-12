cmake_minimum_required(VERSION 3.16)

project(polar_sdk_core_tests C)
set(CMAKE_C_STANDARD 11)

enable_testing()

add_executable(test_pmd_start
    test_pmd_start.c
    ../src/polar_sdk_pmd.c
    ../src/polar_sdk_pmd_start.c
)
target_include_directories(test_pmd_start PRIVATE
    ../include
)
add_test(NAME test_pmd_start COMMAND test_pmd_start)

add_executable(test_btstack_security
    test_btstack_security.c
    ../src/polar_sdk_btstack_security.c
    ../src/polar_sdk_security.c
)
target_include_directories(test_btstack_security PRIVATE
    ../include
    ${CMAKE_CURRENT_LIST_DIR}/fakes
)
add_test(NAME test_btstack_security COMMAND test_btstack_security)