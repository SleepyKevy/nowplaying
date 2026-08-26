cmake_minimum_required(VERSION 3.28...3.30)

project(obs-now-playing VERSION 1.0.0 LANGUAGES CXX)

option(NOWPLAYING_SCAFFOLD_ONLY "Validate the project without OBS dependencies" OFF)

if(NOWPLAYING_SCAFFOLD_ONLY)
    enable_testing()
    add_executable(now-playing-state-test tests/track-state-test.cpp)
    target_include_directories(now-playing-state-test PRIVATE src)
    target_compile_features(now-playing-state-test PRIVATE cxx_std_20)
    add_test(NAME now-playing-state-test COMMAND now-playing-state-test)
    message(STATUS "obs-now-playing scaffold validation complete")
    return()
endif()

# Windows dependency bootstrap from the build-tools package. This downloads
# the matching OBS and Qt development dependencies on the GitHub runner.
if(WIN32)
    if(NOT CMAKE_GENERATOR STREQUAL "Visual Studio 17 2022")
        message(FATAL_ERROR
            "The Windows preset expects the Visual Studio 17 2022 generator. "
            "Use the supplied windows-x64 preset.")
    endif()

    list(APPEND CMAKE_MODULE_PATH
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/common"
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/windows"
    )
    set(OS_WINDOWS TRUE)
    include(buildspec)
endif()

add_library(${PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE OBS::libobs)

find_package(obs-frontend-api REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE OBS::obs-frontend-api)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)
target_link_libraries(${PROJECT_NAME} PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets)

# C++/WinRT headers come from the Windows SDK and windowsapp.lib provides the
# Windows Runtime import library used by Global System Media Transport Controls.
if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE windowsapp)
endif()

target_sources(
    ${PROJECT_NAME}
    PRIVATE
        src/plugin-main.cpp
        src/core/playback-state-store.hpp
        src/core/track-state.hpp
        src/services/playback-provider.hpp
        src/windows/windows-media-playback-provider.cpp
        src/windows/windows-media-playback-provider.hpp
        src/obs/now-playing-source.cpp
        src/obs/now-playing-source.hpp
        src/render/card-renderer.cpp
        src/render/card-renderer.hpp
        src/render/render-config.hpp
)

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_20)
target_compile_definitions(
    ${PROJECT_NAME}
    PRIVATE
        NOWPLAYING_PLUGIN_VERSION="${PROJECT_VERSION}"
        WIN32_LEAN_AND_MEAN
        NOMINMAX
)

if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wpedantic)
endif()

set_target_properties(${PROJECT_NAME} PROPERTIES PREFIX "" OUTPUT_NAME "obs-now-playing")

if(COMMAND set_target_properties_plugin)
    set_target_properties_plugin(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "obs-now-playing")
else()
    install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION "obs-plugins/64bit" LIBRARY DESTINATION "obs-plugins/64bit")
    install(DIRECTORY data/ DESTINATION "data/obs-plugins/obs-now-playing")
endif()

set(CPACK_PACKAGE_NAME "OBS-Now-Playing")
set(CPACK_PACKAGE_VENDOR "SleepyKev")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_FILE_NAME "OBS-Now-Playing-${PROJECT_VERSION}-Windows-x64")
set(CPACK_GENERATOR "ZIP")
include(CPack)
