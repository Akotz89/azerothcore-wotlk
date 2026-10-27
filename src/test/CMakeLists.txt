#
# This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
#
# This file is free software; as a special exception the author gives
# unlimited permission to copy and/or distribute it, with or without
# modifications, as long as this notice is preserved.
#
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY, to the extent permitted by law; without even the
# implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
#
CollectSourceFiles(
        ${CMAKE_CURRENT_SOURCE_DIR}
        PRIVATE_SOURCES
)

include_directories(
        "${CMAKE_CURRENT_SOURCE_DIR}/mocks"
)

add_executable(
        unit_tests
        ${PRIVATE_SOURCES}
)

target_link_libraries(
        unit_tests
        gtest_main
        gmock_main
        game-interface
)

# Add module test sources if any modules registered tests
get_property(MODULE_TEST_SOURCES GLOBAL PROPERTY ACORE_MODULE_TEST_SOURCES)
get_property(MODULE_TEST_INCLUDES GLOBAL PROPERTY ACORE_MODULE_TEST_INCLUDES)

if(MODULE_TEST_SOURCES)
    target_sources(unit_tests PRIVATE ${MODULE_TEST_SOURCES})
    message(STATUS "Added module tests to unit_tests target")
endif()

if(MODULE_TEST_INCLUDES)
    list(REMOVE_DUPLICATES MODULE_TEST_INCLUDES)
    target_include_directories(unit_tests PRIVATE ${MODULE_TEST_INCLUDES})
    message(STATUS "Added module test includes to unit_tests target")
endif()

# Link modules library to tests so module code is available
if(TARGET modules)
    # The worldserver and unit_tests share the production modules archive. Compile
    # only the bridge translation unit again for unit_tests so fixture registration
    # helpers never enter the production modules archive or worldserver.
    add_library(cpi211_test_population_bridge OBJECT
        "${CMAKE_SOURCE_DIR}/modules/mod-playerbots/src/Guide/PythonPopulationBridge.cpp")
    get_target_property(CPI211_MODULE_INCLUDE_DIRECTORIES modules INCLUDE_DIRECTORIES)
    get_target_property(CPI211_MODULE_COMPILE_DEFINITIONS modules COMPILE_DEFINITIONS)
    get_target_property(CPI211_MODULE_COMPILE_OPTIONS modules COMPILE_OPTIONS)
    get_target_property(CPI211_MODULE_COMPILE_FEATURES modules COMPILE_FEATURES)
    get_target_property(CPI211_MODULE_LINK_LIBRARIES modules LINK_LIBRARIES)
    if(CPI211_MODULE_INCLUDE_DIRECTORIES)
        target_include_directories(cpi211_test_population_bridge PRIVATE ${CPI211_MODULE_INCLUDE_DIRECTORIES})
    endif()
    if(CPI211_MODULE_COMPILE_DEFINITIONS)
        target_compile_definitions(cpi211_test_population_bridge PRIVATE ${CPI211_MODULE_COMPILE_DEFINITIONS})
    endif()
    if(CPI211_MODULE_COMPILE_OPTIONS)
        target_compile_options(cpi211_test_population_bridge PRIVATE ${CPI211_MODULE_COMPILE_OPTIONS})
    endif()
    if(CPI211_MODULE_COMPILE_FEATURES)
        target_compile_features(cpi211_test_population_bridge PRIVATE ${CPI211_MODULE_COMPILE_FEATURES})
    endif()
    if(CPI211_MODULE_LINK_LIBRARIES)
        target_link_libraries(cpi211_test_population_bridge PRIVATE ${CPI211_MODULE_LINK_LIBRARIES})
    endif()
    target_compile_definitions(cpi211_test_population_bridge PRIVATE CPI211_NATIVE_TEST_FIXTURE=1)
    target_sources(unit_tests PRIVATE $<TARGET_OBJECTS:cpi211_test_population_bridge>)
    target_link_libraries(unit_tests modules)
    target_link_libraries(unit_tests scripts)
    target_link_libraries(unit_tests game)
endif()

# Module-backed Core tests may include Boost.Python headers directly. The
# modules library keeps these dependencies private, so expose the same
# interface to the unit-test target without changing other Core consumers.
if(TARGET pyeng-interface)
    target_link_libraries(unit_tests pyeng-interface)
endif()

add_test(
        NAME
        unit
        COMMAND
        ${CMAKE_BINARY_DIR}/src/test/unit_tests
)
set(CPI211_NATIVE_TEST_ROOT "${CMAKE_BINARY_DIR}/cpi211-native-test" CACHE PATH
    "Writable root for CPI-211 native unit-test fixtures")
set(CPI211_TASK_ROOT "" CACHE PATH
    "Task workspace root used to find CPI-211 test-only package and consumer inputs")
set(CPI211_COMPILED_PACKAGE_DIR "${CMAKE_BINARY_DIR}/gear-consumer-package" CACHE PATH
    "Compiled strategic-director package used by CPI-211 native integration tests")
set_tests_properties(unit PROPERTIES
    ENVIRONMENT "CPI211_NATIVE_TEST_ROOT=${CPI211_NATIVE_TEST_ROOT};CPI211_TASK_ROOT=${CPI211_TASK_ROOT};CPI211_COMPILED_PACKAGE_DIR=${CPI211_COMPILED_PACKAGE_DIR}")
