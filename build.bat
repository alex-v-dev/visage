@echo off
setlocal

set INSTALL_DIR=Z:\avDependencies-msvc2022-reldeb-shared\Visage
set BUILD_TYPE=%~2
if "%BUILD_TYPE%"=="" set BUILD_TYPE=RelWithDebInfo

mkdir build 2>nul

cmake -B build ^
  -D CMAKE_INSTALL_PREFIX=%INSTALL_DIR% ^
  -D CMAKE_PREFIX_PATH=%INSTALL_DIR% ^
  -D BUILD_SHARED_LIBS=ON ^
  -D VISAGE_BUILD_EXAMPLES=OFF ^
  -D VISAGE_BUILD_TESTS=OFF ^
  -D VISAGE_SYSTEM_BGFX=ON ^
  -D VISAGE_SYSTEM_FREETYPE=ON ^
  -D bgfx_DIR=Z:\avDependencies-msvc2022-reldeb-shared\BGFX\lib\cmake\bgfx
  .

cmake --build build --config %BUILD_TYPE% --target install
