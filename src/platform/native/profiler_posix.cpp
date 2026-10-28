// A sampling profiler for Android and Linux (testing, $TWIN_PROFILE=1): the threads that register themselves (the game's,
// VU1's, the render thread: PinToFastCores registers the caller) are interrupted about once a millisecond with SIGPROF and
// their stacks walked by the frame pointers (arm64: x29 chains; the game is built keeping them). After $TWIN_PROFILE_DELAY
// seconds (default 20: past the loading) it samples for $TWIN_PROFILE_SECONDS (default 20), then writes to the log, for each
// thread, the addresses its time was spent in: self (the innermost frame) and total (anywhere on the stack), as
// module+offset with their counts, which llvm-symbolizer names from the unstripped library (native/tools/profile_report.py
// --android). The Windows build has profiler_win32.cpp
#if !defined(_WIN32)
#include "native.h"

#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/ucontext.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{
constexpr u32 MostFrames = 24;
constexpr u32 MostSamples = 120000;

struct Sample
{
    pid_t thread;
    u32 count;
    uintptr_t frames[MostFrames];
};

struct Registered
{
    pid_t tid;
    std::string name;
};

std::mutex g_Lock;
std::vector<Registered> g_Threads;
Sample* g_Samples = nullptr;
std::atomic<u32> g_Next{0};
std::atomic<bool> g_Sampling{false};

// The frames walked from the interrupted thread's registers (async-signal-safe: no allocation, no locks)
void OnSignal(int, siginfo_t*, void* context)
{
    if (!g_Sampling.load(std::memory_order_relaxed))
    {
        return;
    }

    u32 index = g_Next.fetch_add(1, std::memory_order_relaxed);
    if (index >= MostSamples)
    {
        return;
    }

    Sample& sample = g_Samples[index];
    sample.thread = static_cast<pid_t>(syscall(SYS_gettid));
    sample.count = 0;
    auto* uc = static_cast<ucontext_t*>(context);
#if defined(__aarch64__)
    uintptr_t pc = uc->uc_mcontext.pc;
    uintptr_t fp = uc->uc_mcontext.regs[29];
    uintptr_t lr = uc->uc_mcontext.regs[30];
#elif defined(__x86_64__)
    uintptr_t pc = static_cast<uintptr_t>(uc->uc_mcontext.gregs[REG_RIP]);
    uintptr_t fp = static_cast<uintptr_t>(uc->uc_mcontext.gregs[REG_RBP]);
    uintptr_t lr = 0;
#else
    uintptr_t pc = 0, fp = 0, lr = 0;
#endif
    sample.frames[sample.count++] = pc;
    if (lr != 0 && sample.count < MostFrames)
    {
        sample.frames[sample.count++] = lr;
    }

    // x29 (rbp) points at the saved {previous fp, return address}; each must be above the last and 16-byte aligned
    for (u32 i = 0; i < MostFrames && sample.count < MostFrames; i++)
    {
        if (fp == 0 || (fp & 0xF) != 0)
        {
            break;
        }

        const uintptr_t* frame = reinterpret_cast<const uintptr_t*>(fp);
        uintptr_t next = frame[0];
        uintptr_t ret = frame[1];
        if (ret == 0)
        {
            break;
        }

        sample.frames[sample.count++] = ret;
        if (next <= fp || next - fp > (1u << 20))
        {
            break;
        }

        fp = next;
    }
}

std::string ModuleOffset(uintptr_t address)
{
    Dl_info info;
    if (dladdr(reinterpret_cast<void*>(address), &info) != 0 && info.dli_fname != nullptr)
    {
        const char* slash = std::strrchr(info.dli_fname, '/');
        char text[256];
        std::snprintf(text, sizeof(text), "%s+%llx", slash != nullptr ? slash + 1 : info.dli_fname,
                      static_cast<unsigned long long>(address - reinterpret_cast<uintptr_t>(info.dli_fbase)));
        return text;
    }

    char text[64];
    std::snprintf(text, sizeof(text), "?+%llx", static_cast<unsigned long long>(address));
    return text;
}

void Report()
{
    u32 total = std::min(g_Next.load(), MostSamples);
    std::vector<Registered> threads;
    {
        std::lock_guard lock(g_Lock);
        threads = g_Threads;
    }

    Native::Log("profile: %u samples", total);
    for (const Registered& thread : threads)
    {
        std::map<uintptr_t, u32> self;
        std::map<uintptr_t, u32> whole;
        u32 count = 0;
        for (u32 i = 0; i < total; i++)
        {
            const Sample& sample = g_Samples[i];
            if (sample.thread != thread.tid || sample.count == 0)
            {
                continue;
            }

            count++;
            self[sample.frames[0]]++;
            // (each address once a sample, however many times it recurs on the stack)
            uintptr_t seen[MostFrames];
            u32 seenCount = 0;
            for (u32 f = 0; f < sample.count; f++)
            {
                uintptr_t address = sample.frames[f];
                bool again = false;
                for (u32 k = 0; k < seenCount; k++)
                {
                    again = again || seen[k] == address;
                }

                if (!again)
                {
                    seen[seenCount++] = address;
                    whole[address]++;
                }
            }
        }

        Native::Log("profile: %s: %u samples", thread.name.c_str(), count);
        auto top = [&](const char* kind, const std::map<uintptr_t, u32>& counts, u32 most)
        {
            std::vector<std::pair<u32, uintptr_t>> sorted;
            for (const auto& entry : counts)
            {
                sorted.emplace_back(entry.second, entry.first);
            }

            std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
            for (u32 i = 0; i < sorted.size() && i < most; i++)
            {
                Native::Log("profile: %s %s %u %s", thread.name.c_str(), kind, sorted[i].first, ModuleOffset(sorted[i].second).c_str());
            }
        };
        top("self", self, 150);
        top("total", whole, 300);
    }
}
}

namespace Native
{
void RegisterProfiledThread(const char* name)
{
    std::lock_guard lock(g_Lock);
    g_Threads.push_back({static_cast<pid_t>(syscall(SYS_gettid)), name});
}

void StartProfiler()
{
    const char* wanted = std::getenv("TWIN_PROFILE");
    if (wanted == nullptr || wanted[0] == '0')
    {
        return;
    }

    const char* delayText = std::getenv("TWIN_PROFILE_DELAY");
    const char* secondsText = std::getenv("TWIN_PROFILE_SECONDS");
    double delay = delayText != nullptr ? std::atof(delayText) : 20.0;
    double seconds = secondsText != nullptr ? std::atof(secondsText) : 20.0;
    g_Samples = new Sample[MostSamples];
    struct sigaction action = {};
    action.sa_sigaction = OnSignal;
    action.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&action.sa_mask);
    sigaction(SIGPROF, &action, nullptr);
    std::thread([delay, seconds]
    {
        std::this_thread::sleep_for(std::chrono::duration<double>(delay));
        Log("profile: sampling for %.0f s", seconds);
        g_Sampling = true;
        auto end = std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(seconds));
        pid_t pid = getpid();
        while (std::chrono::steady_clock::now() < end)
        {
            std::vector<Registered> threads;
            {
                std::lock_guard lock(g_Lock);
                threads = g_Threads;
            }

            for (const Registered& thread : threads)
            {
                syscall(SYS_tgkill, pid, thread.tid, SIGPROF);
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        g_Sampling = false;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        Report();
    }).detach();
}
}
#endif
