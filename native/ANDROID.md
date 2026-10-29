# The native build on Android

The game built for Android (arm64-v8a, Android 10 and later: its thread-local storage is the CPU's own, which older linkers can't load) with the NDK's clang, as `libmain.so` under SDL3's activity,
drawing with OpenGL ES 3.0 instead of OpenGL 3.3, with touch controls on the screen. The platform code is the native one
(native/NATIVE.md) with what Android needs apart:

| Where | Android |
|---|---|
| `src/platform/native/graphics/glcompat.h/.cpp` | OpenGL ES 3.0: the context's attributes; the shaders (written for GLSL 330 core) translated to GLSL ES 3.00 as they're compiled (the version, precision statements, `noperspective` dropped: every vertex has w = 1, so the perspective-correct interpolation is the same; the dual-source blend's outputs by `GL_EXT_blend_func_extended` when the GPU has it, else plain variables and the renderer blends in the shader as it does where GL's blend isn't exact); the offscreen contexts (the render thread's) made on the one window Android allows and made current without a surface (or on an EGL pbuffer of our own) |
| `hw/gl.cpp`, `hw/hwgs.cpp` | No `glDrawBuffer` (`glDrawBuffers`), `glPointSize` (the vertex shader's `gl_PointSize` alone), `glBindFragDataLocationIndexed` (the shader's layout qualifiers); anisotropy by its extension; depth buffers read back by drawing them into an R32F texture (ES reads no depth directly); integer buffers read back as RGBA_INTEGER and compacted unless the implementation reads RED_INTEGER |
| `system.cpp` | The settings, saves, mods and texture packs in the app's external files folder: `Android/data/org.twinsanity.native/files` on the shared storage (over USB or a file manager), else the app's private folder. The window is the whole screen, landscape |
| `disc.cpp`, `ui/launcher.cpp` | The launcher at every start (`$TWIN_LAUNCHER=0` skips it; `=1` shows it elsewhere): a screen in the options' look on the game's window (the overlay's renderer in a GL context of its own, destroyed before the graphics make theirs) with the disc image remembered (its name from the system: `TwinsanityActivity.displayName`), whether it's the PAL disc or what's wrong, "choose disc image" (the system's document picker, SDL's file dialog: a `content://` URI, opened through the content resolver and read with `pread` as a file; the permission is kept across launches and the URI remembered in `disc_image.txt`) and "play"; back quits |
| `ui/touch.cpp` | The touch controls (also `$TWIN_TOUCH=1` elsewhere, to try them): a floating left stick, the four face buttons, L1 L2 R1 R2 at the top corners, START and SELECT at the top; a drag anywhere free on the other side of the screen from the stick is the right stick (the camera). Added to the pad's read (`pads.cpp`) and drawn by the overlay; hidden while a gamepad is connected and nothing is touched. Options > Controls: shown or not, their size (50-150%) and opacity, the layout editor (each button dragged where it's wanted, pinched bigger or smaller; `touch_layout.txt` keeps it) and its reset. The mouse's options aren't shown (the touches are the menus' pointer). Bluetooth and USB gamepads work through SDL as on the desktop |
| `ui/overlay.cpp` | The UI's fonts from the APK's assets |
| `lowmemory.cpp` | The arm64 Linux path (the signal's `ucontext`); no backtraces (bionic has `execinfo.h` from API 33) |
| `entry.cpp` | `main` is `SDL_main` (`SDL3/SDL_main.h`), called by the activity |

## Building

Linux (the build was made on Ubuntu 24.04). Needs the split made first (README.md: the PAL disc's `SLES_525.68`, `tools/split.py`),
then:

- The Android NDK r27 or later (`android-ndk-r30-linux.zip`), unzipped anywhere: `--ndk` or `$ANDROID_NDK`.
- The SDK's build-tools 34 (`build-tools_r34-linux.zip`) and platform 34 (`platform-34_r02.zip`), unzipped: `--build-tools DIR`
  (the folder with `aapt2`, `d8`, `zipalign`, `apksigner`) and `--platform DIR` (the folder with `android.jar`), or `$ANDROID_SDK`
  with them at `build-tools/34.0.0` and `platforms/android-34`.
- A JDK 17 or later (`javac`, `keytool`), CMake 3.24+, Ninja, make, pkg-config.
- SDL3's source release (downloaded by the script, or `--sdl-source DIR`) and FFmpeg 9.0.2's source in
  `native/third_party/ffmpeg-src/ffmpeg-9.0.2` (native/tools/build_ffmpeg.sh downloads it; or `--ffmpeg-source DIR`).

```sh
.venv/bin/python native/tools/build_android.py --ndk /path/android-ndk-r30 --build-tools /path/build-tools/34.0.0 \
    --platform /path/platforms/android-34
```

It builds SDL3 for Android into `build/android/deps`, the decode-only LGPL FFmpeg into `build/native/ffmpeg/android-arm64`,
configures `build/android/cmake` with the NDK's toolchain file (the layout tools run the NDK's
`aarch64-linux-android24-clang++`, `TWIN_TOOL_CLANG`), builds `libmain.so`, and packs
`build/android/Crash-Twinsanity-Android.apk`: SDL's Java activity and ours compiled with `javac` and dexed with `d8`, the
resources and manifest (`android/app/src/main`) by `aapt2`, the libraries (ours, SDL3's, FFmpeg's `libavcodec.so` and
`libavutil.so`), the UI's fonts as assets, aligned with `zipalign -p` and signed with a debug key (`build/android/debug.keystore`,
made by `keytool` the first time).

## Playing

Install the APK (allow installing from unknown sources). The launcher comes first: "choose disc image" opens the system's file
picker; pick your own PAL disc's `.iso` wherever it is on the phone (Downloads, say), then "play". The launcher remembers it. Settings and saves go
to `Android/data/org.twinsanity.native/files`. The options (the native menu) work with touches as the mouse's; the game's own
menus answer the touch buttons.

The default picture is 2x the PS2's resolution; a slower phone may want 1x (Options > Graphics > resolution).

## What isn't done

- No tests ran on a phone before the first APK; the ES path was checked on Mesa's llvmpipe (OpenGL ES 3.2) with the level
  sweep's snapshots, headless, on Linux.
- The render thread's context relies on `GL_OES_surfaceless_context` (SDL), else a pbuffer of its own.
- A phone with no `GL_EXT_color_buffer_float` / `GL_EXT_color_buffer_half_float` can't render to the R32F and RGBA16F targets the
  renderer uses (the depth copies, the wrapping additive blend): nearly every OpenGL ES 3.1+ GPU has them.
- Performance is unknown: the game emulates the PS2's VU1 and GS on the CPU and GPU.
