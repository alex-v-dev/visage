if (MSVC)
  set(VISAGE_DEFINE_FLAG "/D")
else()
  set(VISAGE_DEFINE_FLAG "-D")
endif()

if (EMSCRIPTEN)
  add_compile_options(-DVISAGE_EMSCRIPTEN=1)
elseif (WIN32)
  add_compile_options(${VISAGE_DEFINE_FLAG}VISAGE_WINDOWS=1)
elseif (APPLE)
  add_compile_options(-DVISAGE_MAC=1)
elseif (UNIX)
  add_compile_options(-DVISAGE_LINUX=1)
endif()

# Lets headers know whether VISAGE_EXPORT should resolve to
# __declspec(dllexport)/dllimport for the visage shared library on Windows.
if (WIN32 AND BUILD_SHARED_LIBS)
  add_compile_definitions(VISAGE_BUILD_SHARED)
endif()

if (MSVC)
  add_compile_options(/MP /wd4244 /wd4267 /JMC)
else()
  add_compile_options(-Wno-conversion -Wno-sign-conversion)
endif()

add_compile_definitions(VISAGE_APPLICATION_NAME=\"${VISAGE_APPLICATION_NAME}\")
