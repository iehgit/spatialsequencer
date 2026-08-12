cmake_minimum_required(VERSION 3.20)
project(spatial_midi_graph_sequencer LANGUAGES CXX)

include(GNUInstallDirs)

option(SPATIAL_MIDI_BUILD_APP "Build the SDL2/ALSA desktop application" ON)
option(SPATIAL_MIDI_BUILD_TESTS "Build core unit tests" ON)
option(SPATIAL_MIDI_ENABLE_DEB_PACKAGING "Enable Debian package generation" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

add_library(spatial_midi_core
    src/core/graph.cpp
    src/core/midi_note_input_worker.cpp
    src/core/sequencer_engine.cpp
    src/core/transport_worker.cpp
)
target_include_directories(spatial_midi_core PUBLIC include)
target_link_libraries(spatial_midi_core PUBLIC Threads::Threads)
target_compile_options(spatial_midi_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)

if(SPATIAL_MIDI_BUILD_APP)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2>=2.0.18)
    pkg_check_modules(SDL2_TTF REQUIRED IMPORTED_TARGET SDL2_ttf)
    pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa)

    add_library(spatial_midi_alsa src/midi/alsa_sequencer.cpp)
    target_include_directories(spatial_midi_alsa PUBLIC include)
    target_link_libraries(spatial_midi_alsa PUBLIC spatial_midi_core PkgConfig::ALSA)
    target_compile_options(spatial_midi_alsa PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)

    add_library(spatial_midi_ui
        src/ui/sdl_app.cpp
        src/ui/sdl_app_view.cpp
        src/ui/text_cache.cpp
    )
    target_include_directories(spatial_midi_ui PUBLIC include)
    target_link_libraries(spatial_midi_ui PUBLIC spatial_midi_core PkgConfig::SDL2 PkgConfig::SDL2_TTF)
    target_compile_options(spatial_midi_ui PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)

    add_executable(spatial-midi src/main.cpp)
    target_link_libraries(spatial-midi PRIVATE spatial_midi_core spatial_midi_alsa spatial_midi_ui)
    target_compile_options(spatial-midi PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)

    set(SPATIAL_MIDI_INSTALL_DOCDIR
        "${CMAKE_INSTALL_DATAROOTDIR}/doc/spatial-midi")
    install(TARGETS spatial-midi
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    )
    install(FILES
        README.md
        MIDI.md
        screenshot.png
        DESTINATION "${SPATIAL_MIDI_INSTALL_DOCDIR}"
    )
    install(FILES LICENSE
        DESTINATION "${SPATIAL_MIDI_INSTALL_DOCDIR}"
        RENAME copyright
    )
    install(FILES examples/graph.json
        DESTINATION "${SPATIAL_MIDI_INSTALL_DOCDIR}/examples"
    )
endif()

if(SPATIAL_MIDI_BUILD_TESTS)
    enable_testing()
    add_executable(spatial-midi-tests tests/core_tests.cpp)
    target_link_libraries(spatial-midi-tests PRIVATE spatial_midi_core)
    target_compile_options(spatial-midi-tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    add_test(NAME spatial-midi-core-tests COMMAND spatial-midi-tests)
endif()

if(SPATIAL_MIDI_ENABLE_DEB_PACKAGING)
    if(NOT SPATIAL_MIDI_BUILD_APP)
        message(FATAL_ERROR
            "Debian packaging requires SPATIAL_MIDI_BUILD_APP=ON")
    endif()
    if(NOT DEFINED SPATIAL_MIDI_PACKAGE_VERSION
       OR NOT SPATIAL_MIDI_PACKAGE_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
        message(FATAL_ERROR
            "SPATIAL_MIDI_PACKAGE_VERSION must have the form MAJOR.MINOR.PATCH")
    endif()
    if(NOT DEFINED CPACK_DEBIAN_PACKAGE_MAINTAINER
       OR CPACK_DEBIAN_PACKAGE_MAINTAINER STREQUAL "")
        message(FATAL_ERROR
            "CPACK_DEBIAN_PACKAGE_MAINTAINER must be set for Debian packaging")
    endif()

    set(CPACK_GENERATOR "DEB")
    set(CPACK_PACKAGE_NAME "spatial-midi")
    set(CPACK_PACKAGE_VENDOR "Spatial MIDI Graph Sequencer")
    set(CPACK_PACKAGE_CONTACT "${CPACK_DEBIAN_PACKAGE_MAINTAINER}")
    set(CPACK_PACKAGE_VERSION "${SPATIAL_MIDI_PACKAGE_VERSION}")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
        "Graph-based MIDI sequencer for Linux")
    set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
    set(CPACK_PACKAGE_RELOCATABLE OFF)
    set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")

    set(CPACK_DEBIAN_PACKAGE_NAME "spatial-midi")
    set(CPACK_DEBIAN_PACKAGE_VERSION "${SPATIAL_MIDI_PACKAGE_VERSION}")
    set(CPACK_DEBIAN_PACKAGE_RELEASE "1")
    set(CPACK_DEBIAN_PACKAGE_SECTION "sound")
    set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
    set(CPACK_DEBIAN_PACKAGE_DESCRIPTION
        "Spatial MIDI Graph Sequencer represents musical structure as a graph on a two-dimensional grid. It sends MIDI through ALSA and provides an SDL2 editor.")
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    set(CPACK_DEBIAN_PACKAGE_CONTROL_STRICT_PERMISSION ON)
    set(CPACK_DEBIAN_COMPRESSION_TYPE "xz")
    set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")

    include(CPack)
endif()
