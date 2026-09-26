#if defined(__SWITCH__)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
// svcGetInfo for the boot-time memory report. Included ahead of the standard
// headers because libnx defines bare types (u64/Result) that only conflict if
// something else claims those names first.
#include <switch.h>
// Defined in hle/storage/dvd.cpp; global scope so it links.
void DVD_HLE_PrescanDisc();
// Defined in hle/storage/nand_fs.cpp.
void NAND_HLE_PrepareHostRoot();
#include <atomic>
// Set by vi.cpp/gx_copy.cpp/gx_stubs.cpp around calls that can block on the GPU.
std::atomic<const char*> g_switchHostPhase{"(none)"};
// Defined in hle/os/os_scheduler.cpp.
extern std::atomic<uint32_t> g_switchSelectCount;
extern std::atomic<uint32_t> g_switchIdleSpinCount;
extern std::atomic<uint32_t> g_switchFiberSwitchCount;
// Defined in hle/vi.cpp.
extern std::atomic<uint32_t> g_viRetraceMirror;
extern std::atomic<uint32_t> g_viPollCalls;
extern std::atomic<uint32_t> g_viPollNotDue;
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <array>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <mutex>
#include <unordered_map>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#endif

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <fcntl.h>
#include <io.h>
#include <crtdbg.h>
#include <windows.h>
#include <mmsystem.h>
#include <dbghelp.h>
#else
#include <signal.h>
#if defined(__x86_64__)
// Only the x86 POSIX fault path inspects ucontext_t to recover the page-fault
// write bit. macOS deprecates ucontext and requires _XOPEN_SOURCE just to
// include the header, while the arm64 handler does not use it at all.
#include <ucontext.h>
#endif
#include <unistd.h>
#endif

#include "abi_bridge.h"
#include "guest_flat_memory.h"
#include "gx_guest_write.h"
#include "memory.h"
#include "system_bridge.h"
#include "ppc_runtime.h"
#include "aurora_events.h"
#include "guest_os_layout.h"
#include "wii_remote_input.h"
#include "discord_presence.h"
#include "fiber_manager.h"
#include "hle_stubs.h"
#include "runtime_config.h"
#include "runtime_log.h"
#include "runtime_product.h"
#include "recomp_mod_loader.h"
#include <aurora/aurora.h>
#include <aurora/gfx.h>
#include <dolphin/gx/GXAurora.h>
#include <dolphin/vi.h>

// Defined in `runtime/src/hle/vi.cpp` (used by GX/VI HLE).
extern std::atomic_bool g_auroraFrameActive;
extern "C" int g_gxFrameCount;
extern "C" const char* DVDResolveHostPathForTest(const char* dvdPath);
bool OS_HLE_InterruptsEnabled() noexcept;

void WiinxInstallHost();  // app/wiinx_host.cpp

namespace {

// Defined below, beside the fatal-log machinery.
std::string FormatHostStackTrace(unsigned framesToSkip = 0);

void ServiceGuestTimingDuringAuroraFrameWait() {
    // Aurora can block inside FIFO drains before control returns to GX HLE, for as long as a
    // whole display period. Keep VI retraces, alarms and audio moving at wall-clock cadence here
    // while still suppressing guest rescheduling and recursive Aurora work.
    VI_HLE_ProcessRetracesDeferred(8);
    OS_HLE_ProcessAlarmsDeferred(8);
    Audio_HLE_PollDeferred();
}

#if defined(_WIN32)
int __cdecl WindowsCrtReportHook(int reportType, char* message, int* returnValue) {
    if (returnValue) {
        *returnValue = 0;
    }

    RT_LOG(RT_TAG_RUNTIME) << "CRT report type=" << reportType;
    if (message) {
        std::cerr << ": " << message;
    } else {
        std::cerr << '\n';
    }

    RT_LOG(RT_TAG_RUNTIME) << "CRT report stack:\n" << FormatHostStackTrace(1);
    std::cerr.flush();
    return TRUE;
}

void ConfigureWindowsFatalDialogBehavior() {
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportHook(WindowsCrtReportHook);
}

class WindowsTimerResolutionGuard {
public:
    WindowsTimerResolutionGuard() {
        const MMRESULT result = ::timeBeginPeriod(1);
        if (result == TIMERR_NOERROR) {
            armed_ = true;
        } else {
            RT_LOG(RT_TAG_RUNTIME) << "timeBeginPeriod(1) failed: " << result << std::endl;
        }
    }

    ~WindowsTimerResolutionGuard() {
        if (armed_) {
            ::timeEndPeriod(1);
        }
    }

    WindowsTimerResolutionGuard(const WindowsTimerResolutionGuard&) = delete;
    WindowsTimerResolutionGuard& operator=(const WindowsTimerResolutionGuard&) = delete;

private:
    bool armed_ = false;
};
#endif
} // namespace

namespace {

std::string g_lastEntryLabel;
std::atomic_bool g_auroraInitialized{false};
std::atomic_flag g_abortSignalHandled = ATOMIC_FLAG_INIT;
std::atomic_bool g_fatalErrorReported{false};
std::atomic_bool g_fatalPopupShown{false};
std::atomic<int> g_lastExitCode{0};
std::atomic_bool g_exitCodeSet{false};

struct ProcessTranscriptState {
    bool enabled = false;
    std::filesystem::path path;
    std::ofstream file;
    std::mutex fileMutex;
    std::atomic_bool initialized{false};
    int savedStdoutFd = -1;
    int savedStderrFd = -1;
    int stdoutPipeReadFd = -1;
    int stdoutPipeWriteFd = -1;
    int stderrPipeReadFd = -1;
    int stderrPipeWriteFd = -1;
    std::thread stdoutThread;
    std::thread stderrThread;
};

std::filesystem::path GetDefaultRuntimeLogDirectory() {
#if defined(__SWITCH__)
    return RuntimeConfigFile::ApplicationDataDirectory() / SwitchLayout::kLogsDirName;
#else
    return RuntimeConfigFile::ApplicationDataDirectory() / "Logs";
#endif
}

// Every entry in the Logs root - both the per-run folders written by this
// scheme and any flat .log files left over from the previous one - is removed
// once it is older than the retention window.
void PruneOldRunLogs(const std::filesystem::path& logRoot) {
    constexpr auto kRetention = std::chrono::hours(24 * 4);

    std::error_code ec;
    const auto now = std::filesystem::file_time_type::clock::now();
    for (const auto& entry : std::filesystem::directory_iterator(logRoot, ec)) {
        std::error_code entryEc;
        const auto writeTime = std::filesystem::last_write_time(entry.path(), entryEc);
        if (entryEc) {
            continue;
        }
        if (now - writeTime > kRetention) {
            std::filesystem::remove_all(entry.path(), entryEc);
        }
    }
}

// Logs/<product>_<epochSeconds>_pid<pid>/ - one folder per run. The console
// transcript and every crash artifact for the run land in here, so "zip this
// folder" is a complete diagnostic. Created lazily so even a crash before
// transcript setup still has somewhere to write.
const std::filesystem::path& GetRunLogDirectory() {
    static const std::filesystem::path runDirectory = [] {
        const std::filesystem::path logRoot = GetDefaultRuntimeLogDirectory();

        std::error_code ec;
        std::filesystem::create_directories(logRoot, ec);
        PruneOldRunLogs(logRoot);

#if defined(_WIN32)
        const unsigned long pid = ::GetCurrentProcessId();
#else
        const auto pid = static_cast<unsigned long>(::getpid());
#endif
        const auto now = std::chrono::system_clock::now();
        const auto secs = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

        std::ostringstream name;
        name << RuntimeProduct::Active().folderName
             << "_" << secs << "_pid" << pid;
        const std::filesystem::path directory = logRoot / name.str();
        std::filesystem::create_directories(directory, ec);
        return directory;
    }();
    return runDirectory;
}

ProcessTranscriptState& GetProcessTranscriptState() {
    static ProcessTranscriptState state;
    return state;
}

void WriteProcessTranscriptChunk(ProcessTranscriptState& state, const char* data, size_t size) {
    if (!state.enabled || !state.file || size == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(state.fileMutex);
    state.file.write(data, static_cast<std::streamsize>(size));
    state.file.flush();
}

void PumpTranscriptPipe(ProcessTranscriptState& state, int readFd, int mirrorFd) {
    std::array<char, 4096> buffer{};
    for (;;) {
#if defined(_WIN32)
        const int bytesRead = _read(readFd, buffer.data(), static_cast<unsigned int>(buffer.size()));
#else
        const ssize_t bytesRead = ::read(readFd, buffer.data(), buffer.size());
#endif
        if (bytesRead <= 0) {
            break;
        }

        if (mirrorFd >= 0) {
            size_t offset = 0;
            while (offset < static_cast<size_t>(bytesRead)) {
#if defined(_WIN32)
                const int written = _write(mirrorFd,
                                           buffer.data() + offset,
                                           static_cast<unsigned int>(static_cast<size_t>(bytesRead) - offset));
#else
                const ssize_t written = ::write(mirrorFd,
                                                buffer.data() + offset,
                                                static_cast<size_t>(bytesRead) - offset);
#endif
                if (written <= 0) {
                    break;
                }
                offset += static_cast<size_t>(written);
            }
        }

        WriteProcessTranscriptChunk(state, buffer.data(), static_cast<size_t>(bytesRead));
    }
}

#if defined(_WIN32)
int GetFileDescriptor(FILE* file) {
    return _fileno(file);
}

int DuplicateFileDescriptor(int fd) {
    return _dup(fd);
}

int DuplicateFileDescriptorTo(int sourceFd, int targetFd) {
    return _dup2(sourceFd, targetFd);
}

void CloseFileDescriptor(int fd) {
    _close(fd);
}
#else
int GetFileDescriptor(FILE* file) {
    return fileno(file);
}

int DuplicateFileDescriptor(int fd) {
    return ::dup(fd);
}

int DuplicateFileDescriptorTo(int sourceFd, int targetFd) {
    return ::dup2(sourceFd, targetFd);
}

void CloseFileDescriptor(int fd) {
    ::close(fd);
}
#endif

bool InstallTranscriptPipe(int& outReadFd, int& outWriteFd, int targetFd) {
#if defined(_WIN32)
    int pipeFds[2]{-1, -1};
    if (_pipe(pipeFds, 8192, _O_BINARY) != 0) {
        return false;
    }
#else
    int pipeFds[2]{-1, -1};
    if (::pipe(pipeFds) != 0) {
        return false;
    }
#endif

    outReadFd = pipeFds[0];
    outWriteFd = pipeFds[1];
    if (targetFd == GetFileDescriptor(stdout)) {
        std::fflush(stdout);
    } else if (targetFd == GetFileDescriptor(stderr)) {
        std::fflush(stderr);
    }

    if (DuplicateFileDescriptorTo(outWriteFd, targetFd) < 0) {
        CloseFileDescriptor(outReadFd);
        CloseFileDescriptor(outWriteFd);
        outReadFd = -1;
        outWriteFd = -1;
        return false;
    }
    return true;
}

#if defined(_WIN32)
void AttachParentConsoleForDiagnostics() {
    // GUI-subsystem products get no console and no bound stdout/stderr unless a parent already
    // redirected them (pipe/file: leave alone) or has a console to attach to (bind only the
    // streams still unbound). With no parent console, fall back to NUL rather than leaving
    // _fileno(stdout) == -2, which would stop InitializeProcessTranscript from redirecting into
    // the log file at all.
    const HANDLE outHandle = ::GetStdHandle(STD_OUTPUT_HANDLE);
    const HANDLE errHandle = ::GetStdHandle(STD_ERROR_HANDLE);
    const bool haveOut = outHandle != nullptr && outHandle != INVALID_HANDLE_VALUE;
    const bool haveErr = errHandle != nullptr && errHandle != INVALID_HANDLE_VALUE;
    if (haveOut && haveErr) {
        return;
    }

    const bool attached =
        ::GetConsoleWindow() != nullptr || ::AttachConsole(ATTACH_PARENT_PROCESS) != 0;
    const char* const sink = attached ? "CONOUT$" : "NUL";
    if (!haveOut) {
        (void)std::freopen(sink, "w", stdout);
    }
    if (!haveErr) {
        (void)std::freopen(sink, "w", stderr);
    }
    std::cout.clear();
    std::cerr.clear();
}
#else
void AttachParentConsoleForDiagnostics() {}
#endif

// The setup writes build-fingerprint.json beside every product executable; its SetupVersion is
// the only version identity the runtime has (products are compiled locally, so nothing is baked
// into the binary). Surface it at the top of the transcript so every attached log self-identifies.
std::string ReadInstalledSetupVersion() {
    const auto directory = RuntimeConfigFile::ExecutableDirectory();
    if (!directory) {
        return {};
    }
    std::ifstream file(*directory / "build-fingerprint.json", std::ios::binary);
    if (!file) {
        return {};
    }
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    constexpr std::string_view kKey = "\"SetupVersion\"";
    const auto keyPos = text.find(kKey);
    if (keyPos == std::string::npos) {
        return {};
    }
    const auto colon = text.find(':', keyPos + kKey.size());
    const auto open = colon == std::string::npos ? std::string::npos : text.find('"', colon + 1);
    const auto close = open == std::string::npos ? std::string::npos : text.find('"', open + 1);
    if (close == std::string::npos) {
        return {};
    }
    return text.substr(open + 1, close - open - 1);
}

void InitializeProcessTranscript(int argc, char** argv) {
    auto& state = GetProcessTranscriptState();
    if (state.initialized.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    const std::filesystem::path path = GetRunLogDirectory() / "console.log";
    state.file.open(path, std::ios::out | std::ios::trunc | std::ios::binary);
    if (!state.file) {
        return;
    }

    state.enabled = true;
    state.path = path;

#if defined(_WIN32)
    const unsigned long pid = ::GetCurrentProcessId();
#else
    const auto pid = static_cast<unsigned long>(::getpid());
#endif

    const std::string setupVersion = ReadInstalledSetupVersion();

    {
        std::lock_guard<std::mutex> lock(state.fileMutex);
        state.file << "[runtime] WiiCompiled "
                   << (setupVersion.empty() ? "version unknown" : setupVersion) << "\n";
        state.file << "[runtime] process transcript started\n";
        state.file << "[runtime] pid=" << pid << "\n";
        state.file << "[runtime] argv=";
        for (int i = 0; i < argc; ++i) {
            if (i != 0) {
                state.file << ' ';
            }
            state.file << argv[i];
        }
        state.file << "\n";
        state.file.flush();
    }

#if defined(__SWITCH__)
    // Horizon has no pipe(), so the capture pipeline below can never be built and
    // it used to bail out here, leaving console.log with only the banner and
    // every later log line going nowhere. Point stdout/stderr straight at the
    // log instead. The ofstream is closed first so two writers never share the
    // file; line buffering keeps lines on the card if the process dies.
    state.file.close();
    state.enabled = false;
    {
        // Only stderr is redirected: every RT_LOG/aurora line goes through it,
        // while stdout stays attached to libnx's console so boot progress can
        // be drawn on screen (see SwitchConsoleBegin). Redirecting both is what
        // previously made the console impossible.
        const std::string sinkPath = path.string();
        std::fflush(stderr);
        if (std::freopen(sinkPath.c_str(), "a", stderr) != nullptr) {
            std::setvbuf(stderr, nullptr, _IOLBF, 0);
        }
        std::cerr.clear();
    }
    return;
#endif

    state.savedStdoutFd = DuplicateFileDescriptor(GetFileDescriptor(stdout));
    state.savedStderrFd = DuplicateFileDescriptor(GetFileDescriptor(stderr));

    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    auto restoreFailedSetup = [&state]() {
        if (state.savedStdoutFd >= 0) {
            DuplicateFileDescriptorTo(state.savedStdoutFd, GetFileDescriptor(stdout));
        }
        if (state.savedStderrFd >= 0) {
            DuplicateFileDescriptorTo(state.savedStderrFd, GetFileDescriptor(stderr));
        }
        if (state.stdoutPipeReadFd >= 0) {
            CloseFileDescriptor(state.stdoutPipeReadFd);
            state.stdoutPipeReadFd = -1;
        }
        if (state.stdoutPipeWriteFd >= 0) {
            CloseFileDescriptor(state.stdoutPipeWriteFd);
            state.stdoutPipeWriteFd = -1;
        }
        if (state.stderrPipeReadFd >= 0) {
            CloseFileDescriptor(state.stderrPipeReadFd);
            state.stderrPipeReadFd = -1;
        }
        if (state.stderrPipeWriteFd >= 0) {
            CloseFileDescriptor(state.stderrPipeWriteFd);
            state.stderrPipeWriteFd = -1;
        }
        if (state.savedStdoutFd >= 0) {
            CloseFileDescriptor(state.savedStdoutFd);
            state.savedStdoutFd = -1;
        }
        if (state.savedStderrFd >= 0) {
            CloseFileDescriptor(state.savedStderrFd);
            state.savedStderrFd = -1;
        }
        state.enabled = false;
        state.file.close();
    };

    if (!InstallTranscriptPipe(state.stdoutPipeReadFd, state.stdoutPipeWriteFd, GetFileDescriptor(stdout)) ||
        !InstallTranscriptPipe(state.stderrPipeReadFd, state.stderrPipeWriteFd, GetFileDescriptor(stderr))) {
        restoreFailedSetup();
        return;
    }

    state.stdoutThread = std::thread([&state]() {
        PumpTranscriptPipe(state, state.stdoutPipeReadFd, state.savedStdoutFd);
    });
    state.stderrThread = std::thread([&state]() {
        PumpTranscriptPipe(state, state.stderrPipeReadFd, state.savedStderrFd);
    });
}

void ShutdownProcessTranscript() {
    auto& state = GetProcessTranscriptState();
    if (!state.enabled) {
        return;
    }

    std::fflush(stdout);
    std::fflush(stderr);
    std::cout.flush();
    std::cerr.flush();

    if (state.savedStdoutFd >= 0) {
        DuplicateFileDescriptorTo(state.savedStdoutFd, GetFileDescriptor(stdout));
    }
    if (state.savedStderrFd >= 0) {
        DuplicateFileDescriptorTo(state.savedStderrFd, GetFileDescriptor(stderr));
    }

    if (state.stdoutPipeWriteFd >= 0) {
        CloseFileDescriptor(state.stdoutPipeWriteFd);
        state.stdoutPipeWriteFd = -1;
    }
    if (state.stderrPipeWriteFd >= 0) {
        CloseFileDescriptor(state.stderrPipeWriteFd);
        state.stderrPipeWriteFd = -1;
    }

    if (state.stdoutThread.joinable()) {
        state.stdoutThread.join();
    }
    if (state.stderrThread.joinable()) {
        state.stderrThread.join();
    }

    if (state.stdoutPipeReadFd >= 0) {
        CloseFileDescriptor(state.stdoutPipeReadFd);
        state.stdoutPipeReadFd = -1;
    }
    if (state.stderrPipeReadFd >= 0) {
        CloseFileDescriptor(state.stderrPipeReadFd);
        state.stderrPipeReadFd = -1;
    }
    if (state.savedStdoutFd >= 0) {
        CloseFileDescriptor(state.savedStdoutFd);
        state.savedStdoutFd = -1;
    }
    if (state.savedStderrFd >= 0) {
        CloseFileDescriptor(state.savedStderrFd);
        state.savedStderrFd = -1;
    }

    {
        std::lock_guard<std::mutex> lock(state.fileMutex);
        state.file << "\n[runtime] process transcript ended\n";
        state.file.flush();
    }
    state.file.close();
    state.enabled = false;
}

// The one host stack walker. Every caller - the CRT report hook, the fatal log
// and the stderr crash dump - goes through this, so the log and the console see
// exactly the same frames, including the TranslatedFunctionRegistry fallback for
// addresses DbgHelp cannot name.
std::string FormatHostStackTrace(unsigned framesToSkip) {
#if defined(_WIN32)
    static std::atomic_bool s_symbolsReady{false};
    HANDLE process = GetCurrentProcess();
    if (!s_symbolsReady.load(std::memory_order_acquire)) {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
        if (SymInitialize(process, nullptr, TRUE)) {
            s_symbolsReady.store(true, std::memory_order_release);
        }
    }
    const bool symbolsReady = s_symbolsReady.load(std::memory_order_acquire);

    void* frames[64]{};
    const USHORT captured = CaptureStackBackTrace(static_cast<DWORD>(framesToSkip),
                                                  static_cast<DWORD>(std::size(frames)), frames, nullptr);
    std::ostringstream out;
    if (captured == 0) {
        out << "[runtime] host stack trace unavailable (CaptureStackBackTrace returned 0)\n";
        return out.str();
    }
    out << "[runtime] host stack trace (most recent call first):\n";
    for (USHORT i = 0; i < captured; ++i) {
        const DWORD64 addr = reinterpret_cast<DWORD64>(frames[i]);
        HMODULE module = nullptr;
        std::string modulePath = "?";
        DWORD64 moduleBase = 0;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(frames[i]),
                               &module) != 0 &&
            module != nullptr) {
            moduleBase = reinterpret_cast<DWORD64>(module);
            std::wstring modulePathBuffer(MAX_PATH, L'\0');
            for (;;) {
                const DWORD length =
                    GetModuleFileNameW(module, modulePathBuffer.data(), static_cast<DWORD>(modulePathBuffer.size()));
                if (length == 0) {
                    break;
                }
                if (length < modulePathBuffer.size()) {
                    modulePathBuffer.resize(length);
                    modulePath = RuntimeConfigFile::PathToUtf8(std::filesystem::path(modulePathBuffer));
                    break;
                }
                // Truncated; retry with a larger buffer up to the extended path limit.
                if (modulePathBuffer.size() >= 32768) {
                    break;
                }
                modulePathBuffer.resize(modulePathBuffer.size() * 2);
            }
        }

        const char* symbolName = "?";
        std::string translatedFuncName;
        uint32_t ppcAddress = 0;
        DWORD64 symbolDisp = 0;
        std::array<char, sizeof(SYMBOL_INFO) + MAX_SYM_NAME> symbolBuffer{};
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(symbolBuffer.data());
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = MAX_SYM_NAME;
        if (symbolsReady && SymFromAddr(process, addr, &symbolDisp, symbol)) {
            symbolName = symbol->Name;
        } else if (auto info = TranslatedFunctionRegistry::FindByHostAddress(static_cast<uintptr_t>(addr))) {
            // DbgHelp could not name it; the translated-function registry can.
            translatedFuncName = info->name;
            ppcAddress = info->address;
            if (!translatedFuncName.empty()) {
                symbolName = translatedFuncName.c_str();
                symbolDisp = addr - reinterpret_cast<DWORD64>(info->entryPoint);
            }
        }

        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof(line);
        DWORD lineDisp = 0;
        const char* fileName = nullptr;
        DWORD lineNumber = 0;
        if (symbolsReady && SymGetLineFromAddr64(process, addr, &lineDisp, &line)) {
            fileName = line.FileName;
            lineNumber = line.LineNumber;
        }

        out << "  ["
            << std::setw(2) << std::setfill('0') << static_cast<unsigned>(i)
            << std::setfill(' ') << "] "
            << modulePath << "!" << symbolName
            << " + 0x" << std::hex << std::uppercase << symbolDisp
            << " (0x" << addr;
        if (moduleBase != 0) {
            out << ", module+0x" << (addr - moduleBase);
        }
        if (ppcAddress != 0) {
            out << ", PPC:0x" << std::setw(8) << std::setfill('0') << ppcAddress << std::setfill(' ');
        }
        out << std::dec << std::nouppercase;
        if (fileName) {
            out << ", " << fileName << ":" << lineNumber;
        }
        out << ")\n";
    }
    return out.str();
#else
    (void)framesToSkip;
    return {};
#endif
}

void WriteFatalLogImpl(std::string_view reason, std::string_view extraDetails = {},
                       const uint32_t* missingGuestTarget = nullptr) {
    const std::filesystem::path runDirectory = GetRunLogDirectory();
    std::string fileName = "crash_";
    fileName.append(reason);
    fileName.append(".txt");
    std::ofstream out(runDirectory / fileName, std::ios::out | std::ios::trunc);
    if (!out) {
        return;
    }

    const auto now = std::chrono::system_clock::now();
    const auto nowSecs = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

    out << "[runtime] fatal log" << std::endl;
    out << "[runtime] reason: " << reason << std::endl;
    out << "[runtime] timestamp(seconds): " << nowSecs << std::endl;
    out << "[runtime] entry: " << (g_lastEntryLabel.empty() ? "<unknown>" : g_lastEntryLabel) << std::endl;
    if (!extraDetails.empty()) {
        out << "[runtime] details: " << extraDetails << std::endl;
    }
    if (missingGuestTarget) {
        out << "[runtime] guest jump target: 0x" << std::hex << std::uppercase
            << *missingGuestTarget << std::dec << std::endl;
    }

    const CpuContext* cpu = TryGetCpuContext();
    if (!cpu) {
        // Exceptions can unwind CpuContextScope before we get here.
        // Fall back to the persistent CPU context snapshot so fatal logs still include registers.
        cpu = &GetPersistentCpuContext();
    }
    if (cpu) {
        SystemBridge::DumpCrashHeuristics(out, cpu, missingGuestTarget);
        SystemBridge::DumpCpuState(out, cpu);
    } else {
        out << "[runtime] CPU context unavailable." << std::endl;
    }

    // Guest memory snapshots so the heap/object state can be walked offline.
    // Written once per process: several fatal paths can fire in sequence
    // (e.g. an exception followed by the exit-code report) and the snapshots
    // are large.
    static std::atomic_bool s_memorySnapshotWritten{false};
    if (!s_memorySnapshotWritten.exchange(true, std::memory_order_acq_rel)) {
        SystemBridge::WriteGuestMemorySnapshot(out, runDirectory / "mem1.bin");
    }

    out.flush();
    RT_LOG(RT_TAG_RUNTIME) << "crash artifacts written to "
                           << RuntimeConfigFile::PathToUtf8(runDirectory) << std::endl;
}

void SetRuntimeExitCodeImpl(int code) {
    g_lastExitCode.store(code, std::memory_order_relaxed);
    g_exitCodeSet.store(true, std::memory_order_relaxed);
}

} // namespace

namespace {

void DumpHostStackTrace() {
#if defined(_WIN32)
    static std::atomic_flag s_inProgress = ATOMIC_FLAG_INIT;
    if (s_inProgress.test_and_set()) {
        return;
    }
    const std::string trace = FormatHostStackTrace(1);
    std::fputs(trace.c_str(), stderr);
    std::fflush(stderr);
    s_inProgress.clear();
#else
    RT_LOGF(RT_TAG_RUNTIME, "Host stack trace unavailable on this platform\n");
    std::fflush(stderr);
#endif
}

} // namespace

extern "C" void DumpHostStackTraceForRuntimeHelper() {
    DumpHostStackTrace();
}

void MarkFatalErrorReported() {
    g_fatalErrorReported.store(true, std::memory_order_release);
}

void ShowRuntimeFatalPopup(std::string_view category, std::string_view details) noexcept {
    if (g_fatalPopupShown.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    try {
        std::string message;
        message.reserve(category.size() + details.size() + 220);
        message.append("The game stopped because ");
        message.append(category.empty() ? "a fatal error occurred." : category);
        message.append(".\n\n");
        if (details.empty()) {
            message.append("No additional details were available.");
        } else {
            constexpr size_t kMaxPopupDetails = 4096;
            message.append(details.data(), std::min(details.size(), kMaxPopupDetails));
            if (details.size() > kMaxPopupDetails) {
                message.append("\n\n[Additional details were written to the crash log.]");
            }
        }
        message.append("\n\nSee the WiiCompiled Logs folder for the full diagnostic.");
#if defined(_WIN32)
        ::MessageBoxA(nullptr, message.c_str(), "WiiCompiled - Fatal Error",
                      MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TASKMODAL);
#else
        // The shipped product is Windows-first. Keep non-Windows builds safe
        // and retain the console diagnostic when no native dialog is available.
        RT_LOGF(RT_TAG_RUNTIME, "fatal dialog: %s\n", message.c_str());
#endif
    } catch (...) {
        // Reporting a crash must never throw or mask the original failure.
    }
}

namespace RuntimeCrash {

void WriteCrashArtifacts(std::string_view reason, std::string_view extraDetails,
                         const uint32_t* missingGuestTarget) noexcept {
    try {
        WriteFatalLogImpl(reason, extraDetails, missingGuestTarget);
    } catch (...) {
        // Crash reporting must never mask the original failure.
    }
}

[[noreturn]] void FatalMissingGuestTarget(uint32_t target, CpuContext* cpu) noexcept {
    RT_LOG(RT_TAG_RUNTIME) << "InvokeIndirectCpu: target 0x" << std::hex << target
              << " not translated (missing function)" << std::dec << std::endl;
    RT_LOG(RT_TAG_RUNTIME) << "Caller LR = 0x" << std::hex << (cpu ? cpu->lr : 0u)
              << std::dec << std::endl;
    try {
        SystemBridge::DumpCrashHeuristics(std::cerr, cpu, &target);
        SystemBridge::DumpCpuState(cpu);
    } catch (...) {
    }
    std::fflush(stderr);

    std::ostringstream message;
    message << "The game stopped because it tried to execute guest address 0x"
            << std::hex << target
            << ", but that function was not translated or registered.\n\n"
            << "Caller LR: 0x" << (cpu ? cpu->lr : 0u);
    if (target == 0) {
        message << "\n\nA jump to address 0 usually means a virtual call through a bad "
                   "object pointer; the crash log heuristics have details.";
    }
    WriteCrashArtifacts("missing_target", message.str(), &target);
    ShowRuntimeFatalPopup("Missing translated function", message.str());
    MarkFatalErrorReported();
    std::exit(EXIT_FAILURE);
}

} // namespace RuntimeCrash


namespace {

#if defined(__SWITCH__)
// Horizon publishes a file's size only when it is closed, and this build dies
// abruptly without unwinding, so anything written through stdio is still on the
// card but reads as a stale size over FTP - which is how a run that reached
// OSReport looked like a 150-byte banner. Reopening per line is wasteful, but
// these are boot milestones and renderer errors, not hot paths, and it is the
// only form of logging that has actually survived a termination here.
// Total/used/available process memory. An OS kill for memory exhaustion leaves
// no crash report, which matches what this build does (~3 s, silent, and even
// the sys-ftpd sysmodule goes unresponsive), so every milestone carries the
// numbers to confirm or kill that theory.
std::string SwitchMemorySummary() noexcept {
    u64 total = 0;
    u64 used = 0;
    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    const u64 freeBytes = total > used ? total - used : 0;
    char buffer[96];
    std::snprintf(buffer, sizeof(buffer), " [mem used=%lluMB/%lluMB free=%lluMB]",
                  static_cast<unsigned long long>(used / (1024 * 1024)),
                  static_cast<unsigned long long>(total / (1024 * 1024)),
                  static_cast<unsigned long long>(freeBytes / (1024 * 1024)));
    return buffer;
}

std::atomic<bool> g_switchConsoleActive{false};

// libnx's console owns the default framebuffer, which Aurora needs for its
// Vulkan surface, so this is strictly a pre-Aurora affordance: it gives the
// player visible progress during the several seconds of boot instead of a
// black screen, and is torn down before aurora_initialize.
// libnx's default console is 80x45 at 1280x720. Status sits on row 42:
// bottom-centred, the way a game shows loading text.
constexpr int kConsoleColumns = 80;
constexpr int kConsoleStatusRow = 42;

// The same line the launcher shows, in the same place, so a title and the
// launcher that started it read as one thing. It used to name the library
// LIBWII-NX, which has not been its name since the three-library split.
constexpr const char* kBootFooter = "GITHUB | NX-MOD | WII-NX";

std::streambuf* g_savedCoutBuffer = nullptr;

void SwitchConsoleBegin() noexcept {
    consoleInit(nullptr);
    // stdout now drives the screen, but generated code (the data-section
    // loader) and parts of the runtime print through std::cout. Point that
    // stream at stderr, which goes to the log, so only the status line below
    // (written with printf, which bypasses the swapped buffer) reaches the
    // screen. Restored in SwitchConsoleEnd.
    g_savedCoutBuffer = std::cout.rdbuf(std::cerr.rdbuf());
    g_switchConsoleActive.store(true, std::memory_order_release);
    std::printf("\x1b[2J\x1b[H");  // clear to black, cursor home
    consoleUpdate(nullptr);
}

// One line, rewritten in place, so boot reads as a loading screen rather than
// a scrolling log. Internal milestones stay in boot.log. The percentage is per
// boot stage, not measured - it cannot animate without a thread, and a thread
// is what exhausted memory and killed boot earlier.
void SwitchConsoleStatus(const char* status) noexcept;

void SwitchConsoleProgress(int percent) noexcept {
    // Superseded by SwitchLoadStage/SwitchLoadAdvance below; kept as a no-op
    // so older call sites compile while the stage table is calibrated.
    (void)percent;
}

// Loading bar: "LOADING ... WORD NN%". Each stage owns a slice of 0-100 sized
// by its measured share of boot time (see kLoadStages), and advances inside
// that slice on real work events, so the number only moves when work does.
struct LoadStage {
    const char* word;
    int startPercent;
    int endPercent;
};

// Measured, not guessed (boot.log, calibration run): the console is visible
// from 119ms to 644ms. CONFIG 119-189ms (13%), SYSTEM 189-644ms (82%, guest OS
// init plus registry finalize), then GRAPHICS hands the display to Aurora.
constexpr LoadStage kLoadStages[] = {
    {"WII SYSTEM", 0, 13},
    {"WII SYSTEM", 13, 95},
    {"VULKAN", 95, 100},
};

std::atomic<int> g_loadStage{-1};
std::atomic<int> g_loadPercentShown{-1};

void SwitchLoadRender(int stage, float fraction) noexcept {
    if (stage < 0 || stage >= static_cast<int>(std::size(kLoadStages))) {
        return;
    }
    if (fraction < 0.f) {
        fraction = 0.f;
    }
    if (fraction > 1.f) {
        fraction = 1.f;
    }
    const LoadStage& entry = kLoadStages[stage];
    int percent = entry.startPercent +
                  static_cast<int>((entry.endPercent - entry.startPercent) * fraction);
    // Never let the number go backwards.
    const int shown = g_loadPercentShown.load(std::memory_order_relaxed);
    if (percent < shown) {
        percent = shown;
    }
    g_loadPercentShown.store(percent, std::memory_order_relaxed);
    // Project banner rather than a progress word; the stage bookkeeping above is
    // kept so a progress indicator can come back later.
    SwitchConsoleStatus(kBootFooter);
}

void SwitchLoadStage(int stage) noexcept {
    g_loadStage.store(stage, std::memory_order_release);
    SwitchLoadRender(stage, 0.f);
}

void SwitchLoadAdvance(float fraction) noexcept {
    SwitchLoadRender(g_loadStage.load(std::memory_order_acquire), fraction);
}

void SwitchConsoleStatus(const char* status) noexcept {
    if (!g_switchConsoleActive.load(std::memory_order_acquire) || status == nullptr) {
        return;
    }
    const int length = static_cast<int>(std::strlen(status));
    int column = (kConsoleColumns - length) / 2 + 1;
    if (column < 1) {
        column = 1;
    }
    std::printf("\x1b[%d;1H\x1b[2K", kConsoleStatusRow);  // clear the status row
    std::printf("\x1b[%d;%dH%s", kConsoleStatusRow, column, status);
    consoleUpdate(nullptr);
}

void SwitchConsoleEnd() noexcept {
    if (!g_switchConsoleActive.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    consoleUpdate(nullptr);
    consoleExit(nullptr);
    if (g_savedCoutBuffer != nullptr) {
        std::cout.rdbuf(g_savedCoutBuffer);
        g_savedCoutBuffer = nullptr;
    }
    // consoleExit tears down the device stdout was bound to; any later stdout
    // write would dereference it and crash. Rebind stdout to a real file.
    std::fflush(stdout);
    if (std::freopen(WIINX_GAME_PATH("logs/stdout.log"), "w", stdout) != nullptr) {
        std::setvbuf(stdout, nullptr, _IOLBF, 0);
    }
    std::cout.clear();
}

const std::chrono::steady_clock::time_point g_switchBootStart = std::chrono::steady_clock::now();

// Network log. boot.log cannot survive a whole-OS hang: the SD card's
// filesystem belongs to an OS service, and a hard reboot drops whatever it had
// not flushed (a run that froze Horizon left no trace at all). Each line is
// also sent by UDP to the host named in sdmc:/wii-nx/config/loghost.txt; any
// datagram sent before a freeze has already left the console. No file, no
// sockets - a normal launch is untouched.
int g_netLogSocket = -1;
sockaddr_in g_netLogAddr{};
constexpr uint16_t kNetLogPort = 5555;
// Why the UDP log is not running, when a loghost.txt is there and it is not.
// Every way this can fail used to be a bare `return`, so writing "ip:port" into
// the file - which is how the listener is described everywhere else - disabled
// the only log that survives a hang, and said nothing. The file was there, it
// was readable, and the log simply never came.
std::string g_netLogDiagnostic;

void SwitchNetLogInit() noexcept {
    FILE* hostFile = std::fopen(WIINX_CONFIG_PATH("loghost.txt"), "r");
    if (hostFile == nullptr) {
        return;  // No file, no sockets: a normal launch is untouched.
    }
    char host[64] = {};
    const bool haveHost = std::fgets(host, sizeof(host), hostFile) != nullptr;
    std::fclose(hostFile);
    if (!haveHost) {
        g_netLogDiagnostic = "loghost.txt is empty";
        return;
    }
    for (char* cursor = host; *cursor != '\0'; ++cursor) {
        if (*cursor == '\n' || *cursor == '\r' || *cursor == ' ' || *cursor == '\t') {
            *cursor = '\0';
            break;
        }
    }
    // "1.2.3.4" or "1.2.3.4:5555". The port is this port either way, but a file
    // that names it is the obvious thing to write, so read it rather than
    // failing inet_pton on the colon.
    uint16_t port = kNetLogPort;
    if (char* colon = std::strrchr(host, ':'); colon != nullptr) {
        *colon = '\0';
        const long parsed = std::strtol(colon + 1, nullptr, 10);
        if (parsed > 0 && parsed <= 65535) {
            port = static_cast<uint16_t>(parsed);
        } else {
            g_netLogDiagnostic = std::string("loghost.txt names port '") + (colon + 1) + "', which is not a port";
            return;
        }
    }
    g_netLogAddr.sin_family = AF_INET;
    g_netLogAddr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &g_netLogAddr.sin_addr) != 1) {
        g_netLogDiagnostic = std::string("loghost.txt names '") + host + "', which is not an IPv4 address";
        return;
    }
    if (R_FAILED(socketInitializeDefault())) {
        g_netLogDiagnostic = "the console's sockets would not start";
        return;
    }
    g_netLogSocket = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_netLogSocket < 0) {
        g_netLogDiagnostic = "a UDP socket would not open";
    }
}

void SwitchDurableLog(std::string_view text) noexcept {
    static std::mutex durableMutex;
    try {
        std::lock_guard<std::mutex> lock(durableMutex);
        // Timestamps are what calibrate the loading bar: real stage
        // durations, rather than guessed weights that jump.
        const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - g_switchBootStart)
                                 .count();
        char stamp[24];
        std::snprintf(stamp, sizeof(stamp), "%7lldms ", ms);
        std::string line = stamp;
        line.append(text.data(), text.size());
        line += SwitchMemorySummary();
        line += '\n';

        if (g_netLogSocket >= 0) {
            sendto(g_netLogSocket, line.data(), line.size(), 0,
                   reinterpret_cast<const sockaddr*>(&g_netLogAddr), sizeof(g_netLogAddr));
        }
        if (FILE* file = std::fopen(WIINX_GAME_PATH("logs/boot.log"), "a")) {
            std::fwrite(line.data(), 1, line.size(), file);
            std::fclose(file);
        }
    } catch (...) {
    }
}
#endif

void RuntimeAuroraLogCallback(AuroraLogLevel level, const char* module,
                              const char* message, unsigned int len) {
    const std::string_view moduleView = module != nullptr ? std::string_view(module) : std::string_view{};
    const std::string_view messageView = message != nullptr ? std::string_view(message, len) : std::string_view{};
    std::cerr << "[aurora] [" << static_cast<int>(level) << "] [" << moduleView << "] "
              << messageView << std::endl;
#if defined(__SWITCH__)
    {
        std::string durable = "[aurora][";
        durable += std::to_string(static_cast<int>(level));
        durable += "][";
        durable.append(moduleView);
        durable += "] ";
        durable.append(messageView);
        SwitchDurableLog(durable);
    }
#endif
    if (level == LOG_FATAL) {
        ShowRuntimeFatalPopup("Aurora reported a fatal renderer error", messageView);
    }
}

const TranslatedFunctionInfo* ResolveEntry() {
    const auto* entry = TranslatedFunctionRegistry::FindByAddressPtr(kDefaultEntryAddress);
    if (!entry) {
        std::ostringstream oss;
        oss << "No translated function registered at address 0x" << std::hex << kDefaultEntryAddress;
        throw std::runtime_error(oss.str());
    }
    return entry;
}

void SeedCpuContext(CpuContext& cpu) {
    cpu.gpr[1] = 0x81700000u;
}

void DumpAccessViolationReport(const Memory::AccessViolation& ex,
                               std::string_view entryLabel) {
    const uint32_t address = ex.address();
    const size_t length = ex.length();
    const CpuContext* cpu = TryGetCpuContext();

    RT_LOG(RT_TAG_RUNTIME) << "===== Memory Access Violation =====" << std::endl;
    RT_LOG(RT_TAG_RUNTIME) << "Reason : " << ex.reason() << std::endl;
    std::cerr << std::hex << std::uppercase;
    RT_LOG(RT_TAG_RUNTIME) << "Address: 0x" << std::setw(8) << std::setfill('0') << address
              << " (+0x" << length << ")" << std::dec << std::setfill(' ') << std::endl;
    RT_LOG(RT_TAG_RUNTIME) << "Entry  : " << (entryLabel.empty() ? "(unknown)" : std::string(entryLabel)) << std::endl;
    RT_LOG(RT_TAG_RUNTIME) << "Mode   : strict (trap on unmapped)" << std::endl;
    RT_LOG(RT_TAG_RUNTIME) << "r1 seed: 0x81700000" << std::endl;
    if (cpu) {
        RT_LOG(RT_TAG_RUNTIME) << "CurrentCpuContext: " << cpu << "  r1=0x"
                  << std::hex << std::uppercase << cpu->gpr[1] << std::dec << std::nouppercase << std::endl;
        RT_LOG(RT_TAG_RUNTIME) << "Last recorded PC : 0x" << std::hex << std::uppercase << cpu->pc
                  << std::dec << std::nouppercase << std::endl;
    } else {
        RT_LOG(RT_TAG_RUNTIME) << "CurrentCpuContext: (null)" << std::endl;
    }

    SystemBridge::DumpCpuState(cpu);

    const auto regions = Memory::DescribeRegions();
    if (regions.empty()) {
        RT_LOG(RT_TAG_RUNTIME) << "Memory not initialized; no regions mapped." << std::endl;
        return;
    }

    RT_LOG(RT_TAG_RUNTIME) << "Mapped regions:" << std::endl;
    uint64_t bestDistance = std::numeric_limits<uint64_t>::max();
    std::string bestRegion;
    bool insideRegion = false;

    for (const auto& region : regions) {
        const uint64_t base = region.baseAddress;
        const uint64_t end = base + region.sizeBytes;
        const bool contains = address >= base && address < end;
        if (contains) {
            insideRegion = true;
            bestDistance = 0;
            bestRegion = region.name;
        } else {
            const uint64_t distance = address < base ? base - address : address - end + 1;
            if (distance < bestDistance) {
                bestDistance = distance;
                bestRegion = region.name;
            }
        }

        std::cerr << "  - " << region.name
                  << " 0x" << std::hex << std::setw(8) << std::setfill('0') << region.baseAddress
                  << " .. 0x" << std::setw(8) << (end - 1)
                  << std::dec << std::setfill(' ')
                  << " (" << region.sizeBytes / 1024 << " KiB";
        if (contains) {
            std::cerr << ", <-- access landed here";
        }
        std::cerr << ")" << std::endl;
    }

    if (!bestRegion.empty() && !insideRegion) {
        RT_LOG(RT_TAG_RUNTIME) << "Nearest region: " << bestRegion << " (" << bestDistance << " bytes away)" << std::endl;
    }

    RT_LOG(RT_TAG_RUNTIME) << "Verify that the installed game data and runtime build match." << std::endl;
}

#if defined(_WIN32)
PVOID g_vectoredSehHandle = nullptr;
constexpr DWORD kCppExceptionCodeGcc = 0x20474343; // "GCC" exception code
constexpr DWORD kCppExceptionCodeMsvc = 0xE06D7363;
// AddressSanitizer uses STATUS_FATAL_APP_EXIT when it detects an error and wants to report it.
// We must let ASan's handler run so it can print file/line information.
constexpr DWORD kAsanFatalAppExit = 0x40000015; // STATUS_FATAL_APP_EXIT
LONG ReportFatalSehAndExit(EXCEPTION_POINTERS* info);

void ReportStructuredException(EXCEPTION_POINTERS* info) {
    if (!info || !info->ExceptionRecord) {
        RT_LOG(RT_TAG_RUNTIME) << "Structured exception occurred, but no diagnostic info was captured." << std::endl;
        return;
    }

    const auto* record = info->ExceptionRecord;
    const auto code = record->ExceptionCode;
    std::cerr << std::hex << std::uppercase;
    RT_LOG(RT_TAG_RUNTIME) << "Structured exception 0x" << code;
    if (!g_lastEntryLabel.empty()) {
        std::cerr << " while executing " << g_lastEntryLabel;
    }
    std::cerr << std::dec << std::nouppercase << std::endl;

    const auto faultAddress = reinterpret_cast<uintptr_t>(record->ExceptionAddress);
    std::cerr << std::hex << std::uppercase;
    RT_LOG(RT_TAG_RUNTIME) << "Fault address: 0x" << faultAddress << std::dec << std::nouppercase << std::endl;

    if (code == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
        const auto accessType = record->ExceptionInformation[0];
        const auto accessed = record->ExceptionInformation[1];
        RT_LOG(RT_TAG_RUNTIME) << "Access type: " << (accessType ? "write" : "read")
                  << " at 0x" << std::hex << std::uppercase << accessed << std::dec << std::nouppercase << std::endl;

        // Guardrail: flag raw GX gather pipe touches (0xCC00_8xxx) which must be routed through HLE.
        // Direct stores to this MMIO region (e.g., translated stfs/stb -0x8000(r4) with r4=0xCC010000)
        // will fault on the host. Emit an explicit hint so we know to fix the translation/HLE path instead
        // of chasing generic access violations.
        constexpr uintptr_t kGxGatherLo = 0xCC008000;
        constexpr uintptr_t kGxGatherHi = 0xCC009000; // one page past the 0x100-byte gather range for clarity
        if (accessed >= kGxGatherLo && accessed < kGxGatherHi) {
            RT_LOG(RT_TAG_RUNTIME) << "HINT: guest attempted a direct GX gather pipe write (0xCC00_8xxx)." << std::endl;
            RT_LOG(RT_TAG_RUNTIME) << "      These must go through GX_HLE_FIFO_Write*; check the translated function for" << std::endl;
            RT_LOG(RT_TAG_RUNTIME) << "      literal stores to -0x8000(r4) after lis r4,0xCC01 and route them via HLE." << std::endl;
        }
    }

#if defined(_M_X64) || defined(__x86_64__)
    const auto rip = info->ContextRecord ? info->ContextRecord->Rip : 0;
    const auto rsp = info->ContextRecord ? info->ContextRecord->Rsp : 0;
    std::cerr << std::hex << std::uppercase;
    RT_LOG(RT_TAG_RUNTIME) << "RIP=0x" << rip << " RSP=0x" << rsp << std::dec << std::nouppercase << std::endl;
#elif defined(_M_IX86)
    const auto eip = info->ContextRecord ? info->ContextRecord->Eip : 0;
    const auto esp = info->ContextRecord ? info->ContextRecord->Esp : 0;
    std::cerr << std::hex << std::uppercase;
    RT_LOG(RT_TAG_RUNTIME) << "EIP=0x" << eip << " ESP=0x" << esp << std::dec << std::nouppercase << std::endl;
#endif

    // Always dump CPU state on crash.
    RT_LOG(RT_TAG_RUNTIME) << "===== DUMPING CPU STATE =====" << std::endl;
    SystemBridge::DumpCpuState(TryGetCpuContext());
    std::cerr.flush();

    RT_LOG(RT_TAG_RUNTIME) << "Enable /DEBUG builds or capture a dump for full stack details." << std::endl;
    std::cerr.flush();
}



LONG CALLBACK SehLogger(EXCEPTION_POINTERS* info) {
    // Guest-space faults are the flat memory interception mechanism (MMIO,
    // deferred EFB reads, the executable-write guard, unmapped pages). The
    // flat module registers its own handler first, but registration order is
    // not guaranteed once another VEH is installed later, so consult it here
    // too - resolving a fault twice is a no-op.
    if (info->ExceptionRecord != nullptr &&
        info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        info->ExceptionRecord->NumberParameters >= 2 &&
        GuestFlat::HandleAccessViolation(
            reinterpret_cast<void*>(info->ExceptionRecord->ExceptionInformation[1]),
            info->ExceptionRecord->ExceptionInformation[0] != 0)) {
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (g_suppressSehReporting && g_sehJumpTarget) {
        g_sehLastExceptionCode = info->ExceptionRecord->ExceptionCode;
        g_sehLastExceptionAddress = reinterpret_cast<uintptr_t>(info->ExceptionRecord->ExceptionAddress);
        g_sehLastAccessType = 0;
        g_sehLastAccessedAddress = 0;
        if (g_sehLastExceptionCode == EXCEPTION_ACCESS_VIOLATION && info->ExceptionRecord->NumberParameters >= 2) {
            g_sehLastAccessType = static_cast<uint32_t>(info->ExceptionRecord->ExceptionInformation[0]);
            g_sehLastAccessedAddress = static_cast<uintptr_t>(info->ExceptionRecord->ExceptionInformation[1]);
        }
        longjmp(*g_sehJumpTarget, 1);
    }
    if (g_suppressSehReporting) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    // Let C++ exceptions propagate to std::terminate so we can log their what().
    if (info->ExceptionRecord->ExceptionCode == kCppExceptionCodeGcc ||
        info->ExceptionRecord->ExceptionCode == kCppExceptionCodeMsvc) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (info->ExceptionRecord->ExceptionCode == 0x40010006 || // DBG_PRINTEXCEPTION_C
        info->ExceptionRecord->ExceptionCode == 0x4001000A || // DBG_PRINTEXCEPTION_WIDE_C (OutputDebugStringW)
        info->ExceptionRecord->ExceptionCode == 0x406D1388 || // SetThreadName
        info->ExceptionRecord->ExceptionCode == kAsanFatalAppExit) { // ASan reporting - let it print first
        return EXCEPTION_CONTINUE_SEARCH;
    }
    // Software-raised exceptions (customer bit set) are used for internal control flow by
    // system DLLs (e.g. msxml6 while mscms parses a display colour profile) and are caught
    // by their own frame handlers. Only hardware faults are fatal at first chance; anything
    // else that truly goes unhandled reaches UnhandledSehFilter.
    if ((info->ExceptionRecord->ExceptionCode & 0x20000000u) != 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    return ReportFatalSehAndExit(info);
}

LONG WINAPI UnhandledSehFilter(EXCEPTION_POINTERS* info) {
    if (info == nullptr || info->ExceptionRecord == nullptr) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code == kCppExceptionCodeGcc || code == kCppExceptionCodeMsvc || code == kAsanFatalAppExit) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    return ReportFatalSehAndExit(info);
}

LONG ReportFatalSehAndExit(EXCEPTION_POINTERS* info) {
    // Guard against re-entrancy: if we crash while reporting, don't recurse
    static std::atomic_flag s_inCrashHandler = ATOMIC_FLAG_INIT;
    if (s_inCrashHandler.test_and_set()) {
        std::_Exit(EXIT_FAILURE);
    }

    // Report the structured exception with detailed information
    ReportStructuredException(info);
    const auto* record = info->ExceptionRecord;
    const DWORD code = record != nullptr ? record->ExceptionCode : 0;
    std::ostringstream popupDetails;
    popupDetails << "A native Windows exception (0x" << std::hex << std::uppercase << code << ") occurred";
    if (!g_lastEntryLabel.empty()) {
        popupDetails << " while executing " << g_lastEntryLabel;
    }
    if (code == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
        popupDetails << ".\n\nThe game attempted a "
                     << (record->ExceptionInformation[0] ? "write" : "read")
                     << " at host address 0x" << record->ExceptionInformation[1];
    }
    popupDetails << ".\n\nThe process transcript and crash log contain the full CPU and stack diagnostics.";
    ShowRuntimeFatalPopup("a native crash occurred", popupDetails.str());
    DumpHostStackTrace();

    WriteFatalLogImpl("seh");
    
    // CRITICAL: Explicitly flush all output to ensure visibility with PowerShell redirection
    std::cerr << '\n';
    RT_LOG(RT_TAG_RUNTIME) << "===== FLUSHING OUTPUT BEFORE EXIT =====" << std::endl;
    std::cerr.flush();
    std::cout.flush();
    std::fflush(stdout);
    std::fflush(stderr);
    
    std::_Exit(EXIT_FAILURE);
}

void InstallSehLogger() {
    if (!g_vectoredSehHandle) {
        g_vectoredSehHandle = AddVectoredExceptionHandler(1, SehLogger);
        SetUnhandledExceptionFilter(UnhandledSehFilter);
    }
}
#elif defined(__SWITCH__)
// libnx/newlib has no user-space POSIX signal trampoline (no si_addr, no siglongjmp) and Switch
// has no vectored exception handler API, so fault recovery is compiled out here. The guest flat
// memory region is fully mapped up-front by virtmem, so the interception that POSIX signals
// provide on desktop is not required for the on-device path.
void InstallPosixMemoryFaultHandler() {}
#else
// POSIX counterpart to SehLogger above. Unlike Windows' AddVectoredExceptionHandler, which lets
// GuestFlat and this module each install their own handler and defensively re-check each other,
// sigaction only allows one handler per signal - the second registration replaces the first
// instead of chaining. So this is the single SIGSEGV/SIGBUS handler for the whole process, and it
// owns checking GuestFlat's fault-interception logic first, exactly mirroring the order SehLogger
// already uses on Windows.
void ReportUnhandledSignalFault(int sig, void* faultAddress) {
    RT_LOG(RT_TAG_RUNTIME) << "Signal " << sig << " (fault address 0x" << std::hex
              << reinterpret_cast<uintptr_t>(faultAddress) << std::dec << ")";
    if (!g_lastEntryLabel.empty()) {
        std::cerr << " while executing " << g_lastEntryLabel;
    }
    std::cerr << std::endl;
    if (const CpuContext* cpu = TryGetCpuContext()) {
        RT_LOG(RT_TAG_RUNTIME) << "===== DUMPING CPU STATE =====" << std::endl;
        SystemBridge::DumpCpuState(cpu);
    }
    std::cerr.flush();
}

void PosixMemoryFaultHandler(int sig, siginfo_t* info, void* ucontextVoid) {
    void* faultAddress = info != nullptr ? info->si_addr : nullptr;
    bool isWrite = false;
#if defined(__x86_64__)
    // Standard glibc technique for a POSIX fastmem-style handler: bit 1 (0x2) of the hardware
    // error code x86 pushes on a page fault records whether it was a write.
    if (ucontextVoid != nullptr) {
        auto* uc = static_cast<ucontext_t*>(ucontextVoid);
        isWrite = (uc->uc_mcontext.gregs[REG_ERR] & 0x2) != 0;
    }
#endif

    // Guest-space faults are the flat memory interception mechanism (MMIO, deferred EFB reads,
    // the executable-write guard, unmapped pages). Resolving one here means resuming the
    // faulting instruction, which just returning from the handler does.
    if (faultAddress != nullptr && GuestFlat::HandleAccessViolation(faultAddress, isWrite)) {
        return;
    }

    if (g_suppressSehReporting && g_sehJumpTarget) {
        g_sehLastExceptionCode = static_cast<uint32_t>(sig);
        g_sehLastExceptionAddress = reinterpret_cast<uintptr_t>(faultAddress);
        g_sehLastAccessType = isWrite ? 1u : 0u;
        g_sehLastAccessedAddress = reinterpret_cast<uintptr_t>(faultAddress);
        siglongjmp(*g_sehJumpTarget, 1);
    }
    if (g_suppressSehReporting) {
        // Reporting suppressed but nobody armed a recovery jump: restore the default disposition
        // and re-raise so the process still terminates, instead of returning into the same fault.
        signal(sig, SIG_DFL);
        raise(sig);
        return;
    }

    // Guard against re-entrancy: if we crash while reporting, don't recurse.
    static std::atomic_flag s_inCrashHandler = ATOMIC_FLAG_INIT;
    if (s_inCrashHandler.test_and_set()) {
        std::_Exit(EXIT_FAILURE);
    }

    ReportUnhandledSignalFault(sig, faultAddress);
    std::ostringstream popupDetails;
    popupDetails << "A native signal (" << sig << ") occurred";
    if (!g_lastEntryLabel.empty()) {
        popupDetails << " while executing " << g_lastEntryLabel;
    }
    if (faultAddress != nullptr) {
        popupDetails << ".\n\nThe game attempted a " << (isWrite ? "write" : "read")
                     << " at host address 0x" << std::hex
                     << reinterpret_cast<uintptr_t>(faultAddress) << std::dec;
    }
    popupDetails << ".\n\nThe process transcript and crash log contain the full CPU and stack "
                    "diagnostics.";
    ShowRuntimeFatalPopup("a native crash occurred", popupDetails.str());
    DumpHostStackTrace();
    WriteFatalLogImpl(sig == SIGBUS ? "sigbus" : "sigsegv");

    std::cerr.flush();
    std::cout.flush();
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(EXIT_FAILURE);
}

void InstallPosixMemoryFaultHandler() {
    struct sigaction action {};
    action.sa_sigaction = PosixMemoryFaultHandler;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    sigaction(SIGSEGV, &action, nullptr);
    // A touch beyond a memfd-backed mapping's ftruncate()'d size raises SIGBUS rather than
    // SIGSEGV on Linux; region sizing should make this unreachable, but routing it to the same
    // handler costs nothing and avoids a silent gap if it ever isn't.
    sigaction(SIGBUS, &action, nullptr);
}
#endif

void AbortSignalHandler(int signum) {
    // Guard against re-entrancy if multiple aborts are raised in quick succession
    if (g_abortSignalHandled.test_and_set()) {
        std::_Exit(EXIT_FAILURE);
    }

    // abort() bypasses atexit, so the guest-memory fault summary has to be
    // emitted here too. It is idempotent, so a later AtExitHandler is a no-op.
    GuestFlat::LogFaultSummary();

    ShowRuntimeFatalPopup("a fatal internal error occurred",
                          "The process called abort while running the game or Aurora renderer.\n\n"
                          "This usually means an unimplemented function, failed renderer assertion, "
                          "or another unrecoverable runtime condition was reached.");

    // Skip detailed dump if already reported by another handler
    if (g_fatalErrorReported.load(std::memory_order_acquire)) {
        std::fflush(stderr);
        std::fflush(stdout);
        std::_Exit(EXIT_FAILURE);
    }

    WriteFatalLogImpl("sigabrt");

    std::fflush(stderr);
    std::fflush(stdout);
    std::_Exit(EXIT_FAILURE);
}

} // namespace

// Retained as the thin public wrapper over WriteFatalLogImpl (declared in
// system_bridge.h); it has no in-tree callers because every fatal path now goes
// through RuntimeCrash::WriteCrashArtifacts.
void WriteFatalLog(std::string_view reason) {
    WriteFatalLogImpl(reason);
}

void SetRuntimeExitCode(int code) {
    SetRuntimeExitCodeImpl(code);
}

// Global handler called via atexit() to flush buffers before any exit
#if defined(__SWITCH__)
// Exposed for the VI retrace path, which is the only place that can report
// guest progress now that the heartbeat thread is gone (it ran out of memory
// for a thread stack and took boot with it).
// Developer diagnostics (per-call NAND traces, profiler reports) are only
// worth their SD writes when someone is collecting them, i.e. when
// sdmc:/wii-nx/config/loghost.txt points the UDP log at a listener.
bool SwitchDevLoggingEnabled() noexcept { return g_netLogSocket >= 0; }

void SwitchBootLogExternal(const char* text) noexcept {
    if (text != nullptr) {
        SwitchDurableLog(text);
    }
}
#endif

static void AtExitHandler() {
#if defined(__SWITCH__)
    SwitchDurableLog("[exit] AtExitHandler reached (process is terminating)");
#endif
    // End-of-run guest memory report. This runs before the fatal-report check
    // below because the counters describe the whole session and are just as
    // interesting after a crash as after a clean exit.
    GuestFlat::LogFaultSummary();

    // Skip if already reported by another handler
    if (g_fatalErrorReported.load(std::memory_order_acquire)) {
        return;
    }
    if (g_exitCodeSet.load(std::memory_order_relaxed) &&
        g_lastExitCode.load(std::memory_order_relaxed) != 0) {
        ShowRuntimeFatalPopup("the runtime exited with an error",
                              "The game stopped after reporting a fatal error. Check the process transcript and crash log for details.");
        WriteFatalLogImpl("exitcode");
    }

    std::cerr.flush();
    std::cout.flush();
    std::fflush(stdout);
    std::fflush(stderr);
}

// Global terminate handler for uncaught exceptions
static void TerminateHandler() {
#if defined(__SWITCH__)
    SwitchDurableLog("[exit] std::terminate reached");
#endif
    // Skip detailed dump if already reported
    if (g_fatalErrorReported.load(std::memory_order_acquire)) {
        std::fflush(stderr);
        std::_Exit(EXIT_FAILURE);
    }
    std::string terminateDetails;
    if (auto ex = std::current_exception()) {
        try {
            std::rethrow_exception(ex);
        } catch (const std::exception& e) {
            terminateDetails = std::string("Unhandled C++ exception: ") + e.what();
            RT_LOG(RT_TAG_RUNTIME) << "Unhandled C++ exception: " << e.what() << std::endl;
        } catch (...) {
            terminateDetails = "Unhandled non-std C++ exception.";
            RT_LOG(RT_TAG_RUNTIME) << "Unhandled non-std C++ exception." << std::endl;
        }
    } else {
        terminateDetails = "std::terminate() without current exception.";
    }
    ShowRuntimeFatalPopup("an unhandled C++ exception occurred", terminateDetails);
    const std::string hostStackSummary = FormatHostStackTrace(1);
    if (!hostStackSummary.empty()) {
        terminateDetails.append("\n");
        terminateDetails.append(hostStackSummary);
    }
    WriteFatalLogImpl("terminate", terminateDetails);
    RT_LOG(RT_TAG_RUNTIME) << "std::terminate() called - program exiting" << std::endl;
    std::cerr.flush();
    DumpHostStackTrace();
    if (const CpuContext* cpu = TryGetCpuContext()) {
        RT_LOG(RT_TAG_RUNTIME) << "CPU state at terminate:" << std::endl;
        SystemBridge::DumpCpuState(cpu);
    }
    std::fflush(stderr);
    std::_Exit(EXIT_FAILURE);
}

// Runtime entry point: loads the configuration, brings up aurora and runs the game.
#if defined(__SWITCH__)
uint32_t VI_HLE_DebugPresentCount();
uint32_t VI_HLE_DebugRetraceCount();

// Switch has no debugger and no console, so a stuck game is otherwise
// indistinguishable from a running one. Once a second (then every 5 s) log the
// guest function that is executing plus the VI retrace and present counts:
// a moving address means the guest is running, a frozen one says where it
// stopped, and present staying at 0 means nothing is reaching the screen.
constexpr const char* kHeartbeatLogPath = WIINX_GAME_PATH("logs/heartbeat.log");

// Last-events ring. Hot paths (scheduler, fibers, message queues) call this
// instead of SwitchDurableLog: it only copies into a static buffer, so it can
// stay uncapped, where a per-line sdmc: write would change the very timing
// being measured. The watchdog dumps the ring once, when it sees a freeze.
constexpr size_t kTraceRingSlots = 128;
constexpr size_t kTraceRingText = 112;
char g_traceRing[kTraceRingSlots][kTraceRingText];
std::atomic<uint64_t> g_traceRingWrite{0};

void SwitchTraceRing(const char* text) noexcept {
    if (text == nullptr) {
        return;
    }
    const uint64_t slot = g_traceRingWrite.fetch_add(1, std::memory_order_relaxed);
    char* entry = g_traceRing[slot % kTraceRingSlots];
    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - g_switchBootStart)
                             .count();
    std::snprintf(entry, kTraceRingText, "%7lldms %s", ms, text);
}

// Sends the ring oldest-first over UDP only (the console may be wedged; a file
// write could block). Called once per freeze by the watchdog.
void SwitchDumpTraceRing() noexcept {
    const uint64_t written = g_traceRingWrite.load(std::memory_order_relaxed);
    const uint64_t count = written < kTraceRingSlots ? written : kTraceRingSlots;
    const uint64_t first = written - count;
    // Batch into few datagrams (single lines were lost in flight) and mirror the
    // dump to its own file, so a lost packet still leaves evidence on the card.
    FILE* file = std::fopen(WIINX_GAME_PATH("logs/ring.txt"), "w");
    char batch[1024];
    size_t used = 0;
    const auto flush = [&]() {
        if (used == 0) {
            return;
        }
        if (g_netLogSocket >= 0) {
            sendto(g_netLogSocket, batch, used, 0,
                   reinterpret_cast<const sockaddr*>(&g_netLogAddr), sizeof(g_netLogAddr));
            svcSleepThread(20000000LL);  // 20ms between datagrams
        }
        used = 0;
    };
    for (uint64_t i = 0; i < count; ++i) {
        const char* entry = g_traceRing[(first + i) % kTraceRingSlots];
        if (entry[0] == '\0') {
            continue;
        }
        char line[160];
        const int length = std::snprintf(line, sizeof(line), "  [ring] %s\n", entry);
        if (length <= 0) {
            continue;
        }
        if (file != nullptr) {
            std::fwrite(line, 1, static_cast<size_t>(length), file);
        }
        if (used + static_cast<size_t>(length) >= sizeof(batch)) {
            flush();
        }
        std::memcpy(batch + used, line, static_cast<size_t>(length));
        used += static_cast<size_t>(length);
    }
    flush();
    if (file != nullptr) {
        std::fclose(file);
    }
}

// Freeze sampler. Unlike the heartbeat above it allocates nothing (static
// stack), touches no file and takes no lock: once a second it reads the main
// thread's last indirect-dispatch target through a pointer captured on that
// thread, plus lock-free counters, and sends one UDP datagram. It runs on
// core 1 so a guest thread spinning on the main core cannot starve it.
std::atomic<const uint32_t*> g_switchMainGuestAddr{nullptr};
alignas(0x1000) uint8_t g_switchWatchStack[0x10000];
Thread g_switchWatchThread;

// Sampling profiler. The guest runs 4x too slow and the cause is CPU-side, so
// sample where it actually is: every millisecond, record the main thread's last
// indirect-dispatch target (a guest function address) together with the host
// phase marker. Fixed-size table, no allocation, no locks - the watchdog thread
// already owns a core of its own.
constexpr size_t kProfileSlots = 512;
struct ProfileBucket {
    uint32_t address;
    uint32_t count;
};
ProfileBucket g_profile[kProfileSlots];
uint64_t g_profileSamples = 0;
// Samples taken while a native (HLE) replacement was running, keyed by the
// guest address it replaces; g_profile then holds only translated game code.
ProfileBucket g_nativeProfile[kProfileSlots];
uint64_t g_nativeSamples = 0;

// The guest address above is the last indirect-dispatch target, which stays put
// while the main thread is inside host code (aurora/Dawn/NVK). Sampling the
// host phase marker alongside it separates "the game is slow" from "we are
// stuck in the graphics stack", which need completely different fixes.
constexpr size_t kPhaseSlots = 32;
struct PhaseBucket {
    const char* phase;
    uint32_t count;
};
PhaseBucket g_phases[kPhaseSlots];

void PhaseSample(const char* phase) noexcept {
    for (PhaseBucket& bucket : g_phases) {
        if (bucket.phase == nullptr) {
            bucket.phase = phase;
            bucket.count = 1;
            return;
        }
        if (bucket.phase == phase) {
            ++bucket.count;
            return;
        }
    }
}

void PhaseReport() noexcept {
    for (PhaseBucket& bucket : g_phases) {
        if (bucket.phase == nullptr || bucket.count == 0) {
            continue;
        }
        char line[176];
        std::snprintf(line, sizeof(line), "[prof] host '%s' %u samples (%.1f%%)", bucket.phase,
                      bucket.count,
                      g_profileSamples != 0
                          ? 100.0 * static_cast<double>(bucket.count) / static_cast<double>(g_profileSamples)
                          : 0.0);
        SwitchBootLogExternal(line);
        bucket.count = 0;
    }
}

void ProfileSampleInto(ProfileBucket* table, uint32_t address) noexcept {
    size_t slot = (address * 2654435761u) % kProfileSlots;  // Knuth multiplicative
    for (size_t probe = 0; probe < kProfileSlots; ++probe) {
        ProfileBucket& bucket = table[(slot + probe) % kProfileSlots];
        if (bucket.count == 0) {
            bucket.address = address;
            bucket.count = 1;
            return;
        }
        if (bucket.address == address) {
            ++bucket.count;
            return;
        }
    }
}

void ProfileSample(uint32_t guestAddress, uint32_t nativeTarget) noexcept {
    ++g_profileSamples;
    if (nativeTarget != 0) {
        ++g_nativeSamples;
        ProfileSampleInto(g_nativeProfile, nativeTarget);
    } else {
        ProfileSampleInto(g_profile, guestAddress);
    }
}

void ProfileReportTable(ProfileBucket* table, const char* kind, int ranks) noexcept {
    for (int rank = 0; rank < ranks; ++rank) {
        ProfileBucket* best = nullptr;
        for (size_t i = 0; i < kProfileSlots; ++i) {
            ProfileBucket& bucket = table[i];
            if (bucket.count != 0 && (best == nullptr || bucket.count > best->count)) {
                best = &bucket;
            }
        }
        if (best == nullptr) {
            return;
        }
        char line[160];
        std::snprintf(line, sizeof(line), "[prof] %s #%d 0x%08X %.1f%%", kind, rank, best->address,
                      g_profileSamples != 0
                          ? 100.0 * static_cast<double>(best->count) / static_cast<double>(g_profileSamples)
                          : 0.0);
        SwitchBootLogExternal(line);
        best->count = 0;
    }
}

// Per 10 s window: how time splits between translated game code and the
// runtime's native functions, then the hottest entries on each side. Map
// addresses with generated/guest_symbol_table.cpp.
void ProfileReport() noexcept {
    char line[160];
    std::snprintf(line, sizeof(line), "[prof] split: game code %.1f%%, native runtime %.1f%% (%llu samples)",
                  g_profileSamples != 0
                      ? 100.0 * static_cast<double>(g_profileSamples - g_nativeSamples) /
                            static_cast<double>(g_profileSamples)
                      : 0.0,
                  g_profileSamples != 0
                      ? 100.0 * static_cast<double>(g_nativeSamples) / static_cast<double>(g_profileSamples)
                      : 0.0,
                  static_cast<unsigned long long>(g_profileSamples));
    SwitchBootLogExternal(line);
    ProfileReportTable(g_profile, "game", 15);
    ProfileReportTable(g_nativeProfile, "native", 15);
    for (size_t i = 0; i < kProfileSlots; ++i) {
        g_profile[i] = {};
        g_nativeProfile[i] = {};
    }
    g_profileSamples = 0;
    g_nativeSamples = 0;
}


// Host PC sampler. The guest-address profile above only moves on indirect
// dispatch, so direct calls and every host function (Aurora, the runtime) hide
// inside whoever dispatched last. This one briefly pauses the main thread, reads
// its real PC and LR, and resumes it. Samples go to logs/pcsamples.bin as
// (pc, lr) offsets from the module base - resolve them offline against the ELF.
// A record of (0xFFFFFFFF, milliseconds) marks each second.
Handle g_switchMainThreadHandle = INVALID_HANDLE;
uintptr_t g_switchModuleBase = 0;
constexpr size_t kPcSamplesPerFlush = 1100;
uint32_t g_pcSamples[kPcSamplesPerFlush * 2];
size_t g_pcSampleCount = 0;
FILE* g_pcSampleFile = nullptr;
bool g_pcSamplerBroken = false;

void PcSample() noexcept {
    if (g_pcSamplerBroken || g_switchMainThreadHandle == INVALID_HANDLE || g_pcSampleCount >= kPcSamplesPerFlush) {
        return;
    }
    if (R_FAILED(svcSetThreadActivity(g_switchMainThreadHandle, ThreadActivity_Paused))) {
        g_pcSamplerBroken = true;
        return;
    }
    ThreadContext context{};
    const Result rc = svcGetThreadContext3(&context, g_switchMainThreadHandle);
    svcSetThreadActivity(g_switchMainThreadHandle, ThreadActivity_Runnable);
    if (R_FAILED(rc)) {
        g_pcSamplerBroken = true;
        return;
    }
    g_pcSamples[g_pcSampleCount * 2] = static_cast<uint32_t>(context.pc.x - g_switchModuleBase);
    g_pcSamples[g_pcSampleCount * 2 + 1] = static_cast<uint32_t>(context.lr - g_switchModuleBase);
    ++g_pcSampleCount;
}

// Once a second, a short call stack of a thread: (0xFFFFFFFE, which) then pc,
// lr and six frame-record return addresses, all as module offsets. A hang
// shows up as the same stack every second.
extern "C" std::atomic<uint32_t> g_gxDecodeThreadHandle;
uint32_t g_pcStacks[2][10];
bool g_pcStackValid[2] = {false, false};

void PcStackSample(int which, Handle thread) noexcept {
    g_pcStackValid[which] = false;
    if (thread == INVALID_HANDLE || thread == 0) {
        return;
    }
    if (R_FAILED(svcSetThreadActivity(thread, ThreadActivity_Paused))) {
        return;
    }
    ThreadContext context{};
    const Result rc = svcGetThreadContext3(&context, thread);
    uint32_t* out = g_pcStacks[which];
    out[0] = 0xFFFFFFFEu;
    out[1] = static_cast<uint32_t>(which);
    out[2] = static_cast<uint32_t>(context.pc.x - g_switchModuleBase);
    out[3] = static_cast<uint32_t>(context.lr - g_switchModuleBase);
    uintptr_t fp = R_SUCCEEDED(rc) ? context.fp : 0;
    const uintptr_t sp = R_SUCCEEDED(rc) ? context.sp : 0;
    for (int i = 0; i < 6; ++i) {
        uint32_t ret = 0;
        // A frame record is {previous fp, return address}; stop at anything
        // that is not a plausible, 16-byte aligned address above the stack pointer.
        if (fp != 0 && (fp & 15u) == 0 && fp >= sp && fp - sp < 0x1000000u) {
            const uintptr_t* record = reinterpret_cast<const uintptr_t*>(fp);
            ret = static_cast<uint32_t>(record[1] - g_switchModuleBase);
            fp = record[0];
        } else {
            fp = 0;
        }
        out[4 + i] = ret;
    }
    svcSetThreadActivity(thread, ThreadActivity_Runnable);
    g_pcStackValid[which] = R_SUCCEEDED(rc);
}

void PcSampleFlush(uint64_t nowMs) noexcept {
    if (g_pcSampleFile == nullptr) {
        g_pcSampleFile = std::fopen(WIINX_GAME_PATH("logs/pcsamples.bin"), "wb");
        if (g_pcSampleFile == nullptr) {
            g_pcSamplerBroken = true;
            return;
        }
    }
    const uint32_t marker[2] = {0xFFFFFFFFu, static_cast<uint32_t>(nowMs)};
    std::fwrite(marker, sizeof(marker), 1, g_pcSampleFile);
    std::fwrite(g_pcSamples, sizeof(uint32_t) * 2, g_pcSampleCount, g_pcSampleFile);
    PcStackSample(0, g_switchMainThreadHandle);
    PcStackSample(1, static_cast<Handle>(g_gxDecodeThreadHandle.load(std::memory_order_relaxed)));
    for (int which = 0; which < 2; ++which) {
        if (g_pcStackValid[which]) {
            std::fwrite(g_pcStacks[which], sizeof(g_pcStacks[which]), 1, g_pcSampleFile);
        }
    }
    std::fflush(g_pcSampleFile);
    g_pcSampleCount = 0;
}

void SwitchWatchdogMain(void*) {
    const uint64_t watchdogStartMs = armTicksToNs(armGetSystemTick()) / 1000000ull;
    bool pcSamplerLive = false;
    uint32_t lastAddr = 0;
    int sameCount = 0;
    int dumps = 0;
    int profileTicks = 0;
    for (;;) {
        // 1000 x 1ms instead of one 1s sleep: the samples are the point, and the
        // per-second bookkeeping below still runs once per 1000 ticks.
        for (int tick = 0; tick < 1000; ++tick) {
            svcSleepThread(1000000LL);
            // Plain globals on Switch (see mkw_thread_local.h), so the watchdog
            // can read the main thread's values directly.
            ProfileSample(*const_cast<const volatile uint32_t*>(&RecompMod::g_currentTranslatedExecutionAddress),
                          *const_cast<const volatile uint32_t*>(&RecompMod::g_currentNativeTarget));
            PhaseSample(g_switchHostPhase.load(std::memory_order_relaxed));
            // Not during boot: sdmc: I/O from this thread while the main thread
            // is still reading its config has wedged boot before.
            if (pcSamplerLive) {
                PcSample();
            }
        }
        {
            const uint64_t nowMs = armTicksToNs(armGetSystemTick()) / 1000000ull;
            if (pcSamplerLive && !g_pcSamplerBroken) {
                PcSampleFlush(nowMs);
            }
            pcSamplerLive = SwitchDevLoggingEnabled() && nowMs - watchdogStartMs >= 30000;
        }
        if (++profileTicks >= 10 && SwitchDevLoggingEnabled()) {
            profileTicks = 0;
            ProfileReport();
            PhaseReport();
        }
        const uint32_t* addrPtr = g_switchMainGuestAddr.load(std::memory_order_relaxed);
        const uint32_t addr = addrPtr != nullptr ? *const_cast<const volatile uint32_t*>(addrPtr) : 0u;
        uint32_t osThread = 0;
        try {
            osThread = Memory::Read32(0x800000E4u);  // OSGetCurrentThread
        } catch (...) {
        }
        sameCount = addr == lastAddr ? sameCount + 1 : 0;
        lastAddr = addr;
        // Dump only once the ring has content: early boot sits at address 0 for
        // seconds, which used to consume the single dump on an empty ring.
        if (addr != 0 && sameCount >= 3 && dumps < 3 && (sameCount % 5) == 3) {
            ++dumps;
            SwitchDumpTraceRing();
        }
        const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - g_switchBootStart)
                                 .count();
        char line[256];
        const int length = std::snprintf(
            line, sizeof(line),
            "%7lldms [wd] guest=0x%08X (same %ds) osThread=0x%08X presents=%u gxcopies=%d sel=%u idle=%u fib=%u retrace=%u poll=%u notdue=%u host=%s\n", ms,
            addr, sameCount, osThread, VI_HLE_DebugPresentCount(), g_gxFrameCount,
            g_switchSelectCount.load(std::memory_order_relaxed),
            g_switchIdleSpinCount.load(std::memory_order_relaxed),
            g_switchFiberSwitchCount.load(std::memory_order_relaxed),
            g_viRetraceMirror.load(std::memory_order_relaxed),
            g_viPollCalls.load(std::memory_order_relaxed),
            g_viPollNotDue.load(std::memory_order_relaxed),
            g_switchHostPhase.load(std::memory_order_relaxed));
        if (g_netLogSocket >= 0 && length > 0) {
            sendto(g_netLogSocket, line, static_cast<size_t>(length), 0,
                   reinterpret_cast<const sockaddr*>(&g_netLogAddr), sizeof(g_netLogAddr));
        }
        // And into the file, once the guest has stopped moving. This line is the
        // diagnosis for a hang - it says where the guest is and what the host
        // thinks it is doing - and it used to go only to the socket, so a hang
        // with no listener left nothing behind but a boot log that stopped. Only
        // while stuck, so a healthy run does not fill the file with it.
        if (length > 0 && sameCount >= 3 && (sameCount % 5) == 3) {
            SwitchDurableLog(std::string_view(line, static_cast<size_t>(length) - 1));
        }
    }
}

void StartSwitchWatchdog() noexcept {
    g_switchMainThreadHandle = threadGetCurHandle();
    {
        MemoryInfo info{};
        u32 pageInfo = 0;
        if (R_SUCCEEDED(svcQueryMemory(&info, &pageInfo, reinterpret_cast<uintptr_t>(&SwitchWatchdogMain)))) {
            g_switchModuleBase = info.addr;
        }
        char line[128];
        std::snprintf(line, sizeof(line), "[boot] pc sampler: module base 0x%lx, SwitchWatchdogMain at +0x%lx",
                      static_cast<unsigned long>(g_switchModuleBase),
                      static_cast<unsigned long>(reinterpret_cast<uintptr_t>(&SwitchWatchdogMain) - g_switchModuleBase));
        SwitchDurableLog(line);
    }
    g_switchMainGuestAddr.store(&RecompMod::g_currentTranslatedExecutionAddress,
                                std::memory_order_relaxed);
    if (R_SUCCEEDED(threadCreate(&g_switchWatchThread, SwitchWatchdogMain, nullptr,
                                 g_switchWatchStack, sizeof(g_switchWatchStack), 0x2B, 1))) {
        threadStart(&g_switchWatchThread);
        SwitchDurableLog("[boot] watchdog started on core 1");
    } else {
        SwitchDurableLog("[boot] watchdog threadCreate FAILED");
    }
}

void StartSwitchHeartbeat() {
    std::thread([]() {
        const auto start = std::chrono::steady_clock::now();
        for (;;) {
            const auto elapsed = std::chrono::steady_clock::now() - start;
            const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
            // gxcopies is the discriminator for a blank screen: 0 means the guest
            // never reached GXCopyDisp so only the clear colour is ever presented,
            // while a rising count means frames are drawn but not reaching the panel.
            // One address sample per tick cannot tell a hard hang from a spin loop,
            // and they need opposite fixes, so sample rapidly here and report how
            // many distinct addresses were seen: 1 means genuinely stuck at that
            // instruction, a handful means looping over a few functions (usually
            // waiting on something that never completes), many means running.
            uint32_t samples[32];
            for (uint32_t& sample : samples) {
                sample = RecompMod::CurrentTranslatedExecutionAddress();
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
            uint32_t distinct = 0;
            for (uint32_t i = 0; i < 32; ++i) {
                bool seen = false;
                for (uint32_t j = 0; j < i; ++j) {
                    if (samples[j] == samples[i]) {
                        seen = true;
                        break;
                    }
                }
                if (!seen) {
                    ++distinct;
                }
            }

            char line[200];
            const int length = std::snprintf(
                line, sizeof(line),
                "[heartbeat] t=%llds guest=0x%08X distinct=%u retraces=%u presents=%u gxcopies=%d\n",
                static_cast<long long>(seconds), samples[31], distinct,
                VI_HLE_DebugRetraceCount(), VI_HLE_DebugPresentCount(), g_gxFrameCount);
            std::fputs(line, stderr);
            // Also keep a standalone file that is closed after every write. The SD
            // card only publishes a file's size once it is closed, so console.log
            // reads as empty over FTP while the game is running - useless for the
            // hang this exists to diagnose. Reopening per tick is wasteful but this
            // runs once a second, and it means the log survives a force-close.
            if (length > 0) {
                if (FILE* file = std::fopen(kHeartbeatLogPath, seconds == 0 ? "w" : "a")) {
                    std::fwrite(line, 1, static_cast<size_t>(length), file);
                    std::fclose(file);
                }
            }
            std::this_thread::sleep_for(seconds < 30 ? std::chrono::seconds(1) : std::chrono::seconds(5));
        }
    }).detach();
}
#endif

int RuntimeMain(int argc, char** argv) {
#if defined(__SWITCH__)
    // Before anything opens an SD path: create the wii-nx folders and move an
    // old sdmc:/WiiCompiled/ install into them (switch_layout.h).
    const std::string layoutMigration = SwitchLayout::MigrateLegacy();
#endif
    // Must run before the transcript duplicates stdout/stderr: it decides what
    // those descriptors are mirrored to now that the products are GUI-subsystem.
    AttachParentConsoleForDiagnostics();
#if defined(_WIN32)
    ConfigureWindowsFatalDialogBehavior();
    InstallSehLogger();
    WindowsTimerResolutionGuard timerResolutionGuard;
#elif defined(__SWITCH__)
    InstallPosixMemoryFaultHandler();
#else
    InstallPosixMemoryFaultHandler();
#endif
    InitializeProcessTranscript(argc, argv);
#if defined(__SWITCH__)
    if (FILE* truncate = std::fopen(WIINX_GAME_PATH("logs/boot.log"), "w")) {
        std::fclose(truncate);
    }
    SwitchNetLogInit();
    StartSwitchWatchdog();
    SwitchConsoleBegin();
    SwitchLoadStage(0);
    // Into the file, which is where it can still be read: the socket is the very
    // thing that is not working.
    if (!g_netLogDiagnostic.empty()) {
        SwitchDurableLog("[boot] UDP log off: " + g_netLogDiagnostic);
    }
    SwitchDurableLog("[boot] transcript initialised, entering RuntimeMain");
    if (!layoutMigration.empty()) {
        SwitchDurableLog(("[layout] " + layoutMigration).c_str());
    }
    // Heartbeat thread disabled: boot regressed the moment it was introduced.
    // The build before it reached guest OS init (config, data sections,
    // OSReport); the two builds with it die before the next milestone. It both
    // does sdmc: file I/O concurrently with the main thread's config read and
    // takes g_viMutex via VI_HLE_DebugRetraceCount, either of which can wedge
    // this early. The milestones below are main-thread only and are what the
    // working build already proved safe. Re-enable once boot is understood.
    if (std::getenv("MKW_SWITCH_HEARTBEAT") != nullptr) {
        StartSwitchHeartbeat();
    }
#endif
    std::signal(SIGABRT, AbortSignalHandler);
    // Install exit/terminate handlers to ensure we get crash info
    std::atexit(AtExitHandler);
    std::set_terminate(TerminateHandler);
    
    std::string currentEntryLabel;

    try {
#if defined(__SWITCH__)
        SwitchDurableLog("[boot] handlers installed, entering try");
#endif
        if (argc != 1) {
            throw std::invalid_argument("The game runtime does not accept command-line options; use Config.toml through the installed host.");
        }
        RuntimeConfigFile::LogLoadedConfig();
#if defined(__SWITCH__)
        SwitchDurableLog("[boot] config logged");
#endif
        if (RuntimeConfigFile::DiscordPresenceEnabled()) {
            DiscordPresence::Initialize(RuntimeConfigFile::DiscordClientId(), "Mario Kart Wii");
        }
#if defined(__SWITCH__)
        SwitchDurableLog("[boot] discord stage passed");
#endif
        RT_LOG(RT_TAG_RUNTIME) << "[boot] SystemBridge::Initialize enter" << std::endl;
#if defined(__SWITCH__)
        SwitchLoadStage(1);
        SwitchDurableLog("[boot] SystemBridge::Initialize enter (runs guest OS init)");
#endif
        SystemBridge::Initialize();
        RT_LOG(RT_TAG_RUNTIME) << "[boot] SystemBridge::Initialize done" << std::endl;
#if defined(__SWITCH__)
        SwitchLoadAdvance(0.66f);
        SwitchDurableLog("[boot] SystemBridge::Initialize done");
#endif
        TranslatedFunctionRegistry::Finalize();
#if defined(__SWITCH__)
        // Everything here runs while the boot banner is still on screen, so it
        // costs nothing visible; done lazily it lands on the guest's first call
        // with the display already black (or mid-game, as a stall).
        NAND_HLE_PrepareHostRoot();
        SwitchDurableLog("[boot] NAND root ready");
        DVD_HLE_PrescanDisc();
        SwitchDurableLog("[boot] disc prescan done");
#endif

        // Initialize Aurora (graphics backend)
        // We use auto backend (or specific if needed) and set a default window size.
        // This is required for GX commands (like texture loading) to work.
        AuroraConfig auroraConfig = {};
        auroraConfig.appName = RuntimeProduct::Active().displayName.data();
        const auto applicationDataDirectory = RuntimeConfigFile::ApplicationDataDirectory();
#if defined(__SWITCH__)
        const auto rendererCacheDirectory = applicationDataDirectory / SwitchLayout::kCacheDirName;
#else
        const auto rendererCacheDirectory = applicationDataDirectory / "Cache";
#endif
        std::error_code rendererPathError;
        std::filesystem::create_directories(rendererCacheDirectory, rendererPathError);
        if (rendererPathError) {
            RT_LOG(RT_TAG_RUNTIME) << "Unable to create renderer cache directory "
                      << RuntimeConfigFile::PathToUtf8(rendererCacheDirectory) << ": "
                      << rendererPathError.message() << std::endl;
        }
        const std::string auroraUserPath = RuntimeConfigFile::PathToUtf8(applicationDataDirectory);
        const std::string auroraCachePath = RuntimeConfigFile::PathToUtf8(rendererCacheDirectory);
        auroraConfig.userPath = auroraUserPath.c_str();
        auroraConfig.cachePath = auroraCachePath.c_str();
#if defined(__SWITCH__)
        // Aurora looks for the shipped initial_pipeline_cache.db here; on Switch it
        // lives with the other caches.
        auroraConfig.resourcesPath = auroraCachePath.c_str();
#endif
        auroraConfig.logCallback = &RuntimeAuroraLogCallback;
        auroraConfig.logLevel = LOG_DEBUG;
        const bool configWidescreen = RuntimeConfigFile::WidescreenEnabled(true);
#if defined(__SWITCH__)
        // Default to the screen's own size (0 asks Aurora for it). Rendering at
        // the Wii's 480p instead is a setting, not the default: it saves no time
        // here - the frame is spent on the CPU, not on pixels - and a swapchain
        // that does not match the screen is recreated whenever the game changes
        // screens, which blinks.
        auroraConfig.windowWidth = RuntimeConfigFile::WindowWidth(0);
        auroraConfig.windowHeight = RuntimeConfigFile::WindowHeight(0);
#else
        auroraConfig.windowWidth = configWidescreen ? 854 : 640;
        auroraConfig.windowHeight = 480;
        auroraConfig.windowWidth = RuntimeConfigFile::WindowWidth(auroraConfig.windowWidth);
        auroraConfig.windowHeight = RuntimeConfigFile::WindowHeight(auroraConfig.windowHeight);
#endif
        auroraConfig.hasWindowPosition = RuntimeConfigFile::WindowPosition(
            auroraConfig.windowPosX, auroraConfig.windowPosY);
        auroraConfig.allowJoystickBackgroundEvents = true;
        auroraConfig.disableCopyFilter = RuntimeConfigFile::DisableCopyFilter(true);
        // Dolphin-style custom textures. Aurora indexes <userPath>/texture_replacements
        // once during aurora_initialize, so both knobs only take effect on the next launch.
        // Dumps name each unmatched texture the way its replacement would have to be named,
        // which is only useful while the index is live - hence the conjunction.
        auroraConfig.allowTextureReplacements = RuntimeConfigFile::TextureReplacements(false);
        auroraConfig.allowTextureDumps = auroraConfig.allowTextureReplacements &&
                                         RuntimeConfigFile::TextureDumps(false);
        // No vsync knob: aurora always configures a non-blocking present mode.
        auroraConfig.desiredBackend = BACKEND_AUTO;
        const float resolutionMultiplier = RuntimeConfigFile::ResolutionMultiplier(1.0f);
        VISetFrameBufferScale(resolutionMultiplier);
        // One table for both directions. RuntimeConfigFile::IsSupportedGraphicsApi
        // whitelists exactly these config names, so an unrecognised value has
        // already been rejected (and reported) at parse time.
        struct GraphicsBackendEntry {
            const char* configName;
            AuroraBackend backend;
        };
#if defined(__APPLE__)
        static constexpr std::array<GraphicsBackendEntry, 2> kGraphicsBackends{{
            {"auto", BACKEND_AUTO}, {"metal", BACKEND_METAL},
        }};
// only vulkan for linux
#elif (defined(__linux__) || defined(__SWITCH__))
        static constexpr std::array<GraphicsBackendEntry, 2> kGraphicsBackends{{
            {"auto", BACKEND_AUTO}, {"vulkan", BACKEND_VULKAN},
        }};
#elif defined(_WIN32)
        static constexpr std::array<GraphicsBackendEntry, 3> kGraphicsBackends{{
            {"auto", BACKEND_AUTO}, {"d3d12", BACKEND_D3D12}, {"vulkan", BACKEND_VULKAN},
        }};

#endif
        const auto backendDisplayName = [](AuroraBackend value) -> const char* {
            for (const auto& entry : kGraphicsBackends) {
                if (entry.backend == value) {
                    return entry.configName;
                }
            }
            return "unknown";
        };

        const std::string backend = RuntimeConfigFile::GraphicsApi("auto");
        for (const auto& entry : kGraphicsBackends) {
            if (backend == entry.configName) {
                auroraConfig.desiredBackend = entry.backend;
                break;
            }
        }
        const AuroraBackend requestedBackend = auroraConfig.desiredBackend;

        // SDL only reads its Wii driver hint when the joystick subsystem starts, which
        // aurora_initialize does; a Bluetooth Wii Remote paired before launch must be
        // visible on that first scan.
        WiiRemoteInput::ConfigureSdlHints(RuntimeConfigFile::WiiRemotesEnabled(true));

        RT_LOG(RT_TAG_RUNTIME) << "[boot] aurora_initialize enter" << std::endl;
#if defined(__SWITCH__)
        SwitchLoadStage(2);
        SwitchDurableLog("[boot] aurora_initialize enter");
        // Hand the framebuffer back before Aurora creates its Vulkan surface on
        // the same nwindow.
        SwitchConsoleEnd();
#endif
        const AuroraInfo auroraInfo = aurora_initialize(0, nullptr, &auroraConfig);
#if defined(__SWITCH__)
        SwitchDurableLog("[boot] aurora_initialize returned");
#endif
        RT_LOG(RT_TAG_RUNTIME) << "[boot] aurora_initialize done, fb="
                  << auroraInfo.windowSize.native_fb_width << "x"
                  << auroraInfo.windowSize.native_fb_height << std::endl;
        if (requestedBackend != BACKEND_AUTO && auroraInfo.backend != requestedBackend) {
            RT_LOG(RT_TAG_RUNTIME) << "graphics_api=\"" << backend
                      << "\" is not available on this system; aurora fell back to \""
                      << backendDisplayName(auroraInfo.backend)
                      << "\". See the [aurora::gpu] lines above for the reason." << std::endl;
        } else {
            RT_LOG(RT_TAG_RUNTIME) << "graphics backend: " << backendDisplayName(auroraInfo.backend)
                      << std::endl;
        }
        aurora_set_frame_worker_wait_callback(ServiceGuestTimingDuringAuroraFrameWait);
        GxGuestWrite::InstallAuroraHooks();
        WiinxInstallHost();
        if (!RuntimeGuestOs::installed()) {
            // Every Wii game links the OS, so every game has these; libdol-nx
            // just does not know where this one put them (guest_os_layout.h).
            RT_LOG(RT_TAG_OS) << "no guest OS layout installed: the scheduler's globals are unknown, "
                                 "threading will not work" << std::endl;
        } else if (const std::string missing = RuntimeGuestOs::missing(); !missing.empty()) {
            // Partial is normal: wiinx-scan finds the scheduler's own variables
            // and not the rest. Anything needing a missing one says so when it
            // is reached, rather than reading address zero quietly.
            RT_LOG(RT_TAG_OS) << "guest OS layout is partial; not installed: " << missing << std::endl;
        }
        RuntimeGameHooks::surface_resized(auroraInfo.windowSize.native_fb_width,
                                          auroraInfo.windowSize.native_fb_height);
        settings_overlay::InitializeRuntimeSettings();
        RT_LOG(RT_TAG_CONFIG) << "video.widescreen=" << (configWidescreen ? "true" : "false")
                  << " SCGetAspectRatio=" << (configWidescreen ? 1 : 0)
                  << " resolutionMultiplier=" << resolutionMultiplier
                  << " window=" << auroraInfo.windowSize.width << "x" << auroraInfo.windowSize.height
                  << " native=" << auroraInfo.windowSize.native_fb_width << "x"
                  << auroraInfo.windowSize.native_fb_height
                  << " viewportPolicy=" << (configWidescreen ? "stretch" : "fit")
                  << " presentAspect="
                  << (configWidescreen ? "surface" : "4:3")
                  << std::endl;
        g_auroraInitialized.store(true, std::memory_order_release);

        auto entry = ResolveEntry();
        InitializePersistentCpuContext();
        auto& cpu = GetPersistentCpuContext();
        SeedCpuContext(cpu);
        
        // Initialize the fiber-based threading system
        Fiber::GuestFiberManager::Initialize();
        
        CpuContextScope cpuScope(&cpu);

        std::string label = entry->name;
        if (label.empty()) {
            std::ostringstream oss;
            oss << "0x" << std::hex << entry->address;
            label = oss.str();
        }
        currentEntryLabel = label;
        g_lastEntryLabel = currentEntryLabel;

#if defined(__SWITCH__)
        SwitchDurableLog("[boot] invoking guest entry point");
#endif
        InvokeIndirectCpu(entry->address, &cpu);
#if defined(__SWITCH__)
        SwitchDurableLog("[boot] guest entry returned");
#endif
        const uint32_t result = cpu.gpr[3];
        RT_LOG(RT_TAG_RUNTIME) << label << " => 0x" << std::hex << result << std::dec << " (" << result << ")" << std::endl;
        
        // Shutdown fiber system
        Fiber::GuestFiberManager::Shutdown();
        WindowPlacementPersistence::Flush(true);
        aurora_shutdown();
        DiscordPresence::Shutdown();
        SetRuntimeExitCodeImpl(0);
        ShutdownProcessTranscript();
        return 0;
    } catch (const Memory::AccessViolation& ex) {
        std::cerr << "Runtime error: " << ex.what() << std::endl;
        DumpAccessViolationReport(ex, currentEntryLabel);
        std::ostringstream details;
        details << "addr=0x" << std::hex << std::uppercase << ex.address()
                << " len=0x" << ex.length()
                << std::dec << std::nouppercase
                << " reason=" << ex.reason();
        ShowRuntimeFatalPopup("a guest memory access was out of bounds", details.str());
        WriteFatalLogImpl("access_violation", details.str());
        SetRuntimeExitCodeImpl(1);
        Fiber::GuestFiberManager::Shutdown();
        WindowPlacementPersistence::Flush(true);
        aurora_shutdown();
        DiscordPresence::Shutdown();
        ShutdownProcessTranscript();
        return 1;
    } catch (const std::exception& ex) {
#if defined(__SWITCH__)
        SwitchDurableLog(std::string("[exit] uncaught std::exception: ") + ex.what());
#endif
        std::cerr << "Runtime error: " << ex.what() << std::endl;
        SystemBridge::DumpCpuState(TryGetCpuContext());
        ShowRuntimeFatalPopup("a runtime exception occurred", ex.what());
        WriteFatalLogImpl("exception", ex.what());
        SetRuntimeExitCodeImpl(1);
        Fiber::GuestFiberManager::Shutdown();
        WindowPlacementPersistence::Flush(true);
        aurora_shutdown();
        DiscordPresence::Shutdown();
        ShutdownProcessTranscript();
        return 1;
    }
}

int main(int argc, char** argv) {
    return RuntimeMain(argc, argv);
}
