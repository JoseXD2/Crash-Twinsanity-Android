// The native build's entry: its own arguments taken out, the retail data's pointers fixed up, then the game's Main as the
// PS2's entry calls it
#include "abi.h"
#include "debug.h"
#include "native.h"
#include "platform/saves.h"
#include "platform/system.h"
#include "ui/nativeui.h"

#include <SDL3/SDL.h>
#if defined(__ANDROID__)
// Android: main is SDL_main, called by SDL's activity (android/app/src/main/java/org/twinsanity/app/TwinsanityActivity.java)
#include <SDL3/SDL_main.h>
#include <android/log.h>
#include <unistd.h>

#include <fstream>
#include <string>
#include <thread>
#endif

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <io.h>
#include <timeapi.h>

#include <cstdio>
#endif

extern "C"
{
    int Main(u32 argc, char** argv);
    // build/native/data.cpp (native/tools/convert_data.py)
    void NativeApplyDataFixups();
}

namespace Native
{
void StartLowMemory();
}

namespace
{
// --selftest-lowmemory: the retail code's kinds of low accesses done once each, which must go on with the PS2's results
int SelfTestLowMemory()
{
    g_NativeLowMemory[0x10] = 0x5A;
    g_NativeLowMemory[0x18] = 0x80;
    // The addresses made at run time, as the retail code's are: a constant address is an operand with no register to move, which
    // the x86-64 emulation can't take, and the game's code has none
    volatile uiptr zero = 0;
    volatile u8* null = reinterpret_cast<volatile u8*>(zero);
    u8 byte = null[0x10];
    s8 signedByte = static_cast<s8>(null[0x18]);
    volatile u32* words = reinterpret_cast<volatile u32*>(zero + 0x40);
    words[1] = 0x12345678;
    u32 word = words[1];
    volatile u64* pair = reinterpret_cast<volatile u64*>(zero + 0x80);
    pair[0] = 1;
    pair[1] = 2;
    u64 sum = pair[0] + pair[1];
    volatile float* f = reinterpret_cast<volatile float*>(zero + 0x100);
    f[0] = 1.5f;
    float back = f[0];
    bool ok = byte == 0x5A && signedByte == -128 && word == 0x12345678 && sum == 3 && back == 1.5f;
    Native::Log("low memory self-test: %s (byte %#x, signed %d, word %#x, sum %llu, float %g)", ok ? "passed" : "FAILED", byte,
                signedByte, word, static_cast<unsigned long long>(sum), back);
    return ok ? 0 : 1;
}
}

// --selftest-saves: the game's own save manager (src/platform/ps2/saves.cpp) through the native memory card: a save made, a file
// written, found and read back, then the test's save removed
int SelfTestSaves()
{
    using namespace Platform::Saves;
    auto run = [](Operation operation)
    {
        StorageInfo info;
        for (int frames = 0; frames < 100; frames++)
        {
            if (Update(0, 0, &info) == Operation::None && GetResult(operation).succeeded != -1)
            {
                return GetResult(operation);
            }
        }
        return Result{-1, -1, -1};
    };
    StorageInfo info{};
    bool ready = Initialise("BE", "SLES-52568");
    for (int i = 0; i < 3; i++)
    {
        Update(0, 0, &info);
    }

    const char* save = "SELFTEST";
    const char* file = "TEST.DAT";
    char data[300];
    for (u32 i = 0; i < sizeof(data); i++)
    {
        data[i] = static_cast<char>(i * 7);
    }

    bool created = CreateSave(0, 0, save) && run(Operation::CreateSave).succeeded == 1;
    bool written = Write(0, 0, save, file, data, sizeof(data)) && run(Operation::Write).succeeded == 1;
    s32 size = -2;
    bool found = FindFile(0, 0, file, save, &size, nullptr) && run(Operation::FindFile).succeeded == 1;
    char back[300] = {};
    bool read = Read(0, 0, save, file, back, sizeof(back)) && run(Operation::Read).succeeded == 1;
    bool same = std::memcmp(data, back, sizeof(data)) == 0;
    std::error_code ignored;
    std::filesystem::remove_all(std::filesystem::path(Native::SettingsFolder()) / "memorycard" / "BESLES-52568SELFTEST", ignored);
    bool ok = ready && info.type == StorageMemoryCard && created && written && found && size == sizeof(data) && read && same;
    Native::Log("saves self-test: %s (card type %d, %d KB free, created %d, written %d, found %d size %d, read %d, same %d)",
                ok ? "passed" : "FAILED", info.type, info.freeKilobytes, created, written, found, size, read, same);
    return ok ? 0 : 1;
}

#if defined(_WIN32)
namespace
{
// Where the log goes on Windows: a windowed program (native/CMakeLists.txt's TWIN_WINDOWS_CONSOLE off) has no console. What
// started it may have given it its output (a pipe, a file): kept. Else the console of the terminal it was started from, else
// log.txt in the settings folder
void SetUpLog()
{
    HANDLE error = GetStdHandle(STD_ERROR_HANDLE);
    if (error != nullptr && error != INVALID_HANDLE_VALUE && GetFileType(error) != FILE_TYPE_UNKNOWN)
    {
        return;
    }

    if (AttachConsole(ATTACH_PARENT_PROCESS))
    {
        std::freopen("CONOUT$", "w", stdout);
        std::freopen("CONOUT$", "w", stderr);
    }
    else
    {
        std::error_code ignored;
        std::filesystem::create_directories(Native::SettingsFolder(), ignored);
        std::string log = Native::SettingsFolder() + "/log.txt";
        // The last run's log kept (a run that hung or crashed, started again)
        std::filesystem::rename(log, Native::SettingsFolder() + "/log.previous.txt", ignored);
        std::freopen(log.c_str(), "w", stderr);
        std::freopen(log.c_str(), "a", stdout);
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        Native::SetLogToFile(true);
    }

    // The crash report writes to the handle itself (lowmemory_win32.cpp)
    SetStdHandle(STD_ERROR_HANDLE, reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stderr))));
}
}
#endif

#if defined(__ANDROID__)
namespace
{
// Android shows nothing of stdout and stderr: both go through a pipe to logcat (tag "twinsanity"), a line at a time
void StartLogcat()
{
    static int fds[2];
    if (pipe(fds) != 0)
    {
        return;
    }

    dup2(fds[1], 1);
    dup2(fds[1], 2);
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    std::thread([]
    {
        std::string line;
        char buffer[512];
        for (;;)
        {
            ssize_t got = read(fds[0], buffer, sizeof(buffer));
            if (got <= 0)
            {
                break;
            }

            for (ssize_t i = 0; i < got; i++)
            {
                if (buffer[i] == '\n')
                {
                    __android_log_write(ANDROID_LOG_INFO, "twinsanity", line.c_str());
                    line.clear();
                }
                else
                {
                    line += buffer[i];
                }
            }
        }
    }).detach();
}
}
#endif

int main(int argc, char** argv)
{
#if defined(__ANDROID__)
    StartLogcat();
    // The frames' statistics in the log (display.cpp: a line every 60 pictures), while the port's speed on phones is found out
    setenv("TWIN_FRAME_STATS", "1", 0);
    // And the hardware renderer's counts of its downloads, readbacks and copies, with their reasons (hwgs.cpp)
    setenv("TWIN_HW_STATS", "1", 0);
#endif
    // What the log leaves out of its paths (the profile folder), before its first line
    Native::StartLogPrivacy();
    // Where the settings are (portable mode or not: portable.cpp) before anything reads or writes them, the log first
    bool headless = false;
    for (int i = 1; i < argc; i++)
    {
        headless = headless || std::string_view(argv[i]) == "--headless";
    }

    Native::StartPortable(headless);
#if defined(__ANDROID__)
    // A file gif_predecode_off.txt in the settings folder (Android/data/org.twinsanity.native/files) keeps the GIF's packets'
    // decoding on the render thread (renderthread.cpp's TWIN_GIF_PREDECODE), to compare while the port's speed on phones is
    // found out
    if (std::ifstream(Native::SettingsFolder() + "/gif_predecode_off.txt").good())
    {
        setenv("TWIN_GIF_PREDECODE", "0", 0);
    }

    // And page_write_off.txt keeps WriteImage's whole-page copies off (gs.cpp's TWIN_GS_NO_PAGE_WRITE)
    if (std::ifstream(Native::SettingsFolder() + "/page_write_off.txt").good())
    {
        setenv("TWIN_GS_NO_PAGE_WRITE", "1", 0);
    }

    // And direct_texture.txt tries textures read straight from render targets' pixels (hwgs.cpp's TWIN_HW_DIRECT_TEXTURE: off
    // by default, measured no faster on a Mali-G720)
    if (std::ifstream(Native::SettingsFolder() + "/direct_texture.txt").good())
    {
        setenv("TWIN_HW_DIRECT_TEXTURE", "1", 0);
    }
#endif
#if defined(_WIN32)
    SetUpLog();
    Native::StartProfiler();
    // The system timer at 1 ms: the threads' waits (the renderer's, the audio's, the disc's) are as coarse as it otherwise
    timeBeginPeriod(1);
#endif
    // The command line as it was given, for the log (ParseArguments takes the native build's own out)
    std::vector<char*> given(argv, argv + argc);
    Native::ParseArguments(argc, argv);
    if (Native::GetSettings().headless)
    {
        // A headless run (the tests) shows nothing: no Dock icon, no menu bar, never the active app
        SDL_SetHint(SDL_HINT_MAC_BACKGROUND_APP, "1");
    }
    Native::StartLowMemory();
    Native::StartHangWatch();
#if defined(__ANDROID__)
    Native::PinToFastCores("the game's thread");
    // The sampling profiler, while the port's speed on phones is found out: 20 s after 20 s, to the log
    setenv("TWIN_PROFILE", "1", 0);
#endif
#if !defined(_WIN32)
    Native::StartProfiler();
#endif
    for (int i = 1; i < argc; i++)
    {
        if (std::string_view(argv[i]) == "--selftest-lowmemory")
        {
            return SelfTestLowMemory();
        }
        if (std::string_view(argv[i]) == "--selftest-saves")
        {
            return SelfTestSaves();
        }
#if defined(_WIN32)
        // The calls through the retail vtables and pointers in both conventions (src/platform/native/abi_win32.cpp)
        if (std::string_view(argv[i]) == "--selftest-abi")
        {
            return Abi::SelfTest();
        }
#endif
        // --selftest-ui, --dump-texts (src/platform/native/ui/commands.cpp)
        if (int status = 0; NativeUi::RunCommand(argv[i], &status))
        {
            return status;
        }
    }

    // The port's version first (native/CMakeLists.txt's), for the bug reports' logs, then what a report asks (sessionlog.cpp): the
    // date, the system, the command line, the processor; the settings folder; the options, the resolution, the threads, the
    // variables, the mods, the texture pack, how the last session ended
    Native::Log("Crash Twinsanity %s", TWIN_PORT_VERSION);
    Native::LogSessionStart(static_cast<int>(given.size()), given.data());
    Native::LogPortableStart();
    Native::LogSessionSettings();
    Native::OpenDiscImage();
    NativeApplyDataFixups();
    // $TWIN_FIXED_TIME: the test runs' time (debug.h's g_DebugFixedTime): every frame takes exactly a frame's time and reads
    // finish when asked about, so what a frame draws doesn't depend on how fast the machine is (two builds compared frame by frame)
    if (const char* fixed = std::getenv("TWIN_FIXED_TIME"); fixed != nullptr && fixed[0] != '0')
    {
        g_DebugFixedTime = 1;
    }

    Platform::System::Exit(Main(static_cast<u32>(argc), argv));
}
