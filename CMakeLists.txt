cmake_minimum_required(VERSION 3.17)
set(CMAKE_POLICY_DEFAULT_CMP0177 NEW)
set(CMAKE_POLICY_DEFAULT_CMP0135 NEW)

if (APPLE AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "Minimum macOS deployment version")
endif ()

if (NOT DEFINED CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
endif ()

project(visage VERSION 0.1.0)

if (APPLE)
  enable_language(OBJC)
  enable_language(OBJCXX)
endif ()

option(VISAGE_AMALGAMATED_BUILD "Compile sources together" ON)
option(VISAGE_ENABLE_WIDGETS "Add widgets library" ON)
option(VISAGE_ENABLE_BACKGROUND_GRAPHICS_THREAD "Offloads graphics rendering to a background thread" OFF)
option(VISAGE_ENABLE_GRAPHICS_DEBUG_LOGGING "Shows graphics debug log in console in debug mode" OFF)
option(VISAGE_ADDRESS_SANITIZER "Enable AddressSanitizer" OFF)
option(BUILD_SHARED_LIBS "Build shared libraries" OFF)

if (VISAGE_ADDRESS_SANITIZER)
  if (MSVC)
    add_compile_options(/fsanitize=address /Zi)
    add_link_options(/fsanitize=address)
  else ()
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
  endif ()
endif ()

set(VISAGE_APPLICATION_NAME ${CMAKE_PROJECT_NAME} CACHE STRING "Application name used for default window title")

if (CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
  option(VISAGE_BUILD_EXAMPLES "Build examples" ON)
  option(VISAGE_BUILD_TESTS "Build tests" ON)
else ()
  option(VISAGE_BUILD_EXAMPLES "Build examples" OFF)
  option(VISAGE_BUILD_TESTS "Build tests" OFF)
endif ()

# Object/module libraries are linked into the visage library (which may be
# shared), so they must always be compiled as position-independent code.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(VISAGE_INCLUDE_PATH ${CMAKE_CURRENT_SOURCE_DIR})

include(FetchContent)
include(CMakePackageConfigHelpers)
include(GNUInstallDirs)
include(cmake/compile_flags.cmake)
include(cmake/testing_framework.cmake)

set(VISAGE_INCLUDE ${CMAKE_CURRENT_BINARY_DIR}/include)

function(amalgamate_headers AMALGAMATED_HEADER_NAME FILE_HEADERS)
  set(FILE_CONTENTS "// Auto generated amalgamated header.\n#pragma once\n")
  foreach (FILE ${FILE_HEADERS})
    set(FILE_CONTENTS "${FILE_CONTENTS}#include \"${FILE}\"\n")
  endforeach ()
  file(CONFIGURE OUTPUT "${AMALGAMATED_HEADER_NAME}" CONTENT "${FILE_CONTENTS}")
endfunction()

function(amalgamate_sources GENERATED_FILES AMALGAMATED_SOURCE_BASE FILE_SOURCES)
  set(IDX 0)
  set(PART 0)
  set(GENERATED "")

  foreach (FILE ${FILE_SOURCES})
    set_source_files_properties("${FILE}" PROPERTIES HEADER_FILE_ONLY TRUE)
    if (IDX EQUAL 0)
      if (PART GREATER 0)
        file(CONFIGURE OUTPUT "${AMALGAMATED_SOURCE_BASE}${PART}.cpp" CONTENT "${FILE_CONTENTS}")
      endif ()

      math(EXPR PART "${PART} + 1")
      list(APPEND GENERATED "${AMALGAMATED_SOURCE_BASE}${PART}.cpp")
      set(FILE_CONTENTS "// Auto generated amalgamated source.\n")
    endif ()
    set(FILE_CONTENTS "${FILE_CONTENTS}#include \"${FILE}\"\n")

    math(EXPR IDX "${IDX} + 1")
    if (IDX GREATER 6)
      set(IDX 0)
    endif ()
  endforeach ()

  set(${GENERATED_FILES} "${GENERATED}" PARENT_SCOPE)
  file(CONFIGURE OUTPUT "${AMALGAMATED_SOURCE_BASE}${PART}.cpp" CONTENT "${FILE_CONTENTS}")
endfunction()

# Copy the shared libraries an executable depends on (visage.dll and, behind it,
# bgfx.dll / freetype.dll) next to the executable so it can be launched and run
# directly from the build tree. No-op on non-Windows / static builds.
function(visage_copy_runtime_dlls TARGET)
  if (NOT WIN32)
    return ()
  endif ()

  set(DLL_SETS "$<TARGET_RUNTIME_DLLS:${TARGET}>")
  if (BUILD_SHARED_LIBS)
    # visage links bgfx/freetype privately, so their DLLs are not in the
    # executable's own runtime closure; pull them in explicitly. Module
    # test targets call this before `visage` is declared at the top level,
    # so we can't gate on `TARGET visage` here - it would always be false.
    # The generator expression itself only resolves at generate time, once
    # the whole project (including the visage target) is known, so this is
    # safe even though the target doesn't exist yet at this point.
    list(APPEND DLL_SETS "$<TARGET_RUNTIME_DLLS:visage>")
  endif ()

  # When there are no runtime DLLs at all (e.g. a fully static build with
  # in-tree bgfx/freetype), $<TARGET_RUNTIME_DLLS:...> expands to nothing
  # and copy_if_different would get no source files, which is a hard error.
  # Fall back to a no-op command (`true`) in that case.
  add_custom_command(TARGET ${TARGET} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E $<IF:$<BOOL:$<TARGET_RUNTIME_DLLS:${TARGET}>>,copy_if_different,true> ${DLL_SETS} "$<TARGET_FILE_DIR:${TARGET}>"
    COMMAND_EXPAND_LISTS
  )
endfunction()

add_subdirectory(visage_file_embed)
add_subdirectory(visage_graphics)
add_subdirectory(visage_ui)
add_subdirectory(visage_utils)
add_subdirectory(visage_widgets)
add_subdirectory(visage_windowing)
add_subdirectory(visage_app)

# set(DUMMY_FILE ${CMAKE_CURRENT_BINARY_DIR}/dummy.cpp)
# if (NOT EXISTS ${DUMMY_FILE})
#   file(WRITE ${DUMMY_FILE} "")
# endif ()

add_library(visage)
add_library(visage::visage ALIAS visage)

target_include_directories(visage SYSTEM
  PUBLIC
  $<BUILD_INTERFACE:${VISAGE_INCLUDE_PATH}>
  $<BUILD_INTERFACE:${VISAGE_INCLUDE}>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

set(VISAGE_OBJECT_LIBS
  VisageApp
  VisageGraphics
  VisageUtils
  VisageUi
  VisageWindowing
)

if (VISAGE_ENABLE_WIDGETS)
  list(APPEND VISAGE_OBJECT_LIBS VisageWidgets)
endif ()

if (BUILD_SHARED_LIBS)
  foreach (LIB ${VISAGE_OBJECT_LIBS})
    target_compile_definitions(${LIB} PRIVATE VISAGE_BUILDING_DLL)
  endforeach ()
endif ()

# 1. Merge the object files directly into visage
target_sources(visage PRIVATE
  $<TARGET_OBJECTS:VisageApp>
  $<TARGET_OBJECTS:VisageGraphics>
  $<TARGET_OBJECTS:VisageUtils>
  $<TARGET_OBJECTS:VisageUi>
  $<TARGET_OBJECTS:VisageWindowing>
  $<TARGET_OBJECTS:VisageEmbeddedShaders>
  $<TARGET_OBJECTS:VisageEmbeddedFonts>
  $<TARGET_OBJECTS:VisageEmbeddedIcons>
)

if (VISAGE_ENABLE_WIDGETS)
  target_sources(visage PRIVATE $<TARGET_OBJECTS:VisageWidgets>)
endif ()

# 2. Link the real external/system requirements of the merged object
# libraries. The object libraries themselves must not be linked in here:
# their compiled objects are already merged above, and linking the targets
# too would pull them into visage's INTERFACE_LINK_LIBRARIES, which
# install(EXPORT) rejects since none of them belong to an export set.
target_link_libraries(visage
  PUBLIC
  ${VISAGE_GRAPHICS_PUBLIC_LIBS}
  PRIVATE
  ${VISAGE_GRAPHICS_PRIVATE_LIBS}
  ${VISAGE_WINDOWING_PRIVATE_LIBS}
)

set_target_properties(visage PROPERTIES FOLDER "visage")

install(
  TARGETS visage ${VISAGE_LOCAL_THIRD_PARTY_TARGETS}
  EXPORT visageTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(
  DIRECTORY
  ${VISAGE_INCLUDE}/visage
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(
  DIRECTORY
  ${CMAKE_CURRENT_SOURCE_DIR}/visage_app
  ${CMAKE_CURRENT_SOURCE_DIR}/visage_graphics
  ${CMAKE_CURRENT_SOURCE_DIR}/visage_ui
  ${CMAKE_CURRENT_SOURCE_DIR}/visage_utils
  ${CMAKE_CURRENT_SOURCE_DIR}/visage_widgets
  ${CMAKE_CURRENT_SOURCE_DIR}/visage_windowing
  ${CMAKE_CURRENT_SOURCE_DIR}/visage_file_embed
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  FILES_MATCHING PATTERN "*.h"
)

install(
  EXPORT visageTargets
  FILE visageTargets.cmake
  NAMESPACE visage::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/visage
)

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/visageConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
)

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/visageConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/visageConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/visage
)

install(
  FILES
  ${CMAKE_CURRENT_BINARY_DIR}/visageConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/visageConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/visage
)

if (VISAGE_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif ()
