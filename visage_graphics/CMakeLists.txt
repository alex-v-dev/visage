file(GLOB HEADERS *.h)
file(GLOB SOURCE_FILES *.cpp)

if (WIN32)
  list(APPEND SOURCE_FILES win32/emoji_win32.cpp)
  set(WIN_LIBS d2d1 dwrite Windowscodecs)
elseif (APPLE)
  list(APPEND SOURCE_FILES macos/emoji_macos.cpp)
  list(APPEND PLATFORM_FILES macos/windowless_context.mm)
elseif (UNIX)
  list(APPEND SOURCE_FILES linux/emoji_linux.cpp)
endif ()

if (VISAGE_AMALGAMATED_BUILD)
  amalgamate_sources(AMALGAMATED ${CMAKE_CURRENT_BINARY_DIR}/visage_graphics_amalgamated "${SOURCE_FILES}")
  list(APPEND SOURCE_FILES ${AMALGAMATED})
endif ()

amalgamate_headers(${VISAGE_INCLUDE}/visage/graphics.h "${HEADERS}")
list(APPEND HEADERS ${VISAGE_INCLUDE}/visage/graphics.h)

add_library(VisageGraphics OBJECT ${SOURCE_FILES} ${PLATFORM_FILES} ${HEADERS})

if (APPLE)
  find_library(METAL_LIBRARY Metal)
  find_library(METALKIT_LIBRARY MetalKit)
  set(METAL_LIBS ${METAL_LIBRARY} ${METALKIT_LIBRARY})
elseif (UNIX)
  add_definitions(-D_LARGEFILE64_SOURCE)
endif ()

option(VISAGE_SYSTEM_BGFX "Use pre-installed bgfx/bx/bimg via find_package instead of FetchContent" OFF)
option(VISAGE_SYSTEM_FREETYPE "Use pre-installed FreeType via find_package instead of FetchContent" OFF)

if (MSVC)
  set(DISABLE_WARNINGS /W0)
else ()
  set(DISABLE_WARNINGS -w)
endif ()

if (VISAGE_ENABLE_GRAPHICS_DEBUG_LOGGING)
  target_compile_definitions(VisageGraphics PUBLIC VISAGE_GRAPHICS_DEBUG_LOGGING=1)
endif ()

set(BGFX_CONFIG_MULTITHREADED 0)
if (NOT EMSCRIPTEN AND VISAGE_ENABLE_BACKGROUND_GRAPHICS_THREAD)
  set(BGFX_CONFIG_MULTITHREADED 1)
  target_compile_definitions(VisageGraphics PUBLIC VISAGE_BACKGROUND_GRAPHICS_THREAD=1)
endif ()

# --- bgfx / bx / bimg ---
if (VISAGE_SYSTEM_BGFX)
  find_package(bgfx REQUIRED CONFIG)
  set(BGFX_LINK_TARGETS bgfx::bgfx bgfx::bx bgfx::bimg)

  # visage decodes images (bimg::imageParse) and writes PNGs (bimg::imageWritePng),
  # which live in bimg_decode / bimg_encode. Link them when the bgfx package ships
  # them (bgfx.cmake only installs these alongside a SHARED bgfx if patched to).
  foreach (BIMG_EXTRA bgfx::bimg_decode bgfx::bimg_encode)
    if (TARGET ${BIMG_EXTRA})
      list(APPEND BGFX_LINK_TARGETS ${BIMG_EXTRA})
    endif ()
  endforeach ()

  # find_package() imports targets only into this directory's scope by
  # default. visage links these from the top-level CMakeLists.txt too, so
  # promote them to global visibility.
  foreach (BGFX_TARGET ${BGFX_LINK_TARGETS})
    get_target_property(BGFX_TARGET_IS_GLOBAL ${BGFX_TARGET} IMPORTED_GLOBAL)
    if (NOT BGFX_TARGET_IS_GLOBAL)
      set_target_properties(${BGFX_TARGET} PROPERTIES IMPORTED_GLOBAL TRUE)
    endif ()
  endforeach ()

  FetchContent_Declare(stb
    URL https://github.com/nothings/stb/archive/refs/heads/master.zip
    SOURCE_DIR "${CMAKE_BINARY_DIR}/_deps/stb"
  )
  FetchContent_MakeAvailable(stb)

  set(BGFX_PRIVATE_INCLUDES
    ${CMAKE_BINARY_DIR}/_deps
  )
else ()
  set(BGFX_BUILD_EXAMPLES OFF CACHE INTERNAL "" FORCE)
  set(BGFX_BUILD_EXAMPLE_COMMON OFF CACHE INTERNAL "" FORCE)
  set(BGFX_BUILD_TOOLS OFF CACHE INTERNAL "" FORCE)
  set(BGFX_BUILD_TESTS OFF CACHE INTERNAL "" FORCE)
  set(BGFX_INSTALL OFF CACHE INTERNAL "" FORCE)
  set(BX_AMALGAMATED ON CACHE INTERNAL "" FORCE)
  set(BX_CONFIG_DEBUG OFF CACHE INTERNAL "" FORCE)
  set(BGFX_CONFIG_DEBUG_ANNOTATION ${VISAGE_ENABLE_GRAPHICS_DEBUG_LOGGING} CACHE INTERNAL "" FORCE)
  set(BGFX_AMALGAMATED ON CACHE INTERNAL "" FORCE)
  set(BGFX_CONFIG_MAX_FRAME_BUFFERS 1024 CACHE INTERNAL "" FORCE)
  set(BGFX_CONFIG_MAX_VIEWS 256 CACHE INTERNAL "" FORCE)
  set(BGFX_CONFIG_MAX_DRAW_CALLS 8192 CACHE INTERNAL "" FORCE)
  set(BGFX_WITH_WAYLAND OFF CACHE INTERNAL "" FORCE)
  set(BGFX_CONFIG_MULTITHREADED ${BGFX_CONFIG_MULTITHREADED} CACHE INTERNAL "" FORCE)

  message(STATUS "VISAGE: Downloading bgfx")
  set(CMAKE_MESSAGE_LOG_LEVEL "ERROR")
  FetchContent_Declare(bgfx GIT_REPOSITORY https://github.com/bkaradzic/bgfx.cmake.git
    GIT_SHALLOW TRUE
    GIT_TAG v1.129.8958-499
    EXCLUDE_FROM_ALL
  )
  FetchContent_MakeAvailable(bgfx)
  set(CMAKE_MESSAGE_LOG_LEVEL "NOTICE")

  target_compile_options(bgfx PRIVATE ${DISABLE_WARNINGS})
  target_compile_options(bx PRIVATE ${DISABLE_WARNINGS})
  target_compile_definitions(bx PRIVATE BX_CONFIG_EXCEPTION_HANDLING_USE_WINDOWS_SEH=0)
  target_compile_options(bimg_decode PRIVATE ${DISABLE_WARNINGS})
  target_compile_options(bimg_encode PRIVATE ${DISABLE_WARNINGS})
  target_compile_options(bimg PRIVATE ${DISABLE_WARNINGS})

  set_target_properties(bgfx PROPERTIES FOLDER "visage/third_party/bgfx")
  set_target_properties(bx PROPERTIES FOLDER "visage/third_party/bgfx")
  set_target_properties(bimg PROPERTIES FOLDER "visage/third_party/bgfx")
  set_target_properties(bimg_decode PROPERTIES FOLDER "visage/third_party/bgfx")
  set_target_properties(bimg_encode PROPERTIES FOLDER "visage/third_party/bgfx")

  set(BGFX_LINK_TARGETS bgfx bx bimg_decode bimg_encode bimg)
  set(BGFX_PRIVATE_INCLUDES
    ${BGFX_DIR}/include
    ${BGFX_DIR}/3rdparty
    ${BX_DIR}/include
    ${BIMG_DIR}/include
  )

  # bgfx/bx/bimg* are built locally by this project (via FetchContent) and
  # are never installed/exported by bgfx.cmake itself (BGFX_INSTALL is off
  # above), so visage must install/export them itself - otherwise
  # install(EXPORT) would reject visage's link to them as "not in any
  # export set" (see top-level CMakeLists.txt).
  set(VISAGE_LOCAL_THIRD_PARTY_TARGETS bgfx bx bimg_decode bimg_encode bimg)
endif ()

# --- FreeType ---
if (VISAGE_SYSTEM_FREETYPE)
  find_package(Freetype REQUIRED CONFIG)
  set(FREETYPE_LINK_TARGET Freetype::Freetype)

  # Promote to global visibility so the top-level CMakeLists.txt can also
  # link it (see the bgfx promotion above for why).
  get_target_property(FREETYPE_TARGET_IS_GLOBAL ${FREETYPE_LINK_TARGET} IMPORTED_GLOBAL)
  if (NOT FREETYPE_TARGET_IS_GLOBAL)
    set_target_properties(${FREETYPE_LINK_TARGET} PROPERTIES IMPORTED_GLOBAL TRUE)
  endif ()
else ()
  set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
  set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
  set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
  set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
  set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)

  message(STATUS "VISAGE: Downloading FreeType")
  set(CMAKE_MESSAGE_LOG_LEVEL "ERROR")
  FetchContent_Declare(freetype GIT_REPOSITORY https://gitlab.freedesktop.org/freetype/freetype.git
    GIT_SHALLOW TRUE
    GIT_TAG VER-2-14-1
    EXCLUDE_FROM_ALL
  )
  FetchContent_MakeAvailable(freetype)
  set(CMAKE_MESSAGE_LOG_LEVEL "NOTICE")

  target_compile_options(freetype PRIVATE ${DISABLE_WARNINGS})
  set_target_properties(freetype PROPERTIES FOLDER "visage/third_party")

  set(FREETYPE_LINK_TARGET freetype)
  set(FREETYPE_PRIVATE_INCLUDES ${freetype_SOURCE_DIR}/include)

  # freetype is built locally by this project (via FetchContent) and is
  # never installed/exported by its own CMakeLists.txt, so visage must
  # install/export it itself - see the bgfx note above.
  list(APPEND VISAGE_LOCAL_THIRD_PARTY_TARGETS freetype)
endif ()

target_link_libraries(
  VisageGraphics
  PUBLIC
  ${BGFX_LINK_TARGETS}
  ${FREETYPE_LINK_TARGET}
  PRIVATE
  ${WIN_LIBS}
  ${METAL_LIBS}
  ${FONTCONFIG_LIBRARIES}
)

# VisageGraphics is merged into visage as an OBJECT library (see top-level
# CMakeLists.txt), so its own link libraries never reach an install(EXPORT) -
# forward the real (non-target-local) link requirements up to visage itself.
set(VISAGE_GRAPHICS_PUBLIC_LIBS ${BGFX_LINK_TARGETS} ${FREETYPE_LINK_TARGET} PARENT_SCOPE)
set(VISAGE_GRAPHICS_PRIVATE_LIBS ${WIN_LIBS} ${METAL_LIBS} ${FONTCONFIG_LIBRARIES} PARENT_SCOPE)
set(VISAGE_LOCAL_THIRD_PARTY_TARGETS ${VISAGE_LOCAL_THIRD_PARTY_TARGETS} PARENT_SCOPE)

include(embedded.cmake)

target_include_directories(VisageGraphics
  PRIVATE
  ${VISAGE_INCLUDE_PATH}
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${BGFX_PRIVATE_INCLUDES}
  ${FREETYPE_PRIVATE_INCLUDES}
)

if(WIN32)
  target_compile_definitions(VisageGraphics PRIVATE
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
    NTDDI_VERSION=0x0A000000
  )
endif()

target_include_directories(VisageGraphics
  SYSTEM PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/bgfx/3rdparty
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/bgfx/include
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/bgfx/tools
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/bx/include
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/bimg/include
)

set_target_properties(VisageGraphics PROPERTIES FOLDER "visage")

visage_add_test_target(
  TARGET VisageGraphicsTests
  TEST_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/tests
)

if (APPLE)
  target_compile_options(VisageGraphics PRIVATE -fobjc-arc)
endif ()
