@echo off
setlocal
cd /d "%~dp0"

echo [1/2] Building PhotosSlideshow (single-file, framework-dependent)...
dotnet publish PhotosSlideshow -c Release -r win-x64 --self-contained false -p:PublishSingleFile=true -o PhotosSlideshow\publish || exit /b 1

echo [2/2] Building PhotosSlideshowHook...
call PhotosSlideshowHook\build.bat || exit /b 1

if not exist dist mkdir dist
copy /y PhotosSlideshow\publish\PhotosSlideshow.exe dist\ || exit /b 1
copy /y PhotosSlideshowHook\PhotosSlideshowHook.dll dist\ || exit /b 1
echo.
echo Done. Binaries are in dist\ - ship PhotosSlideshow.exe + PhotosSlideshowHook.dll together.
echo (If the copy fails with a sharing violation, close Photos.exe first.)
