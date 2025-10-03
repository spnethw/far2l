project(OpenWith)

set(SOURCES
    src/OpenWith.cpp
    src/AppProvider.cpp
    src/DummyAppProvider.cpp
    src/XDGBasedAppProvider.cpp
)

option(OPENWITH_USE_GIO "Build with GLib/GIO support for application discovery" ON)

if(OPENWITH_USE_GIO)
    find_package(PkgConfig)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(GLIB_GIO glib-2.0 gio-2.0 gio-unix-2.0)
    endif()

    if(GLIB_GIO_FOUND)
        message(STATUS "GLib/GIO found, building with GIO support.")
        list(APPEND SOURCES src/GIOBasedAppProvider.cpp)
        add_definitions(-DENABLE_GIO_SUPPORT)
    else()
        message(WARNING "GLib/GIO not found. Plugin will be built with fallback XDG implementation only.")
    endif()
endif()

if(APPLE)
    list(APPEND SOURCES src/MacOSAppProvider.mm)
endif()

add_library(${PROJECT_NAME} MODULE ${SOURCES})

target_link_libraries(${PROJECT_NAME} PRIVATE
    utils
    far2l
)

if(GLIB_GIO_FOUND)
    target_include_directories(${PROJECT_NAME} PRIVATE ${GLIB_GIO_INCLUDE_DIRS})
    target_link_libraries(${PROJECT_NAME} PRIVATE ${GLIB_GIO_LIBRARIES})
    if(GLIB_GIO_CFLAGS_OTHER)
        target_compile_options(${PROJECT_NAME} PRIVATE ${GLIB_GIO_CFLAGS_OTHER})
    endif()
endif()

# Frameworks for macOS
if(APPLE)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        "-framework Cocoa"
        "-framework CoreServices"
        "-framework UniformTypeIdentifiers"
    )
    target_compile_options(${PROJECT_NAME} PRIVATE "-fobjc-arc")
endif()

target_compile_definitions(OpenWith PRIVATE)

target_include_directories(${PROJECT_NAME} PRIVATE .)
target_include_directories(${PROJECT_NAME} PRIVATE ../far2l/far2sdk)
target_include_directories(${PROJECT_NAME} PRIVATE ../WinPort)
target_include_directories(${PROJECT_NAME} PRIVATE ../utils/include)

set_target_properties(${PROJECT_NAME} PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${INSTALL_DIR}/Plugins/${PROJECT_NAME}/plug"
    PREFIX ""
    SUFFIX ".far-plug-wide"
)

set(CURRENT_TARGET "OpenWith")

file(GLOB_RECURSE RESOURCE_FILES
    RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}/configs
    "${CMAKE_CURRENT_SOURCE_DIR}/configs/*"
)

setup_target_auxiliary_files(${CURRENT_TARGET}
    RESOURCE_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/configs
    "${INSTALL_DIR}/Plugins/${CURRENT_TARGET}/plug"
)

add_dependencies(${CURRENT_TARGET} copy_aux_files_for_${CURRENT_TARGET})
