# MS Photos Slideshow Launcher

Launch Microsoft Photos in slideshow mode for a folder or an explicit file
list, with a configurable per-image duration and slideshow settings (loop /
animations / transitions) that persist between launches.

Works with the normal installed Photos app - nothing under
`C:\Program Files\WindowsApps` is modified.

> **Supported build:** `Microsoft.Windows.Photos_2026.11090.22001.0_x64__8wekyb3d8bbwe`
> (tested). The hook depends on version-specific internals of `Slideshow.dll`
> and will likely not work on other Photos builds. After a Photos update,
> check `hook.log` - if the expected patch lines are missing, it's
> incompatible.

## Requirements

- Windows 10/11 x64 with the supported Photos build installed
- .NET 8 **Desktop Runtime** (to run the exe) and .NET 8 SDK (to build)
- Visual Studio 2022/2026 with "Desktop development with C++" (to rebuild the
  hook DLL only)

## Build

```
build.bat
```

Produces `dist\PhotosSlideshow.exe` (~22 MB, single file) and
`dist\PhotosSlideshowHook.dll`. Ship those two together, plus optionally a
`settings.ini`.

## Usage

```
PhotosSlideshow.exe "C:\path\to\folder"            slideshow of a folder (top level)
PhotosSlideshow.exe "C:\path\to\folder" -r         recurse into subfolders
PhotosSlideshow.exe "C:\path\to\folder" -s         shuffle playback order
PhotosSlideshow.exe "C:\path\to\folder" --windowed start windowed, not fullscreen
PhotosSlideshow.exe --files "C:\a.jpg;C:\b.png"    explicit file list
PhotosSlideshow.exe --duration 15000                persist duration (ms), no launch
PhotosSlideshow.exe --shuffle                       persist shuffle=1, no launch
PhotosSlideshow.exe --recursive                     persist recursion=1, no launch
PhotosSlideshow.exe "C:\folder" --duration 8000     8 s/image for this launch only
```

`--duration` takes any positive millisecond value (default 5000).

### Persisting vs per-launch settings

`--shuffle`, `--recursive`, and `--duration` persist to `settings.ini` **only
when no folder/files are passed**. Combined with a launch, they override the
saved values for that run only. So `"C:\pics" --duration 20000` plays once at
20 s/image, while `--duration 20000` alone makes it the new default.

## Configuration

`settings.ini` next to the exe:

```ini
[Slideshow]
DurationMs=5000
Shuffle=0
Recursive=0
Loop=1
Animations=1
Transitions=1
```

- `DurationMs` - ms per image, any positive value (default 5000)
- `Shuffle` - `1`/`0` (default `0`); shuffles the file list. No `--no-shuffle`
  flag - edit the ini to disable.
- `Recursive` - `1`/`0` (default `0`); folder launches include subfolders.
  Like `Shuffle`, there's no per-launch "off" flag - edit the ini to disable.
- `Loop`, `Animations`, `Transitions` - optional; absent = Photos default.
  Written automatically when toggled in the slideshow viewer.

## How it works (briefly)

The exe fires the app's own `ms-photos:slideshow` protocol with the file
list, then injects `PhotosSlideshowHook.dll` into `Photos.exe`. The hook
patches `Slideshow.dll`'s delay-load table to substitute the native slideshow
callbacks (duration, initial settings, settings-change notifications) and
rewrites the duration literals in the bundled slideshow JS - all in memory,
no app files touched. `--windowed` sends a real `F11` keystroke, the same
accelerator as the fullscreen button.

Per-launch `--duration` reaches the hook via a temporary
`session_duration.txt` next to the exe (deleted after injection).

## Diagnostics

`hook.log` next to the exe is truncated on every launch and contains only
that session's hook output (~1 MB self-cap). Look for `patched ... IAT` and
`rewrote imageItemDurationMs literals` lines - their absence after a Photos
update means the build is unsupported.

## Limitations

- Version-locked to the supported build above.
- Large folders are capped (~900 files / ~90 KB paths) for the protocol
  payload limit.
- `--windowed` sends F11 once at startup; Photos re-entering fullscreen
  later is not handled.
- Unofficial tooling that injects a DLL into a running process - intended
  for personal use.
