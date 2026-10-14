#include "pch.h"

#include "MfgUnlock.h"

#include <Config.h>
#include <Util.h>
#include <scanner/scanner.h>
#include <misc/IdentifyGpu.h>

#include "MfgUnlockFlip.h"
#include "MfgUnlockPlugin.h"

#include <atomic>
#include <map>
#include <mutex>
#include <tlhelp32.h>

namespace
{
// mov ebx,1 / mov r8d,3 / cmp edi,0x1b0 / cmovl r8d,ebx. The two counts and the architecture
// constant together are unique in the module; the wildcards cover nothing, they are here only to
// keep the shape readable.
constexpr std::string_view kAdvertisePattern = "BB 01 00 00 00 41 B8 03 00 00 00 81 FF B0 01 00 00 44 0F 4C C3";

// cmp eax,0x1b0 / jl / cmp ebx,3 / jbe. The only comparison against the architecture constant that
// is followed by a signed branch and a count test.
constexpr std::string_view kValidatePattern = "3D B0 01 00 00 7C ? 83 FB 03 76";

// Five generated frames, the count both patched sites carry.
constexpr uint8_t kMaxGeneratedFrames = 5;

// 310.9 restructured both gates. The count is no longer an immediate next to the comparison: the
// Blackwell branch starts at five and reads a configured value, and anything below Blackwell is sent
// to a branch that publishes one.
//     cmp ebp, 0x1b0
//     jl  ada          <- neutralised, so every card takes the Blackwell branch
//     mov edi, 0x5
constexpr std::string_view kAdvertisePattern309 = "81 FD B0 01 00 00 0F 8C ? ? ? ? BF 05 00 00 00";

// The capability flag in the same build is a setae rather than a branch.
//     cmp   eax, 0x1b0
//     setae al
constexpr std::string_view kValidatePattern309 = "3D B0 01 00 00 0F 93 C0";

// Bytes a pattern spans: one per space-separated token, wildcards included.
constexpr size_t PatternLength(std::string_view pattern)
{
    size_t tokens = 0;
    bool inToken = false;

    for (const char c : pattern)
    {
        if (c == ' ')
            inToken = false;
        else if (!inToken)
        {
            inToken = true;
            ++tokens;
        }
    }

    return tokens;
}

static_assert(PatternLength(kAdvertisePattern309) == 17 && PatternLength(kAdvertisePattern) == 21);

// The first match, or 0 when there is none or more than one: a second match means the pattern no longer
// identifies the code it was written for.
uintptr_t UniqueAddress(HMODULE module, std::string_view pattern)
{
    const auto first = scanner::GetAddress(module, pattern);
    return first && !scanner::GetAddress(module, pattern, 0, first + 1) ? first : 0;
}

// Both gates of one build, located before either is written, so a module is patched whole or not at all.
struct Gates
{
    uintptr_t advertise = 0;
    uintptr_t validate = 0;
    bool shape309 = false;
};

bool LocateGates(HMODULE module, Gates& gates)
{
    gates = {};

    if (const auto advertise = UniqueAddress(module, kAdvertisePattern309),
        validate = UniqueAddress(module, kValidatePattern309);
        advertise != 0 && validate != 0)
    {
        gates = { advertise, validate, true };
        return true;
    }

    if (const auto advertise = UniqueAddress(module, kAdvertisePattern),
        validate = UniqueAddress(module, kValidatePattern);
        advertise != 0 && validate != 0)
    {
        gates = { advertise, validate, false };
        return true;
    }

    return false;
}

// The DLSS-G provider, if it is mapped. The file name finds the game's own copy. The driver's OTA copy
// (models\dlssg\versions\<n>\files\<hash>.bin) and a renamed snippet are found by walking the loaded
// modules, which is only safe from an ordinary thread: a snapshot taken under the loader lock
// deadlocks. The load hook hands TryApply its module directly and never comes here.
//
// TryApply runs on every Streamline call until the snippet is found, so the walk is rate limited. A
// provider that has not appeared after these few walks is not going to appear through this route.
// Adapted from Susemi (3def828d).
HMODULE FindProvider()
{
    if (auto module = GetModuleHandleW(L"nvngx_dlssg.dll"))
        return module;

    static std::atomic<uint64_t> nextWalk { 0 };
    static std::atomic<unsigned> walks { 0 };
    constexpr unsigned kMaxWalks = 8;
    constexpr uint64_t kWalkIntervalMs = 2000;

    const uint64_t now = GetTickCount64();

    if (walks.load() >= kMaxWalks || now < nextWalk.load())
        return nullptr;

    nextWalk.store(now + kWalkIntervalMs);
    ++walks;

    // The marker string is a literal in this DLL, so it has to be excluded from the walk.
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&FindProvider), &self);

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());

    if (snapshot == INVALID_HANDLE_VALUE)
        return nullptr;

    HMODULE found = nullptr;
    MODULEENTRY32W entry {};
    entry.dwSize = sizeof(entry);

    for (bool more = Module32FirstW(snapshot, &entry); more && found == nullptr; more = Module32NextW(snapshot, &entry))
    {
        if (entry.hModule == self)
            continue;

        if (MfgUnlock::Provider::IsProviderPath(entry.szExePath))
        {
            found = entry.hModule;
            continue;
        }

        // A renamed or relocated snippet: only look inside modules that expose an NGX snippet entry point.
        // Every copy seen counts towards the fail-closed ceiling, so DLSS 5 Super Resolution providers,
        // which expose DirectSR, are kept out even though they carry the same NGX exports.
        if (GetProcAddress(entry.hModule, "NVSDK_NGX_D3D12_PopulateDeviceParameters_Impl") == nullptr &&
            GetProcAddress(entry.hModule, "NVSDK_NGX_VULKAN_PopulateDeviceParameters_Impl") == nullptr)
            continue;

        if (GetProcAddress(entry.hModule, "NVSDK_NGX_DirectSR_Create") != nullptr)
            continue;

        if (MfgUnlock::Provider::ImageContains(entry.hModule, MfgUnlock::Provider::kMarker))
            found = entry.hModule;
    }

    CloseHandle(snapshot);

    if (found != nullptr)
    {
        wchar_t path[MAX_PATH] {};
        GetModuleFileNameW(found, path, MAX_PATH);
        LOG_INFO("MFG unlock: DLSS-G provider found by module walk: {}", wstring_to_string(path));
    }

    return found;
}

// scanner::GetAddress only walks sections marked executable. Fatbins are data, so they need their own
// search. Returns 0 unless exactly one non-executable section holds the sequence, once.
uintptr_t FindDataBytes(HMODULE module, const uint8_t* needle, size_t length)
{
    auto base = reinterpret_cast<uint8_t*>(module);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    auto section = IMAGE_FIRST_SECTION(nt);

    uintptr_t found = 0;
    size_t hits = 0;

    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
    {
        const auto& s = section[i];

        if (s.Characteristics & IMAGE_SCN_MEM_EXECUTE)
            continue;

        uint8_t* start = base + s.VirtualAddress;
        uint8_t* end = start + s.Misc.VirtualSize;

        for (uint8_t* p = std::search(start, end, needle, needle + length); p != end;
             p = std::search(p + 1, end, needle, needle + length))
        {
            found = reinterpret_cast<uintptr_t>(p);

            if (++hits > 1)
                return 0;
        }
    }

    return hits == 1 ? found : 0;
}

// Aggregate state lives with the patch map below (g_aggregate).

// The module's own file version, for the report. A signature that does not match is expected on a
// version nobody has looked at, and the version is the one thing that makes such a report actionable.
std::string ModuleVersion(HMODULE module)
{
    wchar_t path[MAX_PATH] {};

    if (GetModuleFileNameW(module, path, MAX_PATH) == 0)
        return {};

    version_t file {};
    version_t product {};

    if (!Util::GetFileVersion(path, &file, &product))
        return {};

    return std::format("{}.{}.{}", file.major, file.minor, file.patch);
}

bool WriteBytes(uintptr_t address, const uint8_t* bytes, size_t count)
{
    DWORD oldProtect = 0;

    if (!VirtualProtect((LPVOID) address, count, PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        LOG_WARN("VirtualProtect failed at {:X}", address);
        return false;
    }

    std::memcpy((void*) address, bytes, count);

    DWORD ignored = 0;
    VirtualProtect((LPVOID) address, count, oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), (LPCVOID) address, count);

    return true;
}

std::string Hex(const uint8_t* bytes, size_t count)
{
    std::string out;

    for (size_t i = 0; i < count; ++i)
        out += std::format("{}{:02X}", i == 0 ? "" : " ", bytes[i]);

    return out;
}

// Rewrites count and neutralises the architecture clamp, so MultiFrameCountMax is published as five.
bool PatchAdvertise(const Gates& gates)
{
    if (gates.shape309)
    {
        // The jl is a rel32, six bytes.
        const auto branchAt = gates.advertise + 6;
        const uint8_t nop[] = { 0x0F, 0x1F, 0x44, 0x00, 0x00, 0x90 };

        LOG_INFO("MFG unlock: advertise (310.9) at {:X}, jl {} -> {}", gates.advertise,
                 Hex((const uint8_t*) branchAt, sizeof(nop)), Hex(nop, sizeof(nop)));

        return WriteBytes(branchAt, nop, sizeof(nop));
    }

    const auto address = gates.advertise;

    // Offsets within the matched sequence: the r8d immediate, and the cmovl.
    const auto countAt = address + 7;
    const auto cmovAt = address + 17;

    const uint8_t count[] = { kMaxGeneratedFrames };
    const uint8_t nop[] = { 0x0F, 0x1F, 0x40, 0x00 };

    LOG_INFO("MFG unlock: advertise at {:X}, count {} -> {}, cmovl {} -> {}", address,
             *(const uint8_t*) countAt, kMaxGeneratedFrames, Hex((const uint8_t*) cmovAt, sizeof(nop)),
             Hex(nop, sizeof(nop)));

    return WriteBytes(countAt, count, sizeof(count)) && WriteBytes(cmovAt, nop, sizeof(nop));
}

// Drops the Ada branch and raises the accepted count, so a request for five is not rejected.
bool PatchValidate(const Gates& gates)
{
    if (gates.shape309)
    {
        // setae al -> mov al, 1, so the flag is set whatever the architecture reports.
        const auto setAt = gates.validate + 5;
        const uint8_t always[] = { 0xB0, 0x01, 0x90 };

        LOG_INFO("MFG unlock: validate (310.9) at {:X}, setae {} -> {}", gates.validate,
                 Hex((const uint8_t*) setAt, sizeof(always)), Hex(always, sizeof(always)));

        return WriteBytes(setAt, always, sizeof(always));
    }

    const auto address = gates.validate;

    // Offsets within the matched sequence: the jl, and the immediate of the count test behind it.
    const auto branchAt = address + 5;
    const auto countAt = address + 9;

    const uint8_t nop[] = { 0x90, 0x90 };
    const uint8_t count[] = { kMaxGeneratedFrames };

    LOG_INFO("MFG unlock: validate at {:X}, jl {} -> {}, count {} -> {}", address,
             Hex((const uint8_t*) branchAt, sizeof(nop)), Hex(nop, sizeof(nop)), *(const uint8_t*) countAt,
             kMaxGeneratedFrames);

    return WriteBytes(branchAt, nop, sizeof(nop)) && WriteBytes(countAt, count, sizeof(count));
}


// Gives Ada the Blackwell kernels the module already carries.
//
// nvngx_dlssg.dll ships two builds of the interpolation kernels. Kernel_EstimateIntermMvecsScatter
// reads three f32 fields of its parameter block on sm_120 and one on sm_89, so on Ada every generated
// frame is placed at the same point between the two real ones: the world does not advance between
// them while the interface, composited once per present, does. At 2X there is one frame and nothing
// to distinguish; above it that is the whole symptom.
//
// The sm_120 module uses no instruction Ada lacks. So per container: the Blackwell PTX image is
// relabelled sm_89, its .target directive is rewritten in place (".target sm_120" and
// ".target sm_89 " are both fourteen bytes, and the directive sits in the literal run at the head of
// the LZ4 stream), and the images that were sm_89 -- the Ada PTX and its SASS -- are relabelled to an
// architecture that does not exist so the driver cannot select them. The driver then JITs Blackwell's
// kernel when it asks for Ada's.
//
// Nothing is copied in and no payload changes length. A container without both images is left alone.
constexpr uint32_t kArchAda = 89;
constexpr uint32_t kArchBlackwell = 120;

// No such shader model. Parks an image where nothing will ask for it.
constexpr uint32_t kArchParked = 122;

// Offsets inside a fatbin image header: payload length, and the architecture the image answers for.
constexpr size_t kImagePayloadSize = 8;
constexpr size_t kImageArch = 28;

unsigned int RewriteBlackwellKernels(HMODULE module)
{
    auto base = reinterpret_cast<uint8_t*>(module);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    auto section = IMAGE_FIRST_SECTION(nt);

    const uint8_t magic[] = { 0x50, 0xED, 0x55, 0xBA };
    unsigned int rewritten = 0;

    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
    {
        const auto& s = section[i];

        if (s.Characteristics & IMAGE_SCN_MEM_EXECUTE)
            continue;

        uint8_t* start = base + s.VirtualAddress;
        uint8_t* end = start + s.Misc.VirtualSize;

        for (uint8_t* c = std::search(start, end, magic, magic + sizeof(magic)); c < end;
             c = std::search(c + 1, end, magic, magic + sizeof(magic)))
        {
            if (end - c < 16)
                break;

            const auto headerSize = *reinterpret_cast<const uint16_t*>(c + 6);
            const auto fatSize = *reinterpret_cast<const uint64_t*>(c + 8);

            if (headerSize != 0x10 || fatSize == 0 || fatSize > (uint64_t) (end - c - 16))
                continue;

            uint8_t* blackwell = nullptr;
            size_t blackwellHeader = 0;
            size_t blackwellPayload = 0;
            std::vector<uint8_t*> ada;
            bool valid = true;

            for (uint8_t* image = c + 16; image < c + 16 + fatSize;)
            {
                // Every header field is read only after it is known to lie inside the container, and a
                // container whose images do not tile it exactly is left alone (bounds from Susemi 3def828d).
                const auto remaining = (uint64_t) (c + 16 + fatSize - image);
                if (remaining < kImageArch + sizeof(uint32_t))
                {
                    valid = false;
                    break;
                }

                const auto kind = *reinterpret_cast<const uint16_t*>(image);
                const auto imageHeader = *reinterpret_cast<const uint32_t*>(image + 4);
                const auto payload = *reinterpret_cast<const uint64_t*>(image + kImagePayloadSize);
                const auto arch = *reinterpret_cast<const uint32_t*>(image + kImageArch);

                if (imageHeader < kImageArch + sizeof(uint32_t) || imageHeader > remaining || payload == 0 ||
                    payload > remaining - imageHeader)
                {
                    valid = false;
                    break;
                }

                // kind 1 is PTX, 2 is a cubin. Only the PTX can be retargeted; the cubin is parked.
                if (kind == 1 && arch == kArchBlackwell)
                {
                    blackwell = image;
                    blackwellHeader = imageHeader;
                    blackwellPayload = payload;
                }
                else if (arch == kArchAda)
                {
                    ada.push_back(image);
                }

                image += imageHeader + payload;
            }

            if (!valid || blackwell == nullptr || ada.empty())
                continue;

            const char from[] = ".target sm_120";
            const char to[] = ".target sm_89 ";
            static_assert(sizeof(from) == sizeof(to), "the directive rewrite must not change length");

            uint8_t* body = blackwell + blackwellHeader;
            uint8_t* bodyEnd = body + blackwellPayload;
            auto at = std::search(body, bodyEnd, from, from + sizeof(from) - 1);

            if (at == bodyEnd)
                continue;

            const uint32_t ada89 = kArchAda;
            const uint32_t parked = kArchParked;

            // Prepare the whole container first and write it once. A failed protection change must not
            // leave its PTX target and architecture headers disagreeing, or count a partial rewrite.
            std::vector<uint8_t> patched(c, c + 16 + fatSize);
            std::memcpy(patched.data() + (at - c), to, sizeof(to) - 1);
            std::memcpy(patched.data() + (blackwell + kImageArch - c), &ada89, sizeof(ada89));

            for (uint8_t* image : ada)
                std::memcpy(patched.data() + (image + kImageArch - c), &parked, sizeof(parked));

            if (WriteBytes(reinterpret_cast<uintptr_t>(c), patched.data(), patched.size()))
                ++rewritten;
        }
    }

    LOG_INFO("MFG unlock: {} kernel containers answer Ada with the Blackwell image", rewritten);

    return rewritten;
}
} // namespace

static void PruneUnloadedCopies();

void MfgUnlock::TryApply()
{
    if (!Config::Instance()->FGDLSSGAdaMfgUnlock.value_or_default())
        return;

    // While the ceiling is shut by a copy that is not fully patched, check now and then whether that copy
    // is still loaded. Unloads that go around the FreeLibrary hooks are caught here; nothing runs once the
    // ceiling is open or before any copy was seen.
    if (AnyModuleSeen() && UnlockedMax() == 0)
    {
        static std::atomic<uint64_t> nextPrune { 0 };
        constexpr uint64_t kPruneIntervalMs = 1000;

        const uint64_t now = GetTickCount64();
        if (auto due = nextPrune.load(); now >= due && nextPrune.compare_exchange_strong(due, now + kPruneIntervalMs))
            PruneUnloadedCopies();
    }

    if (auto module = FindProvider(); module != nullptr)
        TryApply(module);
}

// One record per DLSS-G copy the unlock has processed and that is still mapped.
struct CopyRecord
{
    MfgUnlock::Status status;
    std::wstring path;

    // The bytes of the advertise gate right after it was patched. A fresh mapping of the same file at the
    // same address no longer carries them, which tells a reload apart from a repeated load of one mapping.
    uintptr_t markAt = 0;
    std::vector<uint8_t> mark;
};

static std::mutex g_patchMutex;
static std::map<HMODULE, CopyRecord> g_patched; // completed attempts on copies still mapped
static MfgUnlock::Status g_aggregate {};
static std::atomic<unsigned int> g_publishedMax { 0 };
static std::atomic<bool> g_anyModule { false };
// Reading a DLL's version resources can re-enter our LoadLibrary hooks for that same DLL.
// Keep a per-thread stack so a nested TryApply for an already in-flight module returns instead
// of recursively entering ModuleVersion again. Other threads are still free to race as before.
static thread_local std::vector<HMODULE> g_patchStack;

static std::wstring ModulePath(HMODULE module)
{
    wchar_t path[MAX_PATH] {};
    return GetModuleFileNameW(module, path, MAX_PATH) != 0 ? std::wstring(path) : std::wstring(L"?");
}

// Compares bytes that may belong to a module another thread is unloading; an unmapped page reads as a
// mismatch. No C++ objects here, so structured exception handling is allowed.
static bool BytesEqual(uintptr_t at, const uint8_t* expected, size_t length)
{
    __try
    {
        return std::memcmp(reinterpret_cast<const void*>(at), expected, length) == 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// Whether the loader still has this exact copy mapped: the handle resolves to itself and to the same file.
static bool StillMapped(HMODULE module, const std::wstring& path)
{
    HMODULE current = nullptr;

    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(module), &current) ||
        current != module)
        return false;

    return ModulePath(module) == path;
}

static bool PublishCopy(HMODULE module, CopyRecord record);
static void AggregateLocked();
static void PruneUnloadedCopies();
static void PatchPluginCeilings();

void MfgUnlock::TryApply(HMODULE module)
{
    if (!Config::Instance()->FGDLSSGAdaMfgUnlock.value_or_default() || module == nullptr)
        return;

    {
        std::lock_guard<std::mutex> lock(g_patchMutex);

        if (auto it = g_patched.find(module); it != g_patched.end())
        {
            // Same handle seen before. While the patched bytes are still there it is the same mapping and
            // there is nothing to do (concurrent duplicates converge in PublishCopy). If they are gone, the
            // loader unmapped the copy and mapped a fresh image at the same address: the record is stale
            // and the copy is patched again below.
            const auto& copy = it->second;

            if (copy.mark.empty() || BytesEqual(copy.markAt, copy.mark.data(), copy.mark.size()))
                return;

            LOG_INFO("MFG unlock: {} was mapped again at the same address; patching the fresh image",
                     wstring_to_string(copy.path));
            g_patched.erase(it);
            AggregateLocked();
        }
    }

    if (std::find(g_patchStack.begin(), g_patchStack.end(), module) != g_patchStack.end())
    {
        LOG_TRACE("MFG unlock: reentrant attempt skipped for module {:p}", (void*) module);
        return;
    }

    g_patchStack.push_back(module);
    struct PatchStackGuard
    {
        std::vector<HMODULE>& stack;

        ~PatchStackGuard() { stack.pop_back(); }
    } patchStackGuard { g_patchStack };

    // Everything below runs OUTSIDE the mutex: version/file APIs can pull in system DLLs on
    // first use, and GPU discovery touches NVAPI. Holding our lock across those while a nested
    // loader callback waits on it would invert the lock order (loader lock -> patch mutex here
    // vs patch mutex -> loader lock there). Patch writes themselves are idempotent, so a raced
    // duplicate attempt is redundant work, never corruption; the first finisher wins the insert.
    const auto pathW = ModulePath(module);
    const auto pathA = wstring_to_string(pathW);
    LOG_INFO("MFG unlock: begin module {:p} ({})", (void*) module, pathA);

    LOG_INFO("MFG unlock: reading module version...");
    const std::string version = ModuleVersion(module);
    LOG_INFO("MFG unlock: module version {}", version);

    // Both gates are located, each exactly once, before anything is written: a version that matches
    // one and not the other is left untouched rather than half patched.
    LOG_INFO("MFG unlock: locating frame-count gates...");
    Gates gates {};
    const bool gatesKnown = LocateGates(module, gates);
    LOG_INFO("MFG unlock: gates {} ({})", gatesKnown ? "located" : "not located",
             gatesKnown ? (gates.shape309 ? "310.9 shape" : "legacy shape") : "unsupported or ambiguous");

    // Default on where it applies: below Blackwell the unlock alone produces frames that do
    // not advance the picture, so the two belong together. dlssCapable is set from the same
    // field, so an architecture that never reported leaves this off.
    const auto& gpu = IdentifyGpu::getPrimaryGpu();
    const bool preBlackwell = gpu.vendorId == VendorId::Nvidia &&
                              gpu.nvidiaArchInfo.architecture_id >= NV_GPU_ARCHITECTURE_TU100 &&
                              gpu.nvidiaArchInfo.architecture_id <= NV_GPU_ARCHITECTURE_AD100;

    unsigned int kernels = 0;
    bool kernelsMissing = false;
    bool advertise = false;
    bool validate = false;

    const bool useBlackwellKernels =
        Config::Instance()->FGDLSSGAdaBlackwellKernels.value_or(preBlackwell);
    LOG_INFO("MFG unlock: AdaBlackwellKernels effective value: {}", useBlackwellKernels);

    if (!gatesKnown)
    {
        LOG_WARN("MFG unlock: unsupported or ambiguous DLSS-G {} signatures; left unchanged ({})", version, pathA);
    }
    else
    {
        if (useBlackwellKernels)
            kernels = RewriteBlackwellKernels(module);

        // With retargeting asked for, the kernels and the gates form one feature: opened gates without
        // the Blackwell kernel put every generated frame on the midpoint, which looks worse than 2X.
        if (useBlackwellKernels && kernels == 0)
        {
            kernelsMissing = true;
            LOG_WARN("MFG unlock: no compatible interpolation kernels; frame-count gates left unchanged ({})", pathA);
        }
        else
        {
            advertise = PatchAdvertise(gates);
            validate = PatchValidate(gates);
        }
    }

    if (advertise && validate)
        LOG_INFO("MFG unlock: nvngx_dlssg.dll patched for {} generated frames ({})", kMaxGeneratedFrames,
                 pathA);
    else
        LOG_WARN("MFG unlock: nvngx_dlssg.dll incomplete, advertise {}, validate {} ({})", advertise, validate,
                 pathA);

    CopyRecord record {};
    record.status.ModuleFound = true;
    record.status.SnippetVersion = version;
    record.status.AdvertiseMatched = advertise;
    record.status.ValidateMatched = validate;
    record.status.KernelsRewritten = kernels;
    record.status.KernelsMissing = kernelsMissing;
    record.path = pathW;

    if (advertise)
    {
        // The span the advertise pattern covers, which includes every byte its patch wrote.
        const auto span = PatternLength(gates.shape309 ? kAdvertisePattern309 : kAdvertisePattern);
        const auto* start = reinterpret_cast<const uint8_t*>(gates.advertise);
        record.markAt = gates.advertise;
        record.mark.assign(start, start + span);
    }

    if (!PublishCopy(module, std::move(record)))
        return; // lost the race; the winner's results stand

    // A plugin that was loaded first has been waiting for the snippet.
    PatchPluginCeilings();
}

static bool PublishCopy(HMODULE module, CopyRecord record)
{
    std::lock_guard<std::mutex> lock(g_patchMutex);
    if (g_patched.contains(module))
        return false;

    g_patched.emplace(module, std::move(record));
    AggregateLocked();

    return true;
}

// Fail closed: the ceiling opens only when at least one copy is mapped and EVERY mapped copy is fully
// patched. A complete copy next to an unpatched one reports 0, because the serving copy cannot be
// identified and a split success is not a success. Copies the loader has unmapped are no longer in
// g_patched (PruneUnloadedCopies): one that is gone cannot be serving frames. Caller holds g_patchMutex.
static void AggregateLocked()
{
    bool allComplete = !g_patched.empty();
    bool anyKernelsMissing = false;
    unsigned int bestKernels = 0;
    unsigned int complete = 0;
    std::string firstComplete;
    std::string firstAny;
    std::string unmatched;

    for (const auto& [handle, copy] : g_patched)
    {
        const auto& st = copy.status;
        const bool whole = st.AdvertiseMatched && st.ValidateMatched;
        allComplete = allComplete && whole;
        anyKernelsMissing = anyKernelsMissing || st.KernelsMissing;

        if (whole)
        {
            ++complete;
            if (firstComplete.empty())
                firstComplete = st.SnippetVersion;
        }
        else if (unmatched.empty() && !st.SnippetVersion.empty())
        {
            unmatched = st.SnippetVersion;
        }

        if (st.KernelsRewritten > bestKernels)
            bestKernels = st.KernelsRewritten;
        if (firstAny.empty() && !st.SnippetVersion.empty())
            firstAny = st.SnippetVersion;
    }

    g_aggregate.ModuleFound = !g_patched.empty();
    g_aggregate.SnippetVersion = firstComplete.empty() ? firstAny : firstComplete;
    g_aggregate.UnmatchedVersion = unmatched;
    g_aggregate.AdvertiseMatched = allComplete;
    g_aggregate.ValidateMatched = allComplete;
    g_aggregate.KernelsRewritten = bestKernels;
    g_aggregate.KernelsMissing = anyKernelsMissing;
    g_aggregate.CopiesSeen = static_cast<unsigned int>(g_patched.size());
    g_aggregate.CopiesComplete = complete;
    g_anyModule.store(true, std::memory_order_release);
    g_publishedMax.store(allComplete ? kMaxGeneratedFrames : 0, std::memory_order_release);
}

// Drops the copies the loader has since unmapped. The driver probes its own DriverStore snippet at
// start-up and unloads it again; left in, that copy would keep the ceiling shut for the whole session.
// Loader queries run outside g_patchMutex (see TryApply), so the list is read, checked, then trimmed.
static void PruneUnloadedCopies()
{
    std::vector<std::pair<HMODULE, std::wstring>> copies;

    {
        std::lock_guard<std::mutex> lock(g_patchMutex);
        for (const auto& [handle, copy] : g_patched)
            copies.emplace_back(handle, copy.path);
    }

    std::vector<HMODULE> gone;
    for (const auto& [handle, path] : copies)
    {
        if (!StillMapped(handle, path))
            gone.push_back(handle);
    }

    if (gone.empty())
        return;

    {
        std::lock_guard<std::mutex> lock(g_patchMutex);

        for (HMODULE handle : gone)
        {
            if (auto it = g_patched.find(handle); it != g_patched.end())
            {
                LOG_INFO("MFG unlock: {} ({}) is no longer loaded and no longer counts towards the ceiling",
                         wstring_to_string(it->second.path), it->second.status.SnippetVersion);
                g_patched.erase(it);
            }
        }

        AggregateLocked();
    }

    // The ceiling may have just opened; a plugin seen earlier has been waiting for that.
    PatchPluginCeilings();
}

void MfgUnlock::OnModuleReleased(HMODULE module)
{
    if (module == nullptr)
        return;

    {
        std::lock_guard<std::mutex> lock(g_patchMutex);
        if (!g_patched.contains(module))
            return;
    }

    PruneUnloadedCopies();
}

// The Streamline DLSS-G plugins seen so far, and the ones already tried for the ceiling. A plugin is
// tried once: the answer does not change, and a repeat would only repeat the log line. Kept apart from
// g_patchMutex, which is never held while this one is taken or the other way round.
static std::mutex g_pluginMutex;
static std::vector<HMODULE> g_plugins;
static std::vector<HMODULE> g_pluginsTried;
static const char* g_pluginCeiling = "";
static const char* g_flipMetering = "";
static unsigned int g_flipSites = 0;

// Same guards as the snippet unlock, narrowed to Ada: the plugin patches only make sense where the
// snippet's own gates were opened for a card NVIDIA caps at one generated frame.
static bool AdaUnlockWanted()
{
    if (!Config::Instance()->FGDLSSGAdaMfgUnlock.value_or_default())
        return false;

    const auto& gpu = IdentifyGpu::getPrimaryGpu();
    return gpu.vendorId == VendorId::Nvidia && gpu.nvidiaArchInfo.architecture_id == NV_GPU_ARCHITECTURE_AD100;
}

// Software frame pacing: pin the plugin's flip-metering state to its own software fallback. Applied when
// the plugin is loaded, before Streamline uses it, because rewriting a register store is a seven byte
// write into code that no other thread should be executing yet. Adapted from Susemi (3def828d).
//
// g_pluginMutex only guards the shared state: module queries and writes run outside it, because this file
// can also be reached from the FreeLibrary hook, where the loader lock may be held.
static void PatchFlipMetering(HMODULE plugin)
{
    {
        std::lock_guard<std::mutex> lock(g_pluginMutex);

        if (std::string_view(g_flipMetering) == "patched")
            return;

        // A plugin has been seen; "off" says that, and that nothing was asked of it.
        if (!Config::Instance()->FGDLSSGAdaFlipMeteringPatch.value_or_default())
        {
            g_flipMetering = "off";
            return;
        }
    }

    const auto pluginPath = wstring_to_string(ModulePath(plugin));

    MfgUnlock::Flip::Plan plan;
    const auto found = MfgUnlock::Flip::FindPlan(plugin, plan);
    const char* state = "not applied";

    if (found != MfgUnlock::Flip::FindResult::Found)
    {
        state = MfgUnlock::Flip::Describe(found);
        LOG_WARN("MFG unlock: {}: software frame pacing not applied: {}", pluginPath, state);
    }
    else
    {
        switch (MfgUnlock::Flip::Apply(plan))
        {
        case MfgUnlock::Flip::ApplyResult::Patched:
            state = "patched";
            LOG_INFO("MFG unlock: {}: flip-metering state +0x{:X} pinned to {} at {} site(s); multi-frame should pace "
                     "in software",
                     pluginPath, plan.field, plan.value, plan.sites.size());
            break;
        case MfgUnlock::Flip::ApplyResult::Mismatch:
            state = "the plugin changed while it was being patched";
            LOG_WARN("MFG unlock: {}: software frame pacing not applied: {}", pluginPath, state);
            break;
        case MfgUnlock::Flip::ApplyResult::ProtectFailed:
            state = "its memory could not be made writable";
            LOG_WARN("MFG unlock: {}: software frame pacing not applied: {}", pluginPath, state);
            break;
        }
    }

    std::lock_guard<std::mutex> lock(g_pluginMutex);

    if (std::string_view(g_flipMetering) == "patched")
        return;

    g_flipMetering = state;

    if (std::string_view(state) == "patched")
        g_flipSites = static_cast<unsigned int>(plan.sites.size());
}

// Only once the snippet unlock has landed. Raising the plugin's ceiling while the snippet still answers
// Ada's 1 would only move the rejection from the plugin to the snippet. Adapted from Susemi (3def828d).
static void PatchPluginCeilings()
{
    if (MfgUnlock::UnlockedMax() == 0)
        return;

    // Claim the plugins not tried yet; the module queries and the write happen outside the lock.
    std::vector<HMODULE> pending;

    {
        std::lock_guard<std::mutex> lock(g_pluginMutex);

        for (HMODULE plugin : g_plugins)
        {
            if (std::find(g_pluginsTried.begin(), g_pluginsTried.end(), plugin) != g_pluginsTried.end())
                continue;

            g_pluginsTried.push_back(plugin);
            pending.push_back(plugin);
        }
    }

    for (HMODULE plugin : pending)
    {
        const auto pluginPath = wstring_to_string(ModulePath(plugin));

        MfgUnlock::Plugin::CeilingSite site;
        const char* result = "not matched";

        switch (MfgUnlock::Plugin::FindCeilingSite(plugin, site))
        {
        case MfgUnlock::Plugin::FindResult::Found:
            switch (MfgUnlock::Plugin::ApplyCeilingPatch(site))
            {
            case MfgUnlock::Plugin::ApplyResult::Patched:
                result = "patched";
                LOG_INFO("MFG unlock: {}: frame-count clamp neutralised, compiled maximum {} generated frame(s)",
                         pluginPath, site.compiled);
                break;
            case MfgUnlock::Plugin::ApplyResult::ProtectFailed:
                result = "not writable";
                LOG_WARN("MFG unlock: {}: frame-count clamp found but its page could not be made writable", pluginPath);
                break;
            case MfgUnlock::Plugin::ApplyResult::Mismatch:
                LOG_WARN("MFG unlock: {}: frame-count clamp changed under us; left unchanged", pluginPath);
                break;
            }
            break;
        case MfgUnlock::Plugin::FindResult::Ambiguous:
            result = "ambiguous";
            LOG_WARN("MFG unlock: {}: more than one frame-count clamp; left unchanged", pluginPath);
            break;
        case MfgUnlock::Plugin::FindResult::None:
        case MfgUnlock::Plugin::FindResult::BadImage:
            LOG_WARN("MFG unlock: {}: no frame-count clamp of the known shape; left unchanged", pluginPath);
            break;
        }

        // A plugin that was patched stays patched; a second one that cannot be does not change that.
        std::lock_guard<std::mutex> lock(g_pluginMutex);
        if (std::string_view(g_pluginCeiling) != "patched")
            g_pluginCeiling = result;
    }
}

void MfgUnlock::OnStreamlinePluginLoaded(HMODULE plugin)
{
    if (plugin == nullptr || !AdaUnlockWanted())
        return;

    bool fresh = false;

    {
        std::lock_guard<std::mutex> lock(g_pluginMutex);

        if (std::find(g_plugins.begin(), g_plugins.end(), plugin) == g_plugins.end())
        {
            g_plugins.push_back(plugin);
            fresh = true;
        }
    }

    // At load, ahead of any use of the plugin.
    if (fresh)
        PatchFlipMetering(plugin);

    // A no-op until the snippet unlock has landed; TryApply calls it again then.
    PatchPluginCeilings();
}

bool MfgUnlock::SoftwarePacing()
{
    std::lock_guard<std::mutex> lock(g_pluginMutex);
    return std::string_view(g_flipMetering) == "patched";
}

size_t MfgUnlock::PatchedModuleCount()
{
    std::lock_guard<std::mutex> lock(g_patchMutex);
    return g_patched.size();
}

unsigned int MfgUnlock::UnlockedMax() { return g_publishedMax.load(std::memory_order_acquire); }

bool MfgUnlock::AnyModuleSeen() { return g_anyModule.load(std::memory_order_acquire); }

MfgUnlock::Status MfgUnlock::LastStatus()
{
    Status status;

    {
        std::lock_guard<std::mutex> lock(g_patchMutex);
        status = g_aggregate; // by value: a coherent snapshot, never a mutable shared reference
    }

    std::lock_guard<std::mutex> lock(g_pluginMutex);
    status.PluginCeiling = g_pluginCeiling;
    status.FlipMetering = g_flipMetering;
    status.FlipSites = g_flipSites;
    return status;
}
