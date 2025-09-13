# OpenWith.cpp - CMakeLists.txt
project(OpenWith)

set(SOURCES
src/OpenWith.cpp
src/AppProvider.cpp
src/BSDAppProvider.cpp
src/DummyAppProvider.cpp
src/LinuxAppProvider.cpp
src/MacOSAppProvider.cpp
)

add_library(OpenWith MODULE ${SOURCES})

# Link with required libraries (utils contains KeyFileHelper)
target_link_libraries(OpenWith utils far2l)

target_compile_definitions(OpenWith PRIVATE -DWINPORT_DIRECT -DUNICODE -DFAR_DONT_USE_INTERNALS)

target_include_directories(OpenWith PRIVATE .)
target_include_directories(OpenWith PRIVATE ../far2l/far2sdk)
target_include_directories(OpenWith PRIVATE ../WinPort)
target_include_directories(OpenWith PRIVATE ../utils/include)

set_target_properties(OpenWith
    PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${INSTALL_DIR}/Plugins/OpenWith/"
        PREFIX ""
        SUFFIX ".far-plug-wide")

# copy resource files
set(CURRENT_TARGET "OpenWith")
file(GLOB_RECURSE RESOURCE_FILES
        RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}/configs
        "${CMAKE_CURRENT_SOURCE_DIR}/configs/*"
)
setup_target_auxiliary_files(${CURRENT_TARGET}
        RESOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/configs
        "${INSTALL_DIR}/Plugins/${CURRENT_TARGET}/"
)
add_dependencies(${CURRENT_TARGET} copy_aux_files_for_${CURRENT_TARGET})


# Create plugin directory in install location
install(DIRECTORY DESTINATION "${INSTALL_DIR}/Plugins/${CURRENT_TARGET}/")
