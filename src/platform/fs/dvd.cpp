#include "hle_stubs.h"
#include "isa/big_endian.h"
#include "hle/dvd_contract.h"
#include "hle/runtime_parse_helpers.h"
#include "memory.h"
#include "guest_globals.h"

// Defined in hle/gx/gx_objects.cpp. DVD reads are DMA-class writes to guest
// RAM: the game brackets them with DCInvalidateRange (not a flush), so the GX
// layer's caches must be notified explicitly that these bytes changed.
extern "C" void GxNotifyGuestRamDmaWrite(uint32_t addr, uint32_t size);
#include "fs/riivolution.h"
#include "ppc_runtime.h"
#include "recomp_mod_loader.h"
#include "runtime_config.h"
#include "runtime_log.h"
#include "../os/os_internal.h"
#include "runtime_product.h"

#include <cstdint>
#include <cstdlib>
#include <atomic>
#include <cstdio>

#if defined(__SWITCH__)
#include <switch.h>
// File scope: used by both the disc-index count and the read trace below.
void SwitchBootLogExternal(const char* text) noexcept;
void SwitchPinCurrentThread(int core) noexcept;
#endif
#include <cstring>
#include <algorithm>
#include <vector>
#include <string>
#include <map>
#include <unordered_set>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <memory>
#include <thread>
#include <sstream>
#include <utility>
#include "console_region.h"
#include "generated/RuntimeConfig.h"

namespace fs = std::filesystem;

// ============================================================================
// Configuration
// ============================================================================

// The extracted ISO "DATA" folder. We map its "files" subfolder to the DVD
// root "/" and its "sys" subfolder to "/sys/". The root is user-owned input:
// it is never embedded into or copied by the public runtime.
static fs::path g_dvdRoot;
static std::once_flag g_dvdRootOnce;

// What a disc read answers for the game's code when low memory holds none yet.
// A title from the NAND (WiiWare, a channel) has no disc: its own title code, as
// ES leaves it. Mario Kart Wii's "RMCP" used to be the only fallback, and
// Mega Man 9 read its disk ID into 0x80000000 and became RMCP01.
static uint32_t CurrentDiscGameCode() {
#if defined(RUNTIME_CONFIG_HAS_TITLE_ID)
    return RuntimeHle::CurrentGameCode(RuntimeConfig::TITLE_ID_LO);
#else
    const std::string disc = ConsoleRegion::DiscGameId();
    if (disc.size() >= 4) {
        return RuntimeHle::CurrentGameCode(static_cast<uint32_t>(
            static_cast<unsigned char>(disc[0]) << 24 | static_cast<unsigned char>(disc[1]) << 16 |
            static_cast<unsigned char>(disc[2]) << 8 | static_cast<unsigned char>(disc[3])));
    }
    return RuntimeHle::CurrentGameCode(0x524D4350u); // RMCP
#endif
}

// ============================================================================
// DVD Constants & Structures
// ============================================================================

#define DVD_STATE_FATAL_ERROR  -1
#define DVD_STATE_END           0
#define DVD_STATE_BUSY          1
#define DVD_STATE_WAITING       2
#define DVD_STATE_COVER_CLOSED  3
#define DVD_STATE_NO_DISK       4
#define DVD_STATE_COVER_OPEN    5

// Offsets in DVD Command Block (OS standard)
#define DVD_CB_OFFSET_STATE       0x0C
#define DVD_CB_OFFSET_TRANSFERRED 0x20
#define DVD_FILEINFO_OFFSET_ADDR  0x30
#define DVD_FILEINFO_OFFSET_LEN   0x34

struct DVDFileEntry {
    fs::path hostPath;
    std::string dvdPath;  // Virtual Wii path (e.g., "/Race/Course.szs")
    uint32_t size;
    uint32_t discOffsetWords = 0;
    bool isDirectory = false;
};

struct FstFileEntry {
    uint32_t start;
    uint32_t end;
    uint32_t size;
    std::string dvdPath;
    fs::path hostPath;
};

// Global State
static std::vector<DVDFileEntry> g_fileEntries;
static std::map<std::string, int32_t> g_pathToEntry;
// Maps each published file's disc range back to its runtime entry.
struct PublishedExtent {
    uint64_t startBytes = 0;
    uint64_t endBytes = 0;
    int32_t entryIndex = -1;
};
static std::vector<PublishedExtent> g_publishedExtents;
static bool g_dvdInitialized = false;
static bool g_discPrescanned = false;
static std::vector<FstFileEntry> g_fstFiles;
static bool g_fstLoaded = false;
static std::unordered_set<std::string> g_loggedReadErrors;
static constexpr int32_t kDvdFatalError = -3;

extern "C" void DVDInit_8015EA1C();

// This MEM2 region is reserved at startup for the published FST.
extern "C" uint32_t g_dvdFstReservedBase;
extern "C" uint32_t g_dvdFstReservedSize;

// Byte-wise copy into guest RAM plus the DMA notification the GX caches need.
static void CopyToGuestAsDma(uint32_t dest, const uint8_t* data, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        Memory::Write8(dest + static_cast<uint32_t>(i), data[i]);
    }
    GxNotifyGuestRamDmaWrite(dest, static_cast<uint32_t>(size));
}

// Host path strings only ever leave this module as UTF-8 display text.
static std::string HostPathText(const fs::path& path) {
    return RuntimeConfigFile::PathToUtf8(path);
}

static bool IsDvdDataRoot(const fs::path& path) {
    std::error_code ec;
    return fs::is_directory(path, ec) && !ec &&
           fs::is_directory(path / "files", ec) && !ec &&
           fs::is_regular_file(path / "sys" / "fst.bin", ec) && !ec;
}

// Single fatal idiom for this module: crash artifacts under `reason`, the same
// text in the popup, a non-zero exit code, and no second generic report from
// the atexit handler.
[[noreturn]] static void FailDvd(const char* reason, const char* category,
                                 const std::string& details) {
    RuntimeCrash::WriteCrashArtifacts(reason, details);
    SetRuntimeExitCode(EXIT_FAILURE);
    ShowRuntimeFatalPopup(category, details);
    MarkFatalErrorReported();
    std::exit(EXIT_FAILURE);
}

[[noreturn]] static void FailDvdRoot(const char* source, const fs::path& path = {}) {
    RT_LOGF(RT_TAG_DVD, "ERROR: %s", source);
    if (!path.empty()) {
        std::fprintf(stderr, ": %s", HostPathText(path).c_str());
    }
    std::fprintf(stderr,
                 "\n[dvd] Set [paths] dvd_root in Config.toml "
                 "to this game's extracted DATA directory.\n");
    std::string details = source ? source : "The configured DVD root could not be opened.";
    if (!path.empty()) {
        details += "\n\nPath: ";
        details += HostPathText(path);
    }
    details += "\n\nSet [paths] dvd_root in Config.toml to this game's "
               "extracted DATA directory.";
    FailDvd("dvd_root", "DVD data is unavailable", details);
}

// Where this title's disc data lives, or empty when it has none. WiiWare, a
// channel and the System Menu all carry their data inside the executable, so
// having no disc is an ordinary state rather than a misconfiguration: every one
// of them used to be killed at boot by a GetDvdRoot that could only succeed or
// end the process.
//
// A game that really does want the disc asks for a file by name and fails
// there, where the message can say which file, instead of at boot for a title
// that was never going to read one.
static fs::path DiscDataRootOrEmpty() {
    const fs::path path = RuntimeConfigFile::ResolvedDvdRoot();
    if (path.empty() || !IsDvdDataRoot(path)) {
        return {};
    }
    return path;
}

static const fs::path& GetDvdRoot() {
    std::call_once(g_dvdRootOnce, []() {
        const fs::path path = RuntimeConfigFile::ResolvedDvdRoot();
        if (path.empty()) {
            FailDvdRoot("No DVD root is configured");
        }
        if (!IsDvdDataRoot(path)) {
            FailDvdRoot("Configured DVD root is not an extracted DATA directory", path);
        }
        g_dvdRoot = path;
    });

    return g_dvdRoot;
}

static std::string NormalizePath(const std::string& path);
static fs::path ResolveDvdMappedHostPath(const std::string& dvdPath, const fs::path& fallbackHostPath);

static void InvokeDvdCallback(uint32_t callbackPtr, int32_t result, uint32_t fileInfoPtr) {
    if (callbackPtr == 0) {
        return;
    }
    if (!TranslatedFunctionRegistry::FindByAddressPtr(callbackPtr)) {
        return;
    }
    auto& cpu = GetPersistentCpuContext();
    cpu.gpr[3] = static_cast<uint32_t>(result);
    cpu.gpr[4] = fileInfoPtr;
    InvokeIndirectCpu(callbackPtr, &cpu);
}

void OS_HLE_WakeupThreadNoReschedule(CpuContext* ctx, uint32_t waitQueue);

static void InvokeDvdCallbackOn(CpuContext* cpu, uint32_t callbackPtr, int32_t result, uint32_t fileInfoPtr) {
    if (callbackPtr == 0 || !TranslatedFunctionRegistry::FindByAddressPtr(callbackPtr)) {
        return;
    }
    cpu->gpr[3] = static_cast<uint32_t>(result);
    cpu->gpr[4] = fileInfoPtr;
    InvokeIndirectCpu(callbackPtr, cpu);
}

// Same shape as InvokeDvdCallback minus the command block: the DVDLow callbacks
// take only a result, so r4 is deliberately left untouched here.
static void InvokeDvdLowCallback(uint32_t callbackPtr, int32_t result) {
    if (callbackPtr == 0) {
        return;
    }
    if (!TranslatedFunctionRegistry::FindByAddressPtr(callbackPtr)) {
        return;
    }
    auto& cpu = GetPersistentCpuContext();
    cpu.gpr[3] = static_cast<uint32_t>(result);
    InvokeIndirectCpu(callbackPtr, &cpu);
}

static void ReportDvdReadError(const std::string& hostPath,
                               int64_t offset,
                               uint32_t length,
                               const char* reason) {
    const std::string key = hostPath + "\n" + std::to_string(offset);
    if (!g_loggedReadErrors.insert(key).second) {
        return;
    }

    RT_LOGF(RT_TAG_DVD,
                 "ERROR: DVD read failed: host=\"%s\" offset=%lld (0x%llx) "
                 "length=%u: %s\n",
                 hostPath.c_str(),
                 static_cast<long long>(offset),
                 static_cast<unsigned long long>(offset),
                 length,
                 reason ? reason : "unknown read error");
}

static int32_t DvdReadFatal(uint32_t fileInfoPtr,
                            const std::string& hostPath,
                            int64_t offset,
                            uint32_t length,
                            const char* reason) {
    ReportDvdReadError(hostPath, offset, length, reason);
    try {
        Memory::Write32(fileInfoPtr + DVD_CB_OFFSET_STATE,
                        static_cast<uint32_t>(DVD_STATE_FATAL_ERROR));
        Memory::Write32(fileInfoPtr + DVD_CB_OFFSET_TRANSFERRED, 0);
    } catch (const Memory::AccessViolation&) {
    }
    return kDvdFatalError;
}

// ---------- reads off the game's thread
//
// A read used to happen inside the call that asked for it, on the one host
// thread every guest thread shares: a loader reading the next screen's archive
// stopped the game drawing until it finished - the lag on a button press and at
// each transition - and every streamed music block was a stall too, the file
// opened and closed for each read, a round trip to the card each time. On a Wii
// the drive works while the game draws. Reads go to a worker of their own,
// which keeps a few files open; what it read reaches guest memory on the
// scheduler thread (DVD_HLE_ProcessCompletions, with the alarms), where the
// command block finishes, the game's callback runs and a sleeping DVDReadPrio
// wakes, as the drive's interrupt would have done.
// (off while a hang between menus is narrowed down: the dirty-state fix is
// tested alone first)
constexpr bool kDvdAsync = false;
constexpr size_t kDvdOpenFiles = 8;

struct DvdJob {
    uint32_t block = 0;      // the command block (file info) the game polls
    uint32_t buffer = 0;
    uint32_t callback = 0;   // the game's, or 0
    uint32_t waitQueue = 0;  // a sleeping DVDReadPrio's, or 0
    std::filesystem::path path;
    uint64_t offset = 0;
    uint32_t length = 0;
    int64_t reportOffset = 0;  // for the error report: the offset the game asked for
    std::vector<uint8_t> data;
    DvdReadContract::HostReadFailure failure = DvdReadContract::HostReadFailure::None;
    bool ok = false;
};

namespace {
struct DvdWorker {
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::unique_ptr<DvdJob>> queued, finished;
    std::atomic<uint32_t> finishedCount{0};
    std::once_flag started;
} g_dvdWorker;

// The read, through files kept open, most recent last - the worker's, and
// the game thread's own reads when they are not queued (opening a file is a
// round trip to the card; a read per open was most of a small read's cost)
std::mutex g_dvdOpenFilesMutex;
bool DvdWorkerRead(DvdJob& job) {
    struct Open {
        std::string path;
        FILE* file;
    };
    static std::vector<Open> open;
    std::lock_guard<std::mutex> lock(g_dvdOpenFilesMutex);
    const std::string key = job.path.string();
    FILE* file = nullptr;
    for (size_t i = 0; i < open.size(); ++i) {
        if (open[i].path == key) {
            const Open hit = open[i];
            open.erase(open.begin() + static_cast<std::ptrdiff_t>(i));
            open.push_back(hit);
            file = hit.file;
            break;
        }
    }
    if (!file) {
        file = std::fopen(key.c_str(), "rb");
        if (!file) {
            job.failure = DvdReadContract::HostReadFailure::MissingFile;
            return false;
        }
        if (open.size() >= kDvdOpenFiles) {
            std::fclose(open.front().file);
            open.erase(open.begin());
        }
        open.push_back({key, file});
    }
    job.data.resize(job.length);
    if (job.length == 0) {
        return true;
    }
    if (fseeko(file, static_cast<off_t>(job.offset), SEEK_SET) != 0) {
        job.failure = DvdReadContract::HostReadFailure::BadOffset;
        return false;
    }
    if (std::fread(job.data.data(), 1, job.length, file) != job.length) {
        std::clearerr(file);
        job.failure = DvdReadContract::HostReadFailure::ShortRead;
        return false;
    }
    return true;
}

void DvdWorkerLoop() {
#if defined(__SWITCH__)
    SwitchPinCurrentThread(1);  // (core 2 is the render thread's)
#endif
    for (;;) {
        std::unique_ptr<DvdJob> job;
        {
            std::unique_lock<std::mutex> lock(g_dvdWorker.mutex);
            g_dvdWorker.wake.wait(lock, [] { return !g_dvdWorker.queued.empty(); });
            job = std::move(g_dvdWorker.queued.front());
            g_dvdWorker.queued.pop_front();
        }
        job->ok = DvdWorkerRead(*job);
        {
            std::lock_guard<std::mutex> lock(g_dvdWorker.mutex);
            g_dvdWorker.finished.push_back(std::move(job));
            g_dvdWorker.finishedCount.fetch_add(1, std::memory_order_release);
        }
        g_guestSafePointRequest.store(1, std::memory_order_relaxed);  // (delivered at the next one)
    }
}

// The block busy until the read is delivered; reads finish in the order asked.
void DvdSubmit(std::unique_ptr<DvdJob> job) {
    // (kept for the whole run rather than detached: libnx has no pthread_detach,
    // and std::thread::detach threw "Function not implemented")
    std::call_once(g_dvdWorker.started, [] { new std::thread(DvdWorkerLoop); });
    Memory::Write32(job->block + DVD_CB_OFFSET_STATE, static_cast<uint32_t>(DVD_STATE_BUSY));
    {
        std::lock_guard<std::mutex> lock(g_dvdWorker.mutex);
        g_dvdWorker.queued.push_back(std::move(job));
    }
    g_dvdWorker.wake.notify_one();
}

// Async reads only once guest threads exist: before the scheduler runs there
// is nothing to sleep on, and nothing to deliver at.
bool DvdAsyncReady() {
    return kDvdAsync && TryGetCpuContext() != nullptr && Memory::Read32(0x800000E4u) != 0;
}

// Set while finished reads are delivered: their callbacks run there, and a
// callback that reads again must not sleep - it would park whichever thread
// the scheduler last ran, from inside the very pump that delivers its read
// (the hang between menus).
int g_dvdDelivering = 0;

// Whether DVDReadPrio may sleep its caller: a guest thread proper, not a
// completion, alarm or other deferred callback (the scheduler disabled).
bool DvdCanSleep() {
    return DvdAsyncReady() && g_dvdDelivering == 0 && Memory::Read32(kSchedulerIdleFlagAddr) == 0;
}

// The first reads queued and delivered, for a run that hangs on one
std::atomic<int> g_dvdTraced{0};
void DvdTrace(const char* what, const DvdJob& job) {
    if (g_dvdTraced.fetch_add(1, std::memory_order_relaxed) >= 64) {
        return;
    }
#if defined(__SWITCH__)
    char line[200];
    std::snprintf(line, sizeof(line), "[dvd] %s block=0x%08X len=%u callback=0x%08X sleeper=0x%08X thread=0x%08X",
                  what, job.block, job.length, job.callback, job.waitQueue, Memory::Read32(0x800000E4u));
    SwitchBootLogExternal(line);
#else
    (void)what;
    (void)job;
#endif
}
}  // namespace

static void LoadFstIndex() {
    if (g_fstLoaded) {
        return;
    }
    g_fstLoaded = true;

    // No disc, no file table. This already tolerates a missing fst.bin below,
    // so the only thing a title without a disc could die on here is the root
    // check itself.
    const fs::path discRoot = DiscDataRootOrEmpty();
    if (discRoot.empty()) {
        RT_LOGF(RT_TAG_DVD, "no disc file table: this title carries its own data\n");
        return;
    }

    const fs::path fstPath = discRoot / "sys" / "fst.bin";
    std::ifstream fstFile(fstPath, std::ios::binary);
    if (!fstFile.is_open()) {
        return;
    }

    fstFile.seekg(0, std::ios::end);
    const std::streamsize size = fstFile.tellg();
    fstFile.seekg(0, std::ios::beg);
    if (size < 12) {
        return;
    }

    std::vector<uint8_t> data(static_cast<size_t>(size));
    fstFile.read(reinterpret_cast<char*>(data.data()), size);
    if (!fstFile) {
        return;
    }

    const uint32_t entryCount = BigEndian::Read32(&data[8]);
    if (entryCount == 0 || entryCount > 0x10000) {
        return;
    }

    const size_t entriesSize = static_cast<size_t>(entryCount) * 12;
    if (entriesSize >= data.size()) {
        return;
    }

    const size_t stringBase = entriesSize;

    struct DirFrame {
        uint32_t endIndex;
        std::string path;
    };

    std::vector<DirFrame> stack;
    stack.push_back({entryCount, std::string()});

    g_fstFiles.clear();
    g_fstFiles.reserve(entryCount / 2);

    for (uint32_t i = 1; i < entryCount; ++i) {
        while (!stack.empty() && i >= stack.back().endIndex) {
            stack.pop_back();
        }
        if (stack.empty()) {
            break;
        }

        const size_t entryOff = static_cast<size_t>(i) * 12;
        const uint32_t nameWord = BigEndian::Read32(&data[entryOff]);
        const uint8_t type = static_cast<uint8_t>(nameWord >> 24);
        const uint32_t nameOffset = nameWord & 0x00FFFFFFu;
        if (stringBase + nameOffset >= data.size()) {
            break;
        }

        const char* namePtr = reinterpret_cast<const char*>(&data[stringBase + nameOffset]);
        std::string name(namePtr);

        if (type != 0) {
            const uint32_t nextIndex = BigEndian::Read32(&data[entryOff + 8]);
            std::string dirPath = stack.back().path;
            if (!dirPath.empty()) {
                dirPath += "/";
            }
            dirPath += name;
            stack.push_back({nextIndex, dirPath});
            continue;
        }

        const uint32_t fileOffset = BigEndian::Read32(&data[entryOff + 4]);
        const uint32_t fileSize = BigEndian::Read32(&data[entryOff + 8]);

        std::string relPath = stack.back().path;
        if (!relPath.empty()) {
            relPath += "/";
        }
        relPath += name;

        const uint64_t startBytes64 = static_cast<uint64_t>(fileOffset) * 4ull;
        if (startBytes64 > 0xFFFFFFFFu) {
            continue;
        }
        const uint32_t startBytes = static_cast<uint32_t>(startBytes64);
        const uint32_t endBytes = startBytes + fileSize;

        FstFileEntry entry;
        entry.start = startBytes;
        entry.end = endBytes;
        entry.size = fileSize;
        entry.dvdPath = "/" + relPath;
        const fs::path baseHostPath = GetDvdRoot() / "files" / fs::path(relPath);
        entry.hostPath = ResolveDvdMappedHostPath(entry.dvdPath, baseHostPath);
        g_fstFiles.push_back(std::move(entry));
    }

    std::sort(g_fstFiles.begin(), g_fstFiles.end(),
              [](const FstFileEntry& a, const FstFileEntry& b) { return a.start < b.start; });

}

static const PublishedExtent* FindPublishedExtentForByteOffset(uint64_t offset) {
    if (g_publishedExtents.empty()) {
        return nullptr;
    }

    auto it = std::upper_bound(
        g_publishedExtents.begin(), g_publishedExtents.end(), offset,
        [](uint64_t value, const PublishedExtent& extent) { return value < extent.startBytes; });
    if (it == g_publishedExtents.begin()) {
        return nullptr;
    }
    --it;
    if (offset >= it->startBytes && offset < it->endBytes) {
        return &(*it);
    }
    return nullptr;
}

struct AbsReadResult {
    const DVDFileEntry* entry = nullptr;
    uint32_t fileOffset = 0;
    uint32_t readLength = 0;
};

static bool ResolveAbsRead(uint32_t offset, uint32_t length, AbsReadResult& out) {
    // The extent table is available after DVDInit publishes the FST.
    DVDInit_8015EA1C();
    const PublishedExtent* extent = FindPublishedExtentForByteOffset(offset);
    uint64_t effectiveOffset = offset;

    if (!extent) {
        // Accept both byte offsets and the FST's four-byte word offsets.
        const uint64_t scaledOffset = static_cast<uint64_t>(offset) * 4ull;
        extent = FindPublishedExtentForByteOffset(scaledOffset);
        if (extent) {
            effectiveOffset = scaledOffset;
        }
    }

    if (!extent) {
        return false;
    }

    const DVDFileEntry& entry = g_fileEntries[extent->entryIndex];
    const uint32_t fileOffset = static_cast<uint32_t>(effectiveOffset - extent->startBytes);
    uint32_t readLength = length;
    if (readLength == 0 || fileOffset + readLength > entry.size) {
        readLength = entry.size - fileOffset;
    }

    out.entry = &entry;
    out.fileOffset = fileOffset;
    out.readLength = readLength;
    return true;
}

// Helper: Normalize paths (Windows backslash -> forward slash, lowercase for lookup)
static std::string NormalizePath(const std::string& path) {
    return DvdFstContract::NormalizeLookupPath(path);
}

static void RegisterFileEntry(std::string dvdPath, const fs::path& hostPath, uint32_t size) {
    dvdPath = DvdFstContract::CanonicalizePath(dvdPath);

    DVDFileEntry fileEntry;
    fileEntry.hostPath = hostPath;
    fileEntry.dvdPath = dvdPath;
    fileEntry.size = size;

    const int32_t entryNum = static_cast<int32_t>(g_fileEntries.size());
    g_fileEntries.push_back(std::move(fileEntry));
    g_pathToEntry[NormalizePath(dvdPath)] = entryNum;
}

// True when a disc path already has an entry (vanilla disc or an earlier
// overlay registration). Uses the same canonicalization as RegisterFileEntry.
static bool DvdEntryExists(const std::string& dvdPath) {
    return g_pathToEntry.find(NormalizePath(DvdFstContract::CanonicalizePath(dvdPath))) !=
           g_pathToEntry.end();
}

// Walk `root` and hand every entry to `visit`. Uses the error_code iterator forms so an
// unreadable entry doesn't abort the rest of the overlay. `announceErrors` keeps the
// disc-index scan noisy and the by-name mapping quiet, matching each caller's old behavior.
template <typename Visit>
static void WalkDirectory(const fs::path& root, bool recursive, bool announceErrors, Visit&& visit) {
    std::error_code ec;

    if (!recursive) {
        for (const auto& entry : fs::directory_iterator(
                 root, fs::directory_options::skip_permission_denied, ec)) {
            if (ec) {
                break;
            }
            visit(entry);
        }
        return;
    }

    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        if (announceErrors) {
            RT_LOG(RT_TAG_DVD) << "WARNING: cannot enumerate " << HostPathText(root) << ": "
                      << ec.message() << std::endl;
        }
        return;
    }

    const fs::recursive_directory_iterator end;
    while (it != end) {
        const fs::directory_entry entry = *it;

        it.increment(ec);
        if (ec) {
            if (announceErrors) {
                RT_LOG(RT_TAG_DVD) << "WARNING: stopped enumerating " << HostPathText(root) << ": "
                          << ec.message() << std::endl;
                return;
            }
            visit(entry);
            return;
        }

        visit(entry);
    }
}

// Helper: Scan directory and build file index.
// addNewFiles=false is Riivolution's create="false": only files already on
// the disc may be replaced, nothing new is added.
static void ScanDirectory(const fs::path& root, const std::string& virtualPrefix,
                          bool recursive = true, bool addNewFiles = true) {
    std::error_code ec;
    if (!fs::exists(root, ec)) {
        return;
    }

    const auto registerEntry = [&](const fs::directory_entry& entry) {
        std::error_code entryEc;
        if (!entry.is_regular_file(entryEc) || entryEc) {
            return;
        }

        const std::uintmax_t size = fs::file_size(entry.path(), entryEc);
        if (entryEc) {
            RT_LOG(RT_TAG_DVD) << "WARNING: skipping " << HostPathText(entry.path()) << ": "
                      << entryEc.message() << std::endl;
            return;
        }

        // lexically_relative, not fs::relative: the latter canonicalizes both
        // paths via absolute(), and libstdc++ does not recognize "sdmc:" as a
        // root, so it treats "sdmc:/WiiCompiled/DATA" as relative and prepends
        // current_path() - which fails on Horizon (no cwd). Every file then
        // errored and was skipped silently, leaving the disc index empty and
        // making MKW panic on its first asset lookup. Both paths come from the
        // same walk rooted at `root`, so a pure lexical relation is exact.
        const fs::path relative = entry.path().lexically_relative(root);
        if (relative.empty()) {
            RT_LOG(RT_TAG_DVD) << "WARNING: skipping " << HostPathText(entry.path())
                      << ": not under " << HostPathText(root) << std::endl;
            return;
        }

        std::string prefix = virtualPrefix.empty() ? "/" : virtualPrefix;
        if (prefix.back() != '/' && prefix.back() != '\\') {
            prefix += "/";
        }
        std::string dvdPath = prefix + HostPathText(relative);
        if (!addNewFiles && !DvdEntryExists(dvdPath)) {
            return;
        }
        RegisterFileEntry(std::move(dvdPath), entry.path(), static_cast<uint32_t>(size));
    };

    WalkDirectory(root, recursive, /*announceErrors=*/true, registerEntry);
}

// Riivolution <folder> without a disc path: replace disc files that share a
// filename with a file in the external folder (Dolphin's
// FindFilenameNodeInFST behavior, applied across the whole index).
static void ApplyFolderByNameMapping(const RuntimeRiivolution::Mapping& mapping) {
    // Snapshot the current index: matches by filename may only target entries
    // that exist before this mapping, and RegisterFileEntry appends.
    std::map<std::string, std::vector<std::string>> discPathsByName;
    const size_t entryCount = g_fileEntries.size();
    for (size_t i = 0; i < entryCount; ++i) {
        const std::string& dvdPath = g_fileEntries[i].dvdPath;
        const size_t lastSlash = dvdPath.rfind('/');
        std::string name = lastSlash == std::string::npos ? dvdPath : dvdPath.substr(lastSlash + 1);
        RuntimeHle::LowerInPlace(name);
        if (!name.empty()) {
            discPathsByName[name].push_back(dvdPath);
        }
    }

    uint32_t replaced = 0;
    const auto applyEntry = [&](const fs::directory_entry& entry) {
        std::error_code entryEc;
        if (!entry.is_regular_file(entryEc) || entryEc) {
            return;
        }
        std::string name = HostPathText(entry.path().filename());
        RuntimeHle::LowerInPlace(name);
        const auto matches = discPathsByName.find(name);
        if (matches == discPathsByName.end()) {
            return;
        }
        const std::uintmax_t size = fs::file_size(entry.path(), entryEc);
        if (entryEc) {
            return;
        }
        for (const std::string& dvdPath : matches->second) {
            RegisterFileEntry(dvdPath, entry.path(), static_cast<uint32_t>(size));
            ++replaced;
        }
    };

    WalkDirectory(mapping.hostPath, mapping.recursive, /*announceErrors=*/false, applyEntry);

    RT_LOG(RT_TAG_DVD) << HostPathText(mapping.hostPath) << ": replaced " << replaced
              << " disc file(s) by filename" << std::endl;
}

static void ScanOverlayRoot(const RuntimeRiivolution::Overlay& overlay) {
    if (!overlay.patches) {
        // Fallback for mod roots that mirror the disc filesystem directly (not a
        // Riivolution pack, which wouldn't map anything useful this way).
        RT_LOG(RT_TAG_DVD) << HostPathText(overlay.root)
                  << ": no Riivolution XML found, treating the root as a disc-shaped overlay"
                  << std::endl;
        ScanDirectory(overlay.root, "/");
        return;
    }

    for (const auto& mapping : overlay.patches->mappings) {
        switch (mapping.kind) {
        case RuntimeRiivolution::Mapping::Kind::File: {
            if (!mapping.create && !DvdEntryExists(mapping.discPath)) {
                break;
            }
            std::error_code ec;
            const std::uintmax_t size = fs::file_size(mapping.hostPath, ec);
            if (!ec) {
                RegisterFileEntry(mapping.discPath, mapping.hostPath, static_cast<uint32_t>(size));
            }
            break;
        }
        case RuntimeRiivolution::Mapping::Kind::Folder:
            ScanDirectory(mapping.hostPath, mapping.discPath, mapping.recursive, mapping.create);
            break;
        case RuntimeRiivolution::Mapping::Kind::FolderByName:
            ApplyFolderByNameMapping(mapping);
            break;
        }
    }
}

static fs::path ResolveDvdMappedHostPath(const std::string& dvdPath, const fs::path& fallbackHostPath) {
    const std::string normalized = NormalizePath(dvdPath);
    const auto it = g_pathToEntry.find(normalized);
    if (it != g_pathToEntry.end() && it->second >= 0 &&
        it->second < static_cast<int32_t>(g_fileEntries.size())) {
        return g_fileEntries[it->second].hostPath;
    }

    for (const auto& overlay : RuntimeRiivolution::Overlays()) {
        const fs::path candidate = overlay.root / fs::path(normalized.substr(1));
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec)) {
            return candidate;
        }
    }

    return fallbackHostPath;
}

[[noreturn]] static void FailRuntimeFst(const std::string& reason) {
    RT_LOGF(RT_TAG_DVD, "ERROR: cannot publish the runtime FST: %s\n", reason.c_str());
    FailDvd("dvd_fst", "DVD file table is unavailable", reason);
}

static void BuildAndPublishRuntimeFst() {
    // Preserve real disc offsets where a runtime entry replaces a file from the
    // extracted image. Created Riivolution paths have no physical disc extent;
    // they are opened by their shared FST/SDK entry number instead.
    for (const FstFileEntry& fstFile : g_fstFiles) {
        const auto mapped = g_pathToEntry.find(NormalizePath(fstFile.dvdPath));
        if (mapped == g_pathToEntry.end() || mapped->second < 0 ||
            mapped->second >= static_cast<int32_t>(g_fileEntries.size())) {
            continue;
        }
        g_fileEntries[mapped->second].discOffsetWords = fstFile.start / 4u;
    }

    // Give runtime-added files unique synthetic disc ranges so reads can find them.
    uint64_t nextFreeBytes = 0;
    for (const FstFileEntry& fstFile : g_fstFiles) {
        nextFreeBytes = std::max(nextFreeBytes, static_cast<uint64_t>(fstFile.end));
    }
    for (const DVDFileEntry& entry : g_fileEntries) {
        if (entry.discOffsetWords != 0) {
            nextFreeBytes = std::max(
                nextFreeBytes, static_cast<uint64_t>(entry.discOffsetWords) * UINT64_C(4) +
                                   static_cast<uint64_t>(entry.size));
        }
    }
    for (DVDFileEntry& entry : g_fileEntries) {
        if (entry.isDirectory || entry.discOffsetWords != 0) {
            continue;
        }
        nextFreeBytes = (nextFreeBytes + 31ull) & ~31ull;
        if (nextFreeBytes / 4ull > 0xFFFFFFFFull) {
            FailRuntimeFst("synthetic disc extents exceed the 32-bit FST word offset range");
        }
        entry.discOffsetWords = static_cast<uint32_t>(nextFreeBytes / 4ull);
        nextFreeBytes += std::max<uint32_t>(entry.size, 4u);
    }

    std::vector<DvdFstContract::RegisteredFile> registrations;
    registrations.reserve(g_fileEntries.size());
    for (const DVDFileEntry& entry : g_fileEntries) {
        registrations.push_back({entry.hostPath, entry.dvdPath, entry.size, entry.discOffsetWords});
    }

    DvdFstContract::Image image;
    try {
        image = DvdFstContract::BuildImage(registrations);
    } catch (const std::exception& error) {
        FailRuntimeFst(error.what());
    }

    std::vector<DVDFileEntry> indexedEntries;
    indexedEntries.reserve(image.entries.size());
    for (const DvdFstContract::IndexedEntry& entry : image.entries) {
        indexedEntries.push_back({entry.hostPath, entry.dvdPath, entry.size,
                                  entry.discOffsetWords, entry.isDirectory});
    }
    g_fileEntries = std::move(indexedEntries);
    g_pathToEntry = std::move(image.pathToEntry);

    g_publishedExtents.clear();
    g_publishedExtents.reserve(g_fileEntries.size());
    for (size_t i = 0; i < g_fileEntries.size(); ++i) {
        const DVDFileEntry& entry = g_fileEntries[i];
        if (entry.isDirectory || entry.discOffsetWords == 0) {
            continue;
        }
        const uint64_t startBytes = static_cast<uint64_t>(entry.discOffsetWords) * 4ull;
        // Give empty files a one-byte range so their handles still resolve.
        const uint64_t endBytes = startBytes + std::max<uint32_t>(entry.size, 1u);
        g_publishedExtents.push_back({startBytes, endBytes, static_cast<int32_t>(i)});
    }
    std::sort(g_publishedExtents.begin(), g_publishedExtents.end(),
              [](const PublishedExtent& a, const PublishedExtent& b) {
                  return a.startBytes < b.startBytes;
              });

    // Use the FST memory reserved before guest code can allocate over it.
    if (g_dvdFstReservedBase == 0 || image.bytes.size() > g_dvdFstReservedSize ||
        !Memory::Contains(g_dvdFstReservedBase, g_dvdFstReservedSize)) {
        std::ostringstream reason;
        reason << "The " << image.bytes.size() << "-byte runtime FST does not fit the "
               << g_dvdFstReservedSize << "-byte guest reservation at 0x" << std::hex
               << g_dvdFstReservedBase << ".";
        FailRuntimeFst(reason.str());
    }

    const uint32_t fstAddress = g_dvdFstReservedBase;
    for (size_t i = 0; i < image.bytes.size(); ++i) {
        Memory::Write8(fstAddress + static_cast<uint32_t>(i), image.bytes[i]);
    }

    Memory::Write32(0x80000038u, fstAddress);
    Memory::Write32(0x8000003Cu, static_cast<uint32_t>(image.bytes.size()));

    RT_LOG(RT_TAG_DVD) << "published " << image.entries.size() << " FST entries ("
              << image.bytes.size() << " bytes) at 0x" << std::hex << fstAddress
              << " inside the boot-time MEM2 reservation" << std::dec << std::endl;

    if (const char* dumpPath = std::getenv("MKW_DUMP_FST")) {
        std::ofstream dump(dumpPath, std::ios::binary);
        dump.write(reinterpret_cast<const char*>(image.bytes.data()),
                   static_cast<std::streamsize>(image.bytes.size()));
        RT_LOG(RT_TAG_DVD) << "dumped published FST to " << dumpPath << std::endl;
    }
}

extern "C" const char* DVDResolveHostPathForTest(const char* dvdPath)
{
    static std::string resolved;
    resolved.clear();

    if (!dvdPath || dvdPath[0] == '\0') {
        return nullptr;
    }

    DVDInit_8015EA1C();
    const std::string normalized = NormalizePath(dvdPath);
    const auto it = g_pathToEntry.find(normalized);
    if (it == g_pathToEntry.end() ||
        it->second < 0 ||
        it->second >= static_cast<int32_t>(g_fileEntries.size()) ||
        g_fileEntries[it->second].isDirectory) {
        return nullptr;
    }

    resolved = HostPathText(g_fileEntries[it->second].hostPath);
    return resolved.c_str();
}

// ============================================================================
// High-Level DVD API
// ============================================================================

// Write to one of this game's DVD variables (guest_globals.h), if it named it.
static void WriteDvd32(const char* name, uint32_t value)
{
    if (const uint32_t addr = RuntimeGuestGlobals::find(name)) Memory::Write32(addr, value);
}
static void WriteDvd8(const char* name, uint8_t value)
{
    if (const uint32_t addr = RuntimeGuestGlobals::find(name)) Memory::Write8(addr, value);
}

static void InitDvdWaitingQueues()
{
    const uint32_t kQueueBase = RuntimeGuestGlobals::find("dvd.WaitingQueue");
    if (kQueueBase == 0) return;
    for (uint32_t i = 0; i < 4; ++i) {
        const uint32_t queue = kQueueBase + (i * 8);
        Memory::Write32(queue + 0, queue);
        Memory::Write32(queue + 4, queue);
    }
}

static void CompleteDvdCancelState()
{
    InitDvdWaitingQueues();

    // These are the SDK DVD globals used by DVDCancelAll/__DVDPrepareReset.
    // The actual drive work is HLE'd, so complete pending cancel/reset waits.
    WriteDvd32("dvd.PauseFlag", 0);
    WriteDvd32("dvd.PausingFlag", 0);
    WriteDvd32("dvd.Canceling", 0);
    WriteDvd32("dvd.cancelAllSyncDone", 1);
    WriteDvd32("dvd.prepareResetDone", 1);
    WriteDvd32("dvd.executing", 0);
}

// 0x8015EA1C -> DVDInit
// Builds the vanilla disc index ahead of the guest's DVDInit. The scan is pure
// host work (a stat per file), but on a Switch SD card it takes ~4.7s for
// MKW's 2037 files. Running it before Aurora takes the display lets it happen
// while the boot loading text is still on screen instead of on a black one.
#if defined(__SWITCH__)
// The disc's files listed through Horizon's own directory reads, which give
// each entry's name, type and size in batches: the portable walk asked for
// each file's type and then its size, two round trips to the card a file -
// 2037 files, nearly five seconds before the game's first frame. False when
// the root is not on the card, and the portable walk runs instead.
static bool ScanDirectoryFast(const fs::path& root, const std::string& virtualPrefix) {
    const std::string rootText = root.string();
    constexpr std::string_view kDevice = "sdmc:";
    if (rootText.compare(0, kDevice.size(), kDevice) != 0) {
        return false;
    }
    FsFileSystem* card = fsdevGetDeviceFileSystem("sdmc");
    if (!card) {
        return false;
    }
    std::string prefix = virtualPrefix.empty() ? "/" : virtualPrefix;
    if (prefix.back() != '/') {
        prefix += '/';
    }
    std::vector<FsDirectoryEntry> entries(256);
    // (directories still to list: the card path, and its path under the root)
    std::vector<std::pair<std::string, std::string>> pending{{rootText.substr(kDevice.size()), std::string()}};
    while (!pending.empty()) {
        auto [cardPath, relative] = std::move(pending.back());
        pending.pop_back();
        if (cardPath.empty()) {
            cardPath = "/";
        }
        char path[FS_MAX_PATH];
        std::snprintf(path, sizeof(path), "%s", cardPath.c_str());
        FsDir dir;
        if (R_FAILED(fsFsOpenDirectory(card, path, FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles, &dir))) {
            return false;
        }
        for (;;) {
            s64 count = 0;
            if (R_FAILED(fsDirRead(&dir, &count, entries.size(), entries.data())) || count <= 0) {
                break;
            }
            for (s64 i = 0; i < count; ++i) {
                const FsDirectoryEntry& e = entries[static_cast<size_t>(i)];
                const std::string name = e.name;
                const std::string childRelative = relative.empty() ? name : relative + "/" + name;
                const std::string childCard = cardPath == "/" ? "/" + name : cardPath + "/" + name;
                if (e.type == FsDirEntryType_Dir) {
                    pending.emplace_back(childCard, childRelative);
                } else {
                    RegisterFileEntry(prefix + childRelative, fs::path(std::string(kDevice) + childCard),
                                      static_cast<uint32_t>(e.file_size));
                }
            }
        }
        fsDirClose(&dir);
    }
    return true;
}
#endif

void DVD_HLE_PrescanDisc()
{
    if (g_discPrescanned || g_dvdInitialized) {
        return;
    }
    const fs::path rootPath = DiscDataRootOrEmpty();
    if (rootPath.empty()) {
        RT_LOGF(RT_TAG_DVD, "no disc to prescan: this title carries its own data\n");
        return;
    }
#if defined(__SWITCH__)
    const bool fast = ScanDirectoryFast(rootPath / "files", "/") && ScanDirectoryFast(rootPath / "sys", "/sys/");
    if (!fast) {
        g_fileEntries.clear();  // (a partial listing is redone whole by the portable walk)
        g_pathToEntry.clear();
#else
    {
#endif
        ScanDirectory(rootPath / "files", "/");
        ScanDirectory(rootPath / "sys", "/sys/");
    }
    g_discPrescanned = true;
}

extern "C" void DVDInit_8015EA1C()
{
    if (g_dvdInitialized) return;
    // The scan below builds g_fileEntries incrementally, so a re-entrant call
    // must not start a second scan on top of a half-built index and duplicate
    // every entry.
    static bool initializing = false;
    if (initializing) return;
    initializing = true;


    // 1. Initialize Global Flags (Emulate OS state)
    // These addresses are standard OS globals for DVD context
    WriteDvd8("dvd.contextsInitialized", 1);
    WriteDvd8("dvd.lowInitCalled", 1);
    WriteDvd32("dvd.freeDvdContext", 0);
    WriteDvd8("dvd.DVDInitialized", 1);
    CompleteDvdCancelState();

    // 2. Initialize DVD Context structures (prevent crashes in callbacks)
    const uint32_t kContextBase = RuntimeGuestGlobals::find("dvd.dvdContexts");
    constexpr uint32_t kMagicValue = 0xFEEBDAED;
    for (int i = 0; kContextBase != 0 && i < 4; i++) {
        uint32_t ctx = kContextBase + i * 0x20;
        Memory::Write32(ctx + 0x0C, kMagicValue);
        Memory::Write32(ctx + 0x10, i);
    }

    // 3. Set Low Memory Globals (The "Magic" Identification)
    // This tells the game "Yes, I am Mario Kart Wii"
    uint32_t diskHeader = 0x80000000;
    Memory::Write32(diskHeader + 0x00, CurrentDiscGameCode());
    Memory::Write16(diskHeader + 0x04, 0x3031);     // '01' (Maker)
    Memory::Write8(diskHeader + 0x06, 0x01);        // Disk #1
    // 4. Scan Files (unless DVD_HLE_PrescanDisc already did, during boot)
    // A title with no disc leaves g_discPrescanned false because it had nothing
    // to scan, not because the scan still owes us something: asking for the
    // root here is the same boot-time death the prescan was taught to avoid.
    if (!g_discPrescanned) {
        const fs::path rootPath = DiscDataRootOrEmpty();
        if (rootPath.empty()) {
            RT_LOGF(RT_TAG_DVD, "no disc to index: this title carries its own data\n");
        } else {
            // Map "<dvd_root>/files" -> "/"
            ScanDirectory(rootPath / "files", "/");

            // Map "<dvd_root>/sys" -> "/sys/" (e.g. main.dol, bi2.bin)
            ScanDirectory(rootPath / "sys", "/sys/");
        }
    }

    const auto& overlays = RuntimeRiivolution::Overlays();
    if (overlays.empty() && RuntimeProduct::OverlaysDisc()) {
        const auto& product = RuntimeProduct::Active();
        RT_LOG(RT_TAG_DVD) << "WARNING: " << product.displayName << " found no files of its own to "
                     "overlay. File replacements will not apply and the game will look and play "
                     "like the unmodded disc. Set [paths] " << product.fileRootSetting
                  << " in Config.toml to where they are." << std::endl;
    }
    // RegisterFileEntry lets the last registration win, so apply the roots in
    // reverse discovery order: the explicitly configured root outranks the mod
    // manifest.
    const size_t vanillaEntryCount = g_fileEntries.size();
    for (auto overlay = overlays.rbegin(); overlay != overlays.rend(); ++overlay) {
        ScanOverlayRoot(*overlay);
    }
#if defined(__SWITCH__)
    {
        char trace[96];
        std::snprintf(trace, sizeof(trace), "[dvd] disc index: %zu disc file(s)", vanillaEntryCount);
        SwitchBootLogExternal(trace);
    }
#endif
    RT_LOG(RT_TAG_DVD) << "disc index: " << vanillaEntryCount << " disc file(s), "
              << (g_fileEntries.size() - vanillaEntryCount) << " overlay registration(s) from "
              << overlays.size() << " root(s)" << std::endl;

    // Load FST mapping so real files keep their physical disc extents.
    LoadFstIndex();
    BuildAndPublishRuntimeFst();

    // Initialize the translated DVD filesystem so it can use the published FST.
    const uint32_t kDvdFsInitAddress = RuntimeGuestGlobals::find("dvd.__DVDFSInit");
    if (kDvdFsInitAddress != 0 && TranslatedFunctionRegistry::FindByAddressPtr(kDvdFsInitAddress)) {
        InvokeIndirectCpu(kDvdFsInitAddress, &GetPersistentCpuContext());
    }

    g_dvdInitialized = true;
    initializing = false;
}
PPC_NATIVE_OVERRIDE_VOID(8015EA1C, DVDInit_8015EA1C, (), ());

// FST-only DVD functions run translated so mods can override them safely.

// A file read resolved to its host file and range; false - the block marked
// fatal, `fatal` the result - when it cannot be.
static bool ResolveFileRead(uint32_t fileInfoPtr, uint32_t bufferPtr, int32_t length, int32_t offset, DvdJob& job,
                            int32_t& fatal) {
    uint32_t startWords = 0;
    try {
        startWords = Memory::Read32(fileInfoPtr + DVD_FILEINFO_OFFSET_ADDR);
    } catch (const Memory::AccessViolation&) {
        fatal = DvdReadFatal(fileInfoPtr, "<invalid DVD file info>", offset,
                            length > 0 ? static_cast<uint32_t>(length) : 0,
                            "DVD file info is outside guest memory");
        return false;
    }

    // Treat startAddr as an FST offset, including offsets inside a file.
    const uint64_t startBytes = static_cast<uint64_t>(startWords) * 4ull;
    const PublishedExtent* extent = FindPublishedExtentForByteOffset(startBytes);
    if (!extent) {
        fatal = DvdReadFatal(fileInfoPtr, "<unresolved DVD file handle>", offset,
                            length > 0 ? static_cast<uint32_t>(length) : 0,
                            "DVD file info does not reference a published FST extent");
        return false;
    }

    const DVDFileEntry& entry = g_fileEntries[extent->entryIndex];

    if (offset < 0 || length < 0) {
        fatal = DvdReadFatal(fileInfoPtr, HostPathText(entry.hostPath), offset,
                            length > 0 ? static_cast<uint32_t>(length) : 0,
                            "negative DVD read offset or length");
        return false;
    }

    const uint64_t extentBias = startBytes - extent->startBytes;
    const uint64_t requestedOffset = extentBias + static_cast<uint32_t>(offset);
    uint32_t uLength = (uint32_t)length;

    if (requestedOffset >= entry.size) {
        fatal = DvdReadFatal(fileInfoPtr, HostPathText(entry.hostPath), offset, uLength,
                            "read offset is outside the indexed DVD file");
        return false;
    }
    const uint32_t uOffset = static_cast<uint32_t>(requestedOffset);

    if (uLength > entry.size - uOffset) {
        uLength = entry.size - uOffset;
    }

    if (uLength != 0 && !Memory::Contains(bufferPtr, uLength)) {
        fatal = DvdReadFatal(fileInfoPtr, HostPathText(entry.hostPath), offset, uLength,
                            "DVD read destination is outside guest memory");
        return false;
    }

    job.block = fileInfoPtr;
    job.buffer = bufferPtr;
    job.path = entry.hostPath;
    job.offset = uOffset;
    job.length = uLength;
    job.reportOffset = offset;
    return true;
}

// The read now, on this thread (through the open files the worker keeps).
static int32_t DvdReadNow(DvdJob& job) {
    if (!DvdWorkerRead(job)) {
        return DvdReadFatal(job.block, HostPathText(job.path), job.reportOffset, job.length,
                            DvdReadContract::Describe(job.failure));
    }
    if (job.length != 0) {
        CopyToGuestAsDma(job.buffer, job.data.data(), job.length);
    }
    Memory::Write32(job.block + DVD_CB_OFFSET_TRANSFERRED, job.length);
    Memory::Write32(job.block + DVD_CB_OFFSET_STATE, DVD_STATE_END);
    CompleteDvdCancelState();
    return static_cast<int32_t>(job.length);
}

// DVDReadPrio's wait, as the SDK's: the read queued, and the calling guest
// thread asleep - on a queue in its own stack - until it is delivered. The
// other guest threads run meanwhile; the game keeps drawing.
static int32_t DvdReadAndWait(CpuContext* cpu, std::unique_ptr<DvdJob> job) {
    const uint32_t block = job->block;
    const uint32_t savedR1 = cpu->gpr[1], savedR3 = cpu->gpr[3];
    cpu->gpr[1] = (savedR1 - 32u) & ~7u;
    const uint32_t queue = cpu->gpr[1] + 8u;
    Memory::Write32(queue, 0);
    Memory::Write32(queue + 4u, 0);
    job->waitQueue = queue;
    DvdTrace("wait", *job);
    DvdSubmit(std::move(job));
    while (static_cast<int32_t>(Memory::Read32(block + DVD_CB_OFFSET_STATE)) == DVD_STATE_BUSY) {
        cpu->gpr[3] = queue;
        OSSleepThread_HLE_801aa9b8(cpu);
    }
    cpu->gpr[1] = savedR1;
    cpu->gpr[3] = savedR3;
    return static_cast<int32_t>(Memory::Read32(block + DVD_CB_OFFSET_STATE)) == DVD_STATE_END
               ? static_cast<int32_t>(Memory::Read32(block + DVD_CB_OFFSET_TRANSFERRED))
               : kDvdFatalError;
}

// (scheduler thread, with the alarms) finished reads into guest memory, their
// blocks done, their callbacks run and their sleepers woken
bool DVD_HLE_ProcessCompletions(CpuContext* cpu) {
    if (g_dvdWorker.finishedCount.load(std::memory_order_acquire) == 0) {
        return false;
    }
    std::deque<std::unique_ptr<DvdJob>> done;
    {
        std::lock_guard<std::mutex> lock(g_dvdWorker.mutex);
        done.swap(g_dvdWorker.finished);
        g_dvdWorker.finishedCount.store(0, std::memory_order_relaxed);
    }
    ++g_dvdDelivering;
    for (auto& job : done) {
        DvdTrace("done", *job);
        int32_t result = kDvdFatalError;
        try {
            if (job->ok) {
                if (job->length != 0) {
                    CopyToGuestAsDma(job->buffer, job->data.data(), job->length);
                }
                Memory::Write32(job->block + DVD_CB_OFFSET_TRANSFERRED, job->length);
                Memory::Write32(job->block + DVD_CB_OFFSET_STATE, DVD_STATE_END);
                result = static_cast<int32_t>(job->length);
            } else {
                result = DvdReadFatal(job->block, HostPathText(job->path), job->reportOffset, job->length,
                                      DvdReadContract::Describe(job->failure));
            }
        } catch (const Memory::AccessViolation&) {
        }
        CompleteDvdCancelState();
        if (job->callback) {
            InvokeDvdCallbackOn(cpu, job->callback, result, job->block);
        }
        if (job->waitQueue) {
            OS_HLE_WakeupThreadNoReschedule(cpu, job->waitQueue);
        }
    }
    --g_dvdDelivering;
    return !done.empty();
}

// 0x8015E834 -> DVDReadPrio
extern "C" int32_t DVDReadPrio_8015E834(uint32_t fileInfoPtr, uint32_t bufferPtr, int32_t length, int32_t offset, int32_t prio)
{
#if defined(__SWITCH__)
    // The game sets VI black and waits during load, so if it never draws, the
    // first question is whether disc reads are being served at all. Log the
    // first few and their result; silence here means nothing is even asking.
    {
        static std::atomic<int> loggedReads{0};
        const int index = loggedReads.fetch_add(1, std::memory_order_relaxed);
        if (index < 12) {
            char trace[160];
            std::snprintf(trace, sizeof(trace),
                          "[dvd] read #%d fileInfo=0x%08X len=%d offset=%d", index, fileInfoPtr,
                          length, offset);
            SwitchBootLogExternal(trace);
        }
    }
#endif
    (void)prio;

    DvdJob job;
    int32_t fatal = kDvdFatalError;
    if (!ResolveFileRead(fileInfoPtr, bufferPtr, length, offset, job, fatal)) {
        return fatal;
    }
    if (DvdCanSleep()) {
        return DvdReadAndWait(TryGetCpuContext(), std::make_unique<DvdJob>(std::move(job)));
    }
    return DvdReadNow(job);
}
PPC_NATIVE_OVERRIDE(8015E834, DVDReadPrio_8015E834, int32_t, (uint32_t f, uint32_t b, int32_t l, int32_t o, int32_t p), (f, b, l, o, p));

// 0x8015E74C -> DVDReadAsyncPrio (internal)
// The read queued to the worker; the callback runs when it is delivered.
extern "C" int32_t DVD__ReadAsyncPrio_HLE_8015e74c(uint32_t fileInfoPtr,
                                                   uint32_t bufferPtr,
                                                   int32_t length,
                                                   int32_t offset,
                                                   uint32_t callbackPtr,
                                                   int32_t prio)
{
    (void)prio;
    DvdJob job;
    int32_t bytesRead = kDvdFatalError;
    if (ResolveFileRead(fileInfoPtr, bufferPtr, length, offset, job, bytesRead)) {
        if (DvdAsyncReady()) {
            job.callback = callbackPtr;
            DvdTrace("async", job);
            DvdSubmit(std::make_unique<DvdJob>(std::move(job)));
            return 1;
        }
        bytesRead = DvdReadNow(job);
    }

    InvokeDvdCallback(callbackPtr, bytesRead, fileInfoPtr);
    CompleteDvdCancelState();
    return bytesRead >= 0 ? 1 : 0;
}
PPC_NATIVE_OVERRIDE(8015E74C, DVD__ReadAsyncPrio_HLE_8015e74c, int32_t,
         (uint32_t f, uint32_t b, int32_t l, int32_t o, uint32_t cb, int32_t p),
         (f, b, l, o, cb, p));

// 0x801628CC -> DVDReadAbsAsyncPrio (internal)
// HLE: resolve the absolute disc offset to a host file and read it.
extern "C" int32_t DVD__ReadAbsAsyncPrio_HLE_801628cc(uint32_t cmdBlockPtr,
                                                      uint32_t bufferPtr,
                                                      int32_t length,
                                                      int32_t offset,
                                                      uint32_t callbackPtr,
                                                      int32_t prio)
{
    (void)prio;
    int32_t bytesRead = -1;
    {
        AbsReadResult readInfo;
        const uint32_t requestedLength = length > 0 ? static_cast<uint32_t>(length) : 0;
        const uint32_t absoluteOffset = static_cast<uint32_t>(offset);
        if (length < 0) {
            bytesRead = DvdReadFatal(cmdBlockPtr, "<unmapped DVD offset>", absoluteOffset, 0,
                                     "negative absolute DVD read length");
        } else if (!ResolveAbsRead(absoluteOffset, requestedLength, readInfo)) {
            bytesRead = DvdReadFatal(cmdBlockPtr, "<unmapped DVD offset>", absoluteOffset,
                                     requestedLength,
                                     "absolute DVD read offset is not mapped to a host file");
        } else if (requestedLength != 0 && readInfo.readLength != requestedLength) {
            bytesRead = DvdReadFatal(cmdBlockPtr, HostPathText(readInfo.entry->hostPath),
                                     readInfo.fileOffset, requestedLength,
                                     "requested range extends beyond the indexed DVD file");
        } else if (requestedLength != 0 && !Memory::Contains(bufferPtr, requestedLength)) {
            bytesRead = DvdReadFatal(cmdBlockPtr, HostPathText(readInfo.entry->hostPath),
                                     readInfo.fileOffset, requestedLength,
                                     "DVD read destination is outside guest memory");
        } else if (DvdAsyncReady()) {
            auto job = std::make_unique<DvdJob>();
            job->block = cmdBlockPtr;
            job->buffer = bufferPtr;
            job->callback = callbackPtr;
            job->path = readInfo.entry->hostPath;
            job->offset = readInfo.fileOffset;
            job->length = readInfo.readLength;
            job->reportOffset = readInfo.fileOffset;
            DvdTrace("abs async", *job);
            DvdSubmit(std::move(job));
            return 1;
        } else {
            std::vector<uint8_t> tempBuf;
            DvdReadContract::HostReadFailure failure;
            if (!DvdReadContract::ReadExact(readInfo.entry->hostPath,
                                            readInfo.fileOffset,
                                            readInfo.readLength,
                                            tempBuf,
                                            failure)) {
                bytesRead = DvdReadFatal(cmdBlockPtr, HostPathText(readInfo.entry->hostPath),
                                         readInfo.fileOffset, readInfo.readLength,
                                         DvdReadContract::Describe(failure));
            } else {
                CopyToGuestAsDma(bufferPtr, tempBuf.data(), tempBuf.size());
                bytesRead = static_cast<int32_t>(tempBuf.size());
            }
        }

        if (bytesRead >= 0) {
            try {
                Memory::Write32(cmdBlockPtr + DVD_CB_OFFSET_STATE, DVD_STATE_END);
                Memory::Write32(cmdBlockPtr + DVD_CB_OFFSET_TRANSFERRED,
                                static_cast<uint32_t>(bytesRead));
            } catch (const Memory::AccessViolation&) {
            }
        }
    }

    InvokeDvdCallback(callbackPtr, bytesRead, cmdBlockPtr);
    CompleteDvdCancelState();
    return bytesRead >= 0 ? 1 : 0;
}
PPC_NATIVE_OVERRIDE(801628CC, DVD__ReadAbsAsyncPrio_HLE_801628cc, int32_t,
         (uint32_t cb, uint32_t b, int32_t l, int32_t o, uint32_t cbfn, int32_t p),
         (cb, b, l, o, cbfn, p));


// ============================================================================
// Low-Level / Core DVD (The "Magic" Handlers)
// ============================================================================

// For libwii-nx's /dev/di: whether the title has a disc behind it at all
// (a channel such as the Wii Menu runs with the drive empty).
extern "C" bool DVD_HLE_DiscPresent() { return !DiscDataRootOrEmpty().empty(); }

// 0x80164848 -> DVDLowInit
extern "C" int32_t DVDLowInit_80164848() {
    // Just ensure init is done
    DVDInit_8015EA1C(); 
    return 1;
}
PPC_NATIVE_OVERRIDE(80164848, DVDLowInit_80164848, int32_t, (), ());

extern "C" int32_t DVDLowInquiry_80165A30(uint32_t cmdBlockPtr, uint32_t callback)
{
    // HLE: acknowledge the drive is present. Must mark the command block complete
    // (State = 0) or callers polling this address hang waiting for "Busy" to clear.
    if (cmdBlockPtr) {
        Memory::Write32(cmdBlockPtr + DVD_CB_OFFSET_STATE, DVD_STATE_END);
    }
    CompleteDvdCancelState();

    return 1; // Return 1 to indicate the command was successfully issued.
}
PPC_NATIVE_OVERRIDE(80165A30, DVDLowInquiry_80165A30, int32_t, (uint32_t b, uint32_t c), (b, c));

// 0x80164AAC -> DVDLowReadDiskID
extern "C" int32_t DVDLowReadDiskID_80164AAC(uint32_t diskIdPtr, uint32_t callback) {
    if (diskIdPtr) {
        Memory::Write32(diskIdPtr + 0x00, CurrentDiscGameCode());
        Memory::Write16(diskIdPtr + 0x04, 0x3031);
        Memory::Write8 (diskIdPtr + 0x06, 0x01);
    }
    CompleteDvdCancelState();
    return 1; // Success
}
PPC_NATIVE_OVERRIDE(80164AAC, DVDLowReadDiskID_80164AAC, int32_t, (uint32_t p, uint32_t c), (p, c));

// 0x80166330 -> DVDLowRead (And 0x80165708 UnencryptedRead)
// The game calls this to read the Disk Header (offset 0) or raw data.
extern "C" int32_t DVDLowRead_80166330(uint32_t buffer, uint32_t length, uint32_t offset, uint32_t callback)
{
    const auto finish = [callback](bool succeeded) {
        const auto completion = DvdReadContract::CompletionFor(succeeded);
        InvokeDvdLowCallback(callback, completion.callbackResult);
        CompleteDvdCancelState();
        return completion.returnValue;
    };

    if (length == 0) {
        return finish(true);
    }

    if (offset == 0 && length >= 0x20) {
        if (!Memory::Contains(buffer, length)) {
            ReportDvdReadError("<synthetic disc header>", offset, length,
                               "DVD read destination is outside guest memory");
            return finish(false);
        }
        Memory::Write32(buffer + 0x00, CurrentDiscGameCode());
        Memory::Write16(buffer + 0x04, 0x3031);     // 01
        GxNotifyGuestRamDmaWrite(buffer, length);
        return finish(true);
    }

    AbsReadResult readInfo;
    if (!ResolveAbsRead(offset, length, readInfo)) {
        ReportDvdReadError("<unmapped DVD offset>", offset, length,
                           "absolute DVD read offset is not mapped to a host file");
        return finish(false);
    }
    if (readInfo.readLength != length) {
        ReportDvdReadError(HostPathText(readInfo.entry->hostPath), readInfo.fileOffset, length,
                           "requested range extends beyond the indexed DVD file");
        return finish(false);
    }
    if (!Memory::Contains(buffer, length)) {
        ReportDvdReadError(HostPathText(readInfo.entry->hostPath), readInfo.fileOffset, length,
                           "DVD read destination is outside guest memory");
        return finish(false);
    }

    std::vector<uint8_t> tempBuf;
    DvdReadContract::HostReadFailure failure;
    if (!DvdReadContract::ReadExact(readInfo.entry->hostPath,
                                    readInfo.fileOffset,
                                    readInfo.readLength,
                                    tempBuf,
                                    failure)) {
        ReportDvdReadError(HostPathText(readInfo.entry->hostPath), readInfo.fileOffset,
                           readInfo.readLength, DvdReadContract::Describe(failure));
        return finish(false);
    }

    CopyToGuestAsDma(buffer, tempBuf.data(), tempBuf.size());
    return finish(true);
}
PPC_NATIVE_OVERRIDE(80166330, DVDLowRead_80166330, int32_t, (uint32_t b, uint32_t l, uint32_t o, uint32_t c), (b, l, o, c));

// UnencryptedRead has the same completion and failure contract as DVDLowRead.
extern "C" int32_t DVDLowUnencryptedRead_80165708(uint32_t b, uint32_t l, uint32_t o, uint32_t c) {
    return DVDLowRead_80166330(b, l, o, c);
}
PPC_NATIVE_OVERRIDE(80165708, DVDLowUnencryptedRead_80165708, int32_t, (uint32_t b, uint32_t l, uint32_t o, uint32_t c), (b, l, o, c));

// ============================================================================
// Other Necessary Stubs
// ============================================================================

extern "C" int32_t DVDCheckDevice_801643FC() { return 1; } // Ready
PPC_NATIVE_OVERRIDE(801643FC, DVDCheckDevice_801643FC, int32_t, (), ());

extern "C" int32_t DVDLowClearCoverInterrupt_80166964(uint32_t cb) {
    // (the callback is the completion: a caller may wait for it)
    InvokeDvdLowCallback(cb, 1);
    return 1;
}
PPC_NATIVE_OVERRIDE(80166964, DVDLowClearCoverInterrupt_80166964, int32_t, (uint32_t cb), (cb));

