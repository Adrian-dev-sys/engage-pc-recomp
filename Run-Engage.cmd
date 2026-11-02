@echo off
setlocal
set "TOTK_GPU=vulkan"
set "TOTK_VK_RECORDER=0"
set "TOTK_SHARED_FONTS=%~dp0build\extracted\firmware\shared-fonts"
if not exist "%~dp0build\engage-v200-cont\engage.exe" (
  echo Build the experimental executable first. See docs\engage-port.md.
  exit /b 1
)
"%~dp0build\engage-v200-cont\engage.exe" %*
