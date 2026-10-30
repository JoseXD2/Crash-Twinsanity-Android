#!/usr/bin/env python3
"""The Android build (native/ANDROID.md): the game as libmain.so for arm64-v8a with the NDK, SDL3 and a decode-only LGPL FFmpeg
built for Android too, packed into an APK with the SDK's build tools (no Gradle: javac, d8, aapt2, zipalign, apksigner) and
signed with a debug key.

    .venv/bin/python native/tools/build_android.py [--ndk DIR] [--build-tools DIR] [--platform android.jar's DIR] [--configure]
                                                   [--clean] [ninja arguments...]

Needs: the NDK (r27 or later: clang 18+; $ANDROID_NDK or --ndk), the SDK's build-tools 34 (--build-tools, else
$ANDROID_SDK/build-tools/34.0.0) and platform 34's android.jar (--platform, else $ANDROID_SDK/platforms/android-34), a JDK (javac,
keytool), CMake 3.24+, Ninja, make and the split (tools/split.py). SDL3's source release is downloaded (or given with --sdl-source).
The result: build/android/Crash-Twinsanity-Android.apk
"""
import argparse
import glob
import hashlib
import os
import shutil
import subprocess
import sys
import tarfile
import urllib.request
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parents[2]
OUT = HERE / "build" / "android"
DEPS = OUT / "deps"
ABI = "arm64-v8a"
API = 24
# (the library needs Android 10 to load: -fno-emulated-tls, native/CMakeLists.txt; the manifest asks for 29)
SDL_VERSION = "3.4.18"
SDL_URL = f"https://github.com/libsdl-org/SDL/releases/download/release-{SDL_VERSION}/SDL3-{SDL_VERSION}.tar.gz"
SDL_SHA256 = "9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3"
FFMPEG_VERSION = "9.0.2"
# VkGl's shader compiler (src/platform/native/graphics/vk/vkglprogram.cpp): glslang, static, without its optimizer or HLSL
GLSLANG_VERSION = "15.1.0"
GLSLANG_URL = f"https://github.com/KhronosGroup/glslang/archive/refs/tags/{GLSLANG_VERSION}.tar.gz"
GLSLANG_SHA256 = "4bdcd8cdb330313f0d4deed7be527b0ac1c115ff272e492853a6e98add61b4bc"
PACKAGE = "org.twinsanity.native"


def run(command, **kwargs):
    print("+", " ".join(str(c) for c in command), flush=True)
    subprocess.run([str(c) for c in command], check=True, **kwargs)


def find_tool(name, hint):
    found = shutil.which(name)
    if found is None:
        venv = Path(sys.executable).parent / name
        if venv.exists():
            return str(venv)
        sys.exit(f"{name} isn't installed ({hint})")
    return found


def download(url, target, sha256):
    if not target.exists():
        print(f"downloading {url}", flush=True)
        target.parent.mkdir(parents=True, exist_ok=True)
        partial = target.with_suffix(target.suffix + ".part")
        with urllib.request.urlopen(url) as response, open(partial, "wb") as out:
            shutil.copyfileobj(response, out)
        partial.rename(target)
    actual = hashlib.sha256(target.read_bytes()).hexdigest()
    if actual != sha256:
        sys.exit(f"{target}'s SHA-256 is {actual}, not {sha256}")


class Ndk:
    def __init__(self, folder):
        self.folder = Path(folder)
        self.toolchain = self.folder / "build" / "cmake" / "android.toolchain.cmake"
        if not self.toolchain.exists():
            sys.exit(f"{folder} isn't an NDK (no build/cmake/android.toolchain.cmake)")
        host = "linux-x86_64" if sys.platform.startswith("linux") else "darwin-x86_64" if sys.platform == "darwin" else "windows-x86_64"
        self.bin = self.folder / "toolchains" / "llvm" / "prebuilt" / host / "bin"
        self.clang = self.bin / f"aarch64-linux-android{API}-clang"
        self.clangxx = self.bin / f"aarch64-linux-android{API}-clang++"
        if not self.clangxx.exists():
            sys.exit(f"{self.clangxx} isn't there")

    def cmake_flags(self):
        return [f"-DCMAKE_TOOLCHAIN_FILE={self.toolchain}", f"-DANDROID_ABI={ABI}", f"-DANDROID_PLATFORM=android-{API}",
                "-DANDROID_STL=c++_static", f"-DCMAKE_ANDROID_ARCH_ABI={ABI}", "-DCMAKE_BUILD_TYPE=Release"]


def sdl_source(args):
    if args.sdl_source:
        return Path(args.sdl_source)
    archive = DEPS / f"SDL3-{SDL_VERSION}.tar.gz"
    download(SDL_URL, archive, SDL_SHA256)
    source = DEPS / f"SDL3-{SDL_VERSION}"
    if not source.exists():
        with tarfile.open(archive) as tar:
            tar.extractall(DEPS, filter="data")
    return source


def own_sdl(ndk, cmake, ninja, source):
    prefix = DEPS / f"SDL3-{SDL_VERSION}-android"
    if (prefix / "lib" / "cmake" / "SDL3" / "SDL3Config.cmake").exists():
        return prefix
    build = DEPS / f"SDL3-{SDL_VERSION}-android-build"
    run([cmake, "-S", source, "-B", build, "-G", "Ninja", f"-DCMAKE_MAKE_PROGRAM={ninja}"] + ndk.cmake_flags() +
        [f"-DCMAKE_INSTALL_PREFIX={prefix}", "-DCMAKE_INSTALL_LIBDIR=lib", "-DSDL_SHARED=ON", "-DSDL_STATIC=OFF",
         "-DSDL_TESTS=OFF", "-DSDL_EXAMPLES=OFF", "-DSDL_TEST_LIBRARY=OFF"])
    run([cmake, "--build", build, "--target", "install"])
    return prefix


def own_glslang(ndk, cmake, ninja):
    prefix = DEPS / f"glslang-{GLSLANG_VERSION}-android"
    if (prefix / "lib" / "cmake" / "glslang" / "glslang-config.cmake").exists():
        return prefix
    archive = DEPS / f"glslang-{GLSLANG_VERSION}.tar.gz"
    download(GLSLANG_URL, archive, GLSLANG_SHA256)
    source = DEPS / f"glslang-{GLSLANG_VERSION}"
    if not source.exists():
        with tarfile.open(archive) as tar:
            tar.extractall(DEPS, filter="data")
    build = DEPS / f"glslang-{GLSLANG_VERSION}-android-build"
    run([cmake, "-S", source, "-B", build, "-G", "Ninja", f"-DCMAKE_MAKE_PROGRAM={ninja}"] + ndk.cmake_flags() +
        [f"-DCMAKE_INSTALL_PREFIX={prefix}", "-DCMAKE_INSTALL_LIBDIR=lib", "-DBUILD_SHARED_LIBS=OFF", "-DENABLE_OPT=OFF",
         "-DBUILD_EXTERNAL=OFF", "-DGLSLANG_TESTS=OFF", "-DENABLE_GLSLANG_BINARIES=OFF", "-DENABLE_SPVREMAPPER=OFF",
         "-DENABLE_HLSL=OFF", "-DENABLE_PCH=OFF", "-DCMAKE_POSITION_INDEPENDENT_CODE=ON", "-DGLSLANG_ENABLE_INSTALL=ON"])
    run([cmake, "--build", build, "--target", "install"])
    return prefix


def own_ffmpeg(ndk, args):
    """The decode-only LGPL FFmpeg (native/tools/build_ffmpeg.sh's configuration) cross-compiled for Android into
    build/native/ffmpeg/android-arm64, where native/CMakeLists.txt looks"""
    prefix = HERE / "build" / "native" / "ffmpeg" / "android-arm64"
    if (prefix / "lib" / "pkgconfig" / "libavcodec.pc").exists():
        return prefix
    source = Path(args.ffmpeg_source) if args.ffmpeg_source else HERE / "native" / "third_party" / "ffmpeg-src" / f"ffmpeg-{FFMPEG_VERSION}"
    if not (source / "configure").exists():
        sys.exit(f"FFmpeg's source isn't in {source}: native/tools/build_ffmpeg.sh downloads it, or give --ffmpeg-source")
    build = HERE / "build" / "native" / "ffmpeg" / "build-android-arm64"
    shutil.rmtree(build, ignore_errors=True)
    build.mkdir(parents=True)
    configure = [source / "configure", f"--prefix={prefix}", "--disable-everything", "--disable-gpl", "--disable-nonfree",
                 "--disable-version3", "--disable-programs", "--disable-doc", "--disable-network", "--disable-autodetect",
                 "--disable-avdevice", "--disable-avformat", "--disable-avfilter", "--disable-swresample", "--disable-swscale",
                 "--enable-decoder=mpeg2video", "--enable-parser=mpegvideo", "--enable-shared", "--disable-static",
                 "--enable-pic", "--enable-pthreads", "--enable-cross-compile", "--target-os=android", "--arch=aarch64",
                 f"--cc={ndk.clang}", f"--cxx={ndk.clangxx}", f"--ar={ndk.bin / 'llvm-ar'}", f"--nm={ndk.bin / 'llvm-nm'}",
                 f"--ranlib={ndk.bin / 'llvm-ranlib'}", f"--strip={ndk.bin / 'llvm-strip'}", "--enable-asm", "--enable-neon"]
    run(configure, cwd=build)
    jobs = os.cpu_count() or 2
    run(["make", f"-j{jobs}"], cwd=build)
    run(["make", "install"], cwd=build)
    return prefix


def build_game(ndk, cmake, ninja, sdl, glslang, args):
    build = OUT / "cmake"
    if args.clean and build.exists():
        shutil.rmtree(build)
    if args.configure or not (build / "build.ninja").exists():
        run([cmake, "-S", HERE / "native", "-B", build, "-G", "Ninja", f"-DCMAKE_MAKE_PROGRAM={ninja}"] + ndk.cmake_flags() +
            [f"-DCMAKE_PREFIX_PATH={sdl};{glslang}",
             f"-DCMAKE_FIND_ROOT_PATH={sdl};{glslang};{HERE / 'build' / 'native' / 'ffmpeg' / 'android-arm64'}",
             f"-DTWIN_TOOL_CLANG={ndk.clangxx}", f"-DPython3_EXECUTABLE={sys.executable}",
             "-DTWIN_GLES=ON", f"-DPKG_CONFIG_EXECUTABLE={find_tool('pkg-config', 'apt install pkg-config')}"])
    run([cmake, "--build", build, "--"] + args.ninja)
    lib = build / "libmain.so"
    if not lib.exists():
        sys.exit(f"{lib} wasn't built")
    return lib


def package(ndk, sdl_prefix, sdl_src, ffmpeg, lib, args):
    """The APK: the Java glue (SDL's and ours) compiled and dexed, the resources and manifest by aapt2, the libraries and the
    fonts added, aligned and signed with a debug key"""
    tools = Path(args.build_tools)
    platform = Path(args.platform)
    android_jar = platform / "android.jar"
    if not android_jar.exists():
        sys.exit(f"no android.jar in {platform}")
    for name in ("zipalign", "apksigner", "d8"):
        if not (tools / name).exists():
            sys.exit(f"no {name} in {tools} (the SDK's build-tools 34)")
    # aapt2 (the SDK's), or the old aapt (Debian's package) which packs the resources and manifest in one go
    aapt2 = (tools / "aapt2").exists()
    if not aapt2 and not (tools / "aapt").exists():
        sys.exit(f"no aapt2 (or aapt) in {tools}")
    javac = find_tool("javac", "a JDK 17 or later")
    keytool = find_tool("keytool", "a JDK")
    stage = OUT / "apk"
    shutil.rmtree(stage, ignore_errors=True)
    (stage / "classes").mkdir(parents=True)
    (stage / "dex").mkdir()
    (stage / "compiled").mkdir()
    (stage / "root" / "lib" / ABI).mkdir(parents=True)
    (stage / "root" / "assets" / "fonts").mkdir(parents=True)

    # The Java sources: SDL's activity and ours
    sources = sorted(glob.glob(str(sdl_src / "android-project" / "app" / "src" / "main" / "java" / "org" / "libsdl" / "app" / "*.java")))
    sources += sorted(glob.glob(str(HERE / "android" / "app" / "src" / "main" / "java" / "**" / "*.java"), recursive=True))
    run([javac, "-source", "8", "-target", "8", "-Xlint:-options", "-nowarn", "-bootclasspath", android_jar, "-d", stage / "classes"] + sources)
    classes = sorted(glob.glob(str(stage / "classes" / "**" / "*.class"), recursive=True))
    run([tools / "d8", "--release", "--min-api", str(API), "--lib", android_jar, "--output", stage / "dex"] + classes)

    # The resources and the manifest
    res = HERE / "android" / "app" / "src" / "main" / "res"
    manifest = HERE / "android" / "app" / "src" / "main" / "AndroidManifest.xml"
    unsigned = stage / "unsigned.apk"
    if aapt2:
        run([tools / "aapt2", "compile", "--dir", res, "-o", stage / "compiled" / "res.zip"])
        run([tools / "aapt2", "link", "-o", unsigned, "-I", android_jar, "--manifest", manifest, "--min-sdk-version", "29",
             "--target-sdk-version", "34", "--version-code", "1", "--version-name", "1.0.19", stage / "compiled" / "res.zip"])
    else:
        # (Debian's old aapt reads an older platform's android.jar: --aapt-platform)
        aapt_jar = Path(args.aapt_platform) / "android.jar" if args.aapt_platform else android_jar
        run([tools / "aapt", "package", "-f", "-M", manifest, "-S", res, "-I", aapt_jar, "-F", unsigned])

    # The libraries, the dex and the fonts into it (the libraries stored uncompressed, page-aligned by zipalign -p)
    root = stage / "root"
    shutil.copy2(lib, root / "lib" / ABI / "libmain.so")
    shutil.copy2(sdl_prefix / "lib" / "libSDL3.so", root / "lib" / ABI / "libSDL3.so")
    for name in ("libavcodec.so", "libavutil.so"):
        shutil.copy2(ffmpeg / "lib" / name, root / "lib" / ABI / name)
    strip = ndk.bin / "llvm-strip"
    for so in (root / "lib" / ABI).glob("*.so"):
        run([strip, "--strip-unneeded", so])
    shutil.copy2(stage / "dex" / "classes.dex", root / "classes.dex")
    fonts = HERE / "native" / "third_party" / "fonts"
    for name in ("LuckiestGuy-Regular.ttf", "Fredoka-SemiBold.ttf", "Fredoka-Bold.ttf", "LICENSE-LuckiestGuy.txt", "OFL-Fredoka.txt"):
        shutil.copy2(fonts / name, root / "assets" / "fonts" / name)
    with zipfile.ZipFile(unsigned, "a") as apk:
        for path in sorted(root.rglob("*")):
            if path.is_file():
                name = path.relative_to(root).as_posix()
                compress = zipfile.ZIP_STORED if name.endswith(".so") else zipfile.ZIP_DEFLATED
                apk.write(path, name, compress_type=compress)

    aligned = stage / "aligned.apk"
    run([tools / "zipalign", "-f", "-p", "4", unsigned, aligned])
    keystore = OUT / "debug.keystore"
    if not keystore.exists():
        run([keytool, "-genkeypair", "-keystore", keystore, "-storepass", "android", "-keypass", "android", "-alias", "androiddebugkey",
             "-keyalg", "RSA", "-keysize", "2048", "-validity", "10000", "-dname", "CN=Android Debug,O=Android,C=US"])
    final = OUT / "Crash-Twinsanity-Android.apk"
    run([tools / "apksigner", "sign", "--ks", keystore, "--ks-pass", "pass:android", "--key-pass", "pass:android",
         "--ks-key-alias", "androiddebugkey", "--out", final, aligned])
    print(f"built {final}", flush=True)
    return final


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--ndk", default=os.environ.get("ANDROID_NDK") or os.environ.get("ANDROID_NDK_HOME"))
    sdk = os.environ.get("ANDROID_SDK") or os.environ.get("ANDROID_HOME") or ""
    parser.add_argument("--build-tools", default=str(Path(sdk) / "build-tools" / "34.0.0") if sdk else None)
    parser.add_argument("--platform", default=str(Path(sdk) / "platforms" / "android-34") if sdk else None)
    parser.add_argument("--aapt-platform", help="the platform folder whose android.jar aapt (not aapt2) packs the resources with")
    parser.add_argument("--sdl-source", help="SDL3's source folder (else downloaded)")
    parser.add_argument("--ffmpeg-source", help="FFmpeg's source folder (else native/third_party/ffmpeg-src/ffmpeg-9.0.2)")
    parser.add_argument("--configure", action="store_true", help="configure the game's build again")
    parser.add_argument("--clean", action="store_true", help="delete the game's build first")
    parser.add_argument("--no-package", action="store_true", help="build the library only")
    parser.add_argument("ninja", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if not args.ndk:
        sys.exit("--ndk DIR (or $ANDROID_NDK): the Android NDK")
    if not args.no_package and (not args.build_tools or not args.platform):
        sys.exit("--build-tools DIR and --platform DIR (or $ANDROID_SDK): the SDK's build-tools 34 and platforms/android-34")
    if not (HERE / "asm" / "data").exists():
        sys.exit("No split: run tools/split.py first (README.md)")

    ndk = Ndk(args.ndk)
    cmake = find_tool("cmake", "apt install cmake")
    ninja = find_tool("ninja", "apt install ninja-build")
    DEPS.mkdir(parents=True, exist_ok=True)
    source = sdl_source(args)
    sdl = own_sdl(ndk, cmake, ninja, source)
    ffmpeg = own_ffmpeg(ndk, args)
    glslang = own_glslang(ndk, cmake, ninja)
    lib = build_game(ndk, cmake, ninja, sdl, glslang, args)
    if not args.no_package:
        package(ndk, sdl, source, ffmpeg, lib, args)


if __name__ == "__main__":
    main()
