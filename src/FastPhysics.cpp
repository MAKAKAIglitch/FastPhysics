// FastPhysics: swaps the Dantzig LCP solver of Scrap Mechanic's physics (btDantzigSolver::solveMLCP) for a faster
// build of the same algorithm. AVX2 kernels plus a warm start from the previous tick; the warm start is verified
// and repaired, so the answer is the exact one either way. Big creations (lots of bearings/pistons and parts
// pressing on each other in one island) spend 10-30 ms per tick in that solver, this brings it down to 1-3.
//
// Settings: FastPhysics.ini next to the DLL. Log: FastPhysics.log.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <intrin.h>

#include <cfloat>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <initializer_list>
#include <vector>

#include "fastlcp.h"
#include "version.h"

namespace {

HANDLE g_log = INVALID_HANDLE_VALUE;

bool PathNextToModule(HMODULE module, const wchar_t* fileName, wchar_t (&out)[MAX_PATH]) {
    DWORD len = GetModuleFileNameW(module, out, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return false;
    wchar_t* slash = wcsrchr(out, L'\\');
    if (!slash) return false;
    size_t room = MAX_PATH - static_cast<size_t>(slash + 1 - out);
    return wcscpy_s(slash + 1, room, fileName) == 0;
}

void Log(const char* fmt, ...) {
    if (g_log == INVALID_HANDLE_VALUE) return;
    char buf[1024];
    SYSTEMTIME t;
    GetLocalTime(&t);
    int len = snprintf(buf, sizeof(buf), "[%02u:%02u:%02u] ", t.wHour, t.wMinute, t.wSecond);
    va_list args;
    va_start(args, fmt);
    len += vsnprintf(buf + len, sizeof(buf) - static_cast<size_t>(len) - 2, fmt, args);
    va_end(args);
    if (len < 0) return;
    if (len > static_cast<int>(sizeof(buf)) - 3) len = sizeof(buf) - 3;
    buf[len++] = '\r';
    buf[len++] = '\n';
    DWORD written;
    WriteFile(g_log, buf, static_cast<DWORD>(len), &written, nullptr);
}

bool HashFileSha256(const wchar_t* path, char (&hex)[65]) {
    hex[0] = '\0';
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::vector<UCHAR> object;
    UCHAR digest[32] = {};
    DWORD objectLength = 0, hashLength = 0, bytes = 0;
    bool ok = false;

    NTSTATUS status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (status >= 0 &&
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectLength),
                          sizeof(objectLength), &bytes, 0) >= 0 &&
        BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashLength),
                          sizeof(hashLength), &bytes, 0) >= 0 &&
        hashLength == sizeof(digest)) {
        object.resize(objectLength);
        status = BCryptCreateHash(algorithm, &hash, object.data(), objectLength, nullptr, 0, 0);
        if (status >= 0) {
            UCHAR buffer[64 * 1024];
            for (;;) {
                DWORD read = 0;
                if (!ReadFile(file, buffer, sizeof(buffer), &read, nullptr)) break;
                if (read == 0) {
                    status = BCryptFinishHash(hash, digest, sizeof(digest), 0);
                    ok = status >= 0;
                    break;
                }
                status = BCryptHashData(hash, buffer, read, 0);
                if (status < 0) break;
            }
        }
    }

    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    CloseHandle(file);

    if (!ok) return false;
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(digest); ++i) {
        hex[i * 2] = digits[digest[i] >> 4];
        hex[i * 2 + 1] = digits[digest[i] & 0x0F];
    }
    hex[64] = '\0';
    return true;
}

bool IsSha256Hex(const char* value) {
    if (!value || strlen(value) != 64) return false;
    for (size_t i = 0; i < 64; ++i) {
        const char c = value[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}

struct Range {
    uint8_t* begin = nullptr;
    uint8_t* end = nullptr;
    bool Contains(const void* p) const { return p >= begin && p < end; }
};

uint8_t* g_base = nullptr;
Range g_text, g_rdata, g_data;

bool IsScrapMechanicExe() {
    wchar_t path[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return false;
    const wchar_t* name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    return _wcsicmp(name, L"ScrapMechanic.exe") == 0;
}

bool InitSections(uint8_t* exeBase) {
    g_base = exeBase;
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(g_base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(g_base + dos->e_lfanew);
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        char name[IMAGE_SIZEOF_SHORT_NAME + 1] = {};
        memcpy(name, sec->Name, IMAGE_SIZEOF_SHORT_NAME);
        Range r{g_base + sec->VirtualAddress, g_base + sec->VirtualAddress + sec->Misc.VirtualSize};
        if (strcmp(name, ".text") == 0) g_text = r;
        else if (strcmp(name, ".rdata") == 0) g_rdata = r;
        else if (strcmp(name, ".data") == 0) g_data = r;
    }
    return g_text.begin && g_rdata.begin && g_data.begin;
}

bool IsReadableRange(const void* pointer, size_t size) {
    if (!pointer || size == 0) return false;
    uintptr_t current = reinterpret_cast<uintptr_t>(pointer);
    if (current > UINTPTR_MAX - size) return false;
    const uintptr_t finish = current + size;
    while (current < finish) {
        MEMORY_BASIC_INFORMATION mbi = {};
        if (!VirtualQuery(reinterpret_cast<const void*>(current), &mbi, sizeof(mbi))) return false;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
        const DWORD protection = mbi.Protect & 0xFF;
        if (protection != PAGE_READONLY && protection != PAGE_READWRITE &&
            protection != PAGE_WRITECOPY && protection != PAGE_EXECUTE_READ &&
            protection != PAGE_EXECUTE_READWRITE && protection != PAGE_EXECUTE_WRITECOPY) return false;
        const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (regionEnd <= current) return false;
        current = regionEnd < finish ? regionEnd : finish;
    }
    return true;
}

bool IsWritableRange(const void* pointer, size_t size) {
    if (!pointer || size == 0) return false;
    uintptr_t current = reinterpret_cast<uintptr_t>(pointer);
    if (current > static_cast<uintptr_t>(-1) - size) return false;
    const uintptr_t finish = current + size;
    while (current < finish) {
        MEMORY_BASIC_INFORMATION mbi = {};
        if (!VirtualQuery(reinterpret_cast<const void*>(current), &mbi, sizeof(mbi))) return false;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
        const DWORD protection = mbi.Protect & 0xFF;
        if (protection != PAGE_READWRITE && protection != PAGE_WRITECOPY &&
            protection != PAGE_EXECUTE_READWRITE && protection != PAGE_EXECUTE_WRITECOPY) return false;
        const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (regionEnd <= current) return false;
        current = regionEnd < finish ? regionEnd : finish;
    }
    return true;
}

template <typename T>
T Read(const void* p) {
    T value;
    memcpy(&value, p, sizeof(T));
    return value;
}

uint8_t* FindBytes(const Range& r, const void* needle, size_t size, uint8_t* from) {
    for (uint8_t* p = from; p + size <= r.end; ++p) {
        if (memcmp(p, needle, size) == 0) return p;
    }
    return nullptr;
}

// MSVC RTTI walk: type descriptor name -> complete object locator -> vtable.
void** FindVTable(const char* mangledName) {
    const size_t nameSize = strlen(mangledName) + 1;
    for (const Range* r : {&g_data, &g_rdata}) {
        for (uint8_t* name = FindBytes(*r, mangledName, nameSize, r->begin); name;
             name = FindBytes(*r, mangledName, nameSize, name + 1)) {
            const uint32_t tdRva = static_cast<uint32_t>(name - 0x10 - g_base);
            for (uint8_t* col = g_rdata.begin; col + 24 <= g_rdata.end; col += 4) {
                if (Read<uint32_t>(col) != 1 || Read<uint32_t>(col + 4) != 0) continue;
                if (Read<uint32_t>(col + 12) != tdRva) continue;
                if (Read<uint32_t>(col + 20) != static_cast<uint32_t>(col - g_base)) continue;
                for (uint8_t* slot = g_rdata.begin; slot + 16 <= g_rdata.end; slot += 8) {
                    if (Read<uint8_t*>(slot) == col) return reinterpret_cast<void**>(slot + 8);
                }
            }
        }
    }
    return nullptr;
}

bool WriteProtected(void* at, const void* bytes, size_t size) {
    if (!at || !bytes || size == 0) return false;
    void* backup = HeapAlloc(GetProcessHeap(), 0, size);
    if (!backup) return false;
    memcpy(backup, at, size);

    DWORD oldProtect = 0;
    if (!VirtualProtect(at, size, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        HeapFree(GetProcessHeap(), 0, backup);
        return false;
    }

    memcpy(at, bytes, size);
    const BOOL flushed = FlushInstructionCache(GetCurrentProcess(), at, size);
    DWORD ignored = 0;
    const BOOL restored = VirtualProtect(at, size, oldProtect, &ignored);
    if (!flushed || !restored) {
        DWORD rollbackProtect = 0;
        if (VirtualProtect(at, size, PAGE_EXECUTE_READWRITE, &rollbackProtect)) {
            memcpy(at, backup, size);
            FlushInstructionCache(GetCurrentProcess(), at, size);
            VirtualProtect(at, size, oldProtect, &ignored);
        }
        HeapFree(GetProcessHeap(), 0, backup);
        return false;
    }
    HeapFree(GetProcessHeap(), 0, backup);
    return true;
}

bool PatchPointer(void** slot, void* hook, void** original) {
    if (!slot || !hook || !original) return false;
    *original = *slot;

    DWORD oldProtect = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) return false;

    // A vtable entry may be read concurrently by a game worker. Use an aligned atomic pointer
    // exchange so no thread can observe a partially-written function pointer.
    InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(slot), hook);
    const BOOL flushed = FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
    DWORD ignored = 0;
    const BOOL restored = VirtualProtect(slot, sizeof(void*), oldProtect, &ignored);

    if (!flushed || !restored) {
        DWORD rollbackProtect = 0;
        if (VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &rollbackProtect)) {
            InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(slot), *original);
            FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
            VirtualProtect(slot, sizeof(void*), oldProtect, &ignored);
        }
        return false;
    }
    return true;
}

uint8_t* FindUnique(const Range& r, const uint8_t* sig, size_t size) {
    uint8_t* first = FindBytes(r, sig, size, r.begin);
    if (!first) return nullptr;
    return FindBytes(r, sig, size, first + 1) ? nullptr : first;
}

// Counts exact or masked matches in a section. A zero mask byte is a wildcard.
// The diagnostic build uses this only for reporting; relaxed matches are never patched.
int CountPattern(const Range& r, const uint8_t* pattern, const uint8_t* mask, size_t size,
                 uint8_t** found, int capacity) {
    if (!r.begin || !r.end || !pattern || size == 0) return 0;
    int count = 0;
    for (uint8_t* p = r.begin; p + size <= r.end; ++p) {
        bool match = true;
        for (size_t i = 0; i < size; ++i) {
            if ((!mask || mask[i] != 0) && p[i] != pattern[i]) {
                match = false;
                break;
            }
        }
        if (!match) continue;
        if (found && count < capacity) found[count] = p;
        ++count;
    }
    return count;
}

void LogPatternCandidate(const char* label, int index, const uint8_t* at) {
    if (!at || !g_text.end || at >= g_text.end) return;
    size_t length = static_cast<size_t>(g_text.end - at);
    if (length > 64) length = 64;
    char hex[64 * 3] = {};
    size_t used = 0;
    for (size_t i = 0; i < length; ++i) {
        int written = snprintf(hex + used, sizeof(hex) - used, "%02X%s",
                               at[i], (i + 1 == length) ? "" : " ");
        if (written <= 0 || static_cast<size_t>(written) >= sizeof(hex) - used) break;
        used += static_cast<size_t>(written);
    }
    Log("%s candidate %d at RVA 0x%llX (%u bytes): %s",
        label, index, static_cast<unsigned long long>(at - g_base),
        static_cast<unsigned>(length), hex);
}

void LogPatternCandidates(const char* label, const uint8_t* pattern, const uint8_t* mask, size_t size) {
    uint8_t* found[3] = {};
    const int count = CountPattern(g_text, pattern, mask, size, found, 3);
    Log("%s: %d match(es)%s", label, count, count > 3 ? " (first 3 shown)" : "");
    for (int i = 0; i < count && i < 3; ++i) LogPatternCandidate(label, i + 1, found[i]);
}


// ---------------------------------------------------------------------------
// Two small fixes for big static builds. They have nothing to do with the solver, they just live here
// because one mod is easier to install than three.
//
//  - Reflection probes: every frame the camera moves, the probe generator finds the ground height under it
//    by sweeping a 32 m sphere 250 m down through the physics world. Next to a build of tens of thousands of
//    parts that one query costs 40-50 ms. A smaller sphere samples a narrower column and costs a few ms;
//    the probes end up anchored to the ground right under the camera instead of the highest surface within
//    32 m. ProbeSweepRadius is the radius in metres, 0 keeps the game's value.
//  - Bot navmesh: when a cell's static boxes are registered, every box walks the cell's whole box list to
//    check that its id is not there yet. The result only feeds a debug assert, and the walk is quadratic in
//    the boxes per cell. NavMeshFix jumps over it.
//
// Both sites are found by a byte signature that must match exactly once; otherwise nothing is changed.
// ---------------------------------------------------------------------------
// xorps xmm0,xmm0 ; movaps [rbp],xmm0 ; mov dword ptr [rbp],32.0f ; mov qword ptr [rbp+0x10],32.0f
const uint8_t kProbeSweepSig[] = {0x0F, 0x57, 0xC0, 0x0F, 0x29, 0x45, 0x00, 0xC7, 0x45, 0x00, 0x00, 0x00, 0x00, 0x42,
                                  0x48, 0xC7, 0x45, 0x10, 0x00, 0x00, 0x00, 0x42};
// mov rax,[rdi+18] ; mov rcx,[rdi+20] ; cmp rax,rcx ; je +31 ; mov rdx,[rbx] ; nop ; cmp [rax],rdx ; je +9 ; add rax,28 ; cmp rax,rcx ; jne -e
const uint8_t kNavMeshSig[] = {0x48, 0x8B, 0x47, 0x18, 0x48, 0x8B, 0x4F, 0x20, 0x48, 0x3B, 0xC1, 0x74, 0x31, 0x48, 0x8B, 0x13,
                               0x66, 0x90, 0x48, 0x39, 0x10, 0x74, 0x09, 0x48, 0x83, 0xC0, 0x28, 0x48, 0x3B, 0xC1, 0x75, 0xF2};
void DiagnoseProbeSweepSignature() {
    const int exactMatches = CountPattern(g_text, kProbeSweepSig, nullptr, sizeof(kProbeSweepSig), nullptr, 0);
    Log("probe sweep exact signature matches: %d", exactMatches);
    std::vector<uint8_t> relaxedMask(sizeof(kProbeSweepSig), 0xFF);
    // The two ModRM displacement bytes depend on the compiler's stack layout.
    relaxedMask[6] = 0;
    relaxedMask[9] = 0;
    LogPatternCandidates("probe sweep relaxed signature (stack offsets ignored)",
                         kProbeSweepSig, relaxedMask.data(), sizeof(kProbeSweepSig));

    const uint8_t radius32[] = {0xC7, 0x45, 0x00, 0x00, 0x00, 0x00, 0x42};
    const uint8_t radius32Mask[] = {0xFF, 0xFF, 0x00, 0xFF, 0xFF, 0xFF, 0xFF};
    LogPatternCandidates("probe sweep 32.0f immediate anchor",
                         radius32, radius32Mask, sizeof(radius32));

    const uint8_t secondRadius[] = {0x48, 0xC7, 0x45, 0x10, 0x00, 0x00, 0x00, 0x42};
    LogPatternCandidates("probe sweep second-radius anchor",
                         secondRadius, nullptr, sizeof(secondRadius));
}

void DiagnoseNavMeshSignature() {
    const int exactMatches = CountPattern(g_text, kNavMeshSig, nullptr, sizeof(kNavMeshSig), nullptr, 0);
    Log("navmesh exact signature matches: %d", exactMatches);
    std::vector<uint8_t> relaxedMask(sizeof(kNavMeshSig), 0xFF);
    // These are rel8 branch destinations; ignore them only while locating candidates.
    // The original EB 3C patch is NOT applied to a relaxed match.
    relaxedMask[12] = 0;
    relaxedMask[22] = 0;
    relaxedMask[31] = 0;
    LogPatternCandidates("navmesh relaxed signature (branch offsets ignored)",
                         kNavMeshSig, relaxedMask.data(), sizeof(kNavMeshSig));

    const uint8_t loopStart[] = {
        0x48, 0x8B, 0x47, 0x18, 0x48, 0x8B, 0x4F, 0x20, 0x48, 0x3B, 0xC1
    };
    LogPatternCandidates("navmesh loop-start anchor", loopStart, nullptr, sizeof(loopStart));

    const uint8_t loopTail[] = {0x48, 0x83, 0xC0, 0x28, 0x48, 0x3B, 0xC1};
    LogPatternCandidates("navmesh loop-tail anchor", loopTail, nullptr, sizeof(loopTail));
}

int g_probeSweepRadius = 8, g_navMeshFix = 1;

void PatchProbeSweep() {
    if (g_probeSweepRadius <= 0 || g_probeSweepRadius >= 32) return;
    uint8_t* at = FindUnique(g_text, kProbeSweepSig, sizeof kProbeSweepSig);
    if (!at) {
        Log("probe sweep: exact signature not found or not unique; patch left unchanged");
        DiagnoseProbeSweepSignature();
        return;
    }
    float r = static_cast<float>(g_probeSweepRadius);
    uint32_t bits;
    memcpy(&bits, &r, 4);
    uint8_t patch[12];
    memcpy(patch, at + 10, sizeof(patch));
    memcpy(patch, &bits, sizeof(bits));
    memcpy(patch + 8, &bits, sizeof(bits));
    if (WriteProtected(at + 10, patch, sizeof(patch))) {
        Log("reflection probe placement sweep: sphere radius 32 -> %d m", g_probeSweepRadius);
    } else {
        Log("reflection probe placement sweep: patch failed and was rolled back");
    }
}

void PatchNavMesh() {
    if (!g_navMeshFix) return;
    uint8_t* at = FindUnique(g_text, kNavMeshSig, sizeof kNavMeshSig);
    if (!at) {
        Log("navmesh fix: exact signature not found or not unique; patch left unchanged");
        DiagnoseNavMeshSignature();
        return;
    }
    const uint8_t jmp[] = {0xEB, 0x3C};   // where the je in the signature lands: past the walk and the assert
    if (WriteProtected(at, jmp, sizeof jmp)) Log("navmesh static-box registration: duplicate search skipped");
}

LARGE_INTEGER g_freq;
LONGLONG Now() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
double Ms(LONGLONG ticks) { return 1000.0 * static_cast<double>(ticks) / static_cast<double>(g_freq.QuadPart); }

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------
int g_enabled = 1, g_warmStart = 1, g_verify = 0, g_minRows = 0, g_logInterval = 5, g_dumpFailures = 0;
int g_diagnoseSignatures = 1;
double g_verifyTolerance = 0.001;
LONG g_runtimeDisabled = 0;

// ---------------------------------------------------------------------------
// Warm-start cache: active set of the last solve of the same problem. A bad guess costs time, never correctness.
// ---------------------------------------------------------------------------
struct CacheEntry {
    uint64_t key = 0;
    int n = 0;
    LONGLONG lastUse = 0;
    std::vector<unsigned char> interior;
};
CacheEntry g_cache[32];
SRWLOCK g_cacheLock = SRWLOCK_INIT;

uint64_t HashBytes(uint64_t h, const void* p, size_t size) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < size; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}

// The bound values drift every tick (motor/piston limits get recomputed), so hash the structure only:
// row count, friction dependencies, kind of bounds per row.
uint64_t HashProblem(int n, const float* lo, const float* hi, const int* dep) {
    uint64_t h = 14695981039346656037ull;
    h = HashBytes(h, &n, sizeof n);
    h = HashBytes(h, dep, sizeof(int) * n);
    for (int i = 0; i < n; ++i) {
        unsigned char cls;
        if (dep[i] >= 0) cls = 4;
        else if (lo[i] <= -FLT_MAX && hi[i] >= FLT_MAX) cls = 0;
        else if (lo[i] == 0.0f) cls = 1;
        else if (hi[i] == 0.0f) cls = 3;
        else if (lo[i] < 0.0f && hi[i] > 0.0f) cls = 2;
        else cls = 5;
        h ^= cls; h *= 1099511628211ull;
    }
    return h ? h : 1;
}

bool ValidateLcpSolution(int n, const float* A, const float* b, const float* lo,
                         const float* hi, const int* dep, const float* x) {
    if (n <= 0 || !A || !b || !lo || !hi || !dep || !x) return false;
    double maxUnboundedX = 0.0;
    for (int i = 0; i < n; ++i) {
        if (!std::isfinite(x[i]) || !std::isfinite(b[i]) ||
            !std::isfinite(lo[i]) || !std::isfinite(hi[i])) return false;
        if (dep[i] >= n) return false;
        if (dep[i] < 0 && fabs(static_cast<double>(x[i])) > maxUnboundedX)
            maxUnboundedX = fabs(static_cast<double>(x[i]));
    }

    for (int i = 0; i < n; ++i) {
        if (dep[i] >= 0) {
            const double scaledBound = fabs(static_cast<double>(hi[i])) * maxUnboundedX + 1.0e-3;
            if (fabs(static_cast<double>(x[i])) > scaledBound) return false;
            continue;
        }

        double w = -static_cast<double>(b[i]);
        for (int j = 0; j < n; ++j) {
            const double a = A[static_cast<size_t>(i) * n + j];
            if (!std::isfinite(a)) return false;
            w += a * static_cast<double>(x[j]);
        }
        const double wTolerance = 1.0e-3 * (1.0 + fabs(static_cast<double>(b[i])));
        const double lowerTolerance = 1.0e-5 * (1.0 + fabs(static_cast<double>(lo[i])));
        const double upperTolerance = 1.0e-5 * (1.0 + fabs(static_cast<double>(hi[i])));
        const double value = x[i];
        if (value < static_cast<double>(lo[i]) - lowerTolerance ||
            value > static_cast<double>(hi[i]) + upperTolerance) return false;

        const bool atLower = value <= static_cast<double>(lo[i]) + lowerTolerance;
        const bool atUpper = value >= static_cast<double>(hi[i]) - upperTolerance;
        if (atLower && atUpper) continue;
        if (atLower) {
            if (w < -wTolerance) return false;
        } else if (atUpper) {
            if (w > wTolerance) return false;
        } else if (fabs(w) > wTolerance) {
            return false;
        }
    }
    return true;
}

bool CacheLookup(uint64_t key, int n, std::vector<unsigned char>& out) {
    AcquireSRWLockShared(&g_cacheLock);
    bool found = false;
    for (CacheEntry& e : g_cache) {
        if (e.key == key && e.n == n) { out = e.interior; found = true; break; }
    }
    ReleaseSRWLockShared(&g_cacheLock);
    return found;
}

void CacheStore(uint64_t key, int n, const std::vector<unsigned char>& interior) {
    LONGLONG now = Now();
    AcquireSRWLockExclusive(&g_cacheLock);
    CacheEntry* target = nullptr;
    for (CacheEntry& e : g_cache) if (e.key == key && e.n == n) { target = &e; break; }
    if (!target) {
        target = &g_cache[0];
        for (CacheEntry& e : g_cache) if (e.lastUse < target->lastUse) target = &e;
    }
    target->key = key; target->n = n; target->lastUse = now; target->interior = interior;
    ReleaseSRWLockExclusive(&g_cacheLock);
}

// ---------------------------------------------------------------------------
// Stats
// ---------------------------------------------------------------------------
CRITICAL_SECTION g_statsCs;
struct Stats {
    int calls = 0, fast = 0, fallback = 0, invalid = 0, skipped = 0, cacheHits = 0, maxRows = 0, retries = 0, evictions = 0;
    LONGLONG tFast = 0, tFastMax = 0, tOrig = 0;
    int origCalls = 0;
    void Reset() { *this = Stats(); }
} g_stats;
LONGLONG g_lastReport = 0;

void Report() {
    Stats s;
    EnterCriticalSection(&g_statsCs);
    s = g_stats; g_stats.Reset();
    LeaveCriticalSection(&g_statsCs);
    if (s.calls == 0) return;
    Log("solves %d: fast %d (avg %.3f ms, max %.3f ms, max rows %d, warm-start hits %d, cold retries %d), fallback to game solver %d, skipped %d, invalid layout %d%s",
        s.calls, s.fast, s.fast ? Ms(s.tFast) / s.fast : 0.0, Ms(s.tFastMax), s.maxRows, s.cacheHits, s.retries, s.fallback, s.skipped, s.invalid,
        s.origCalls ? " | verify" : "");
    if (s.origCalls) Log("  game solver for comparison: %d calls, avg %.3f ms", s.origCalls, Ms(s.tOrig) / s.origCalls);
}

// ---------------------------------------------------------------------------
// The hook. btDantzigSolver::solveMLCP(const btMatrixXu& A, const btVectorXu& b, btVectorXu& x, const btVectorXu& lo,
// const btVectorXu& hi, const btAlignedObjectArray<int>& limitDependency, int numIterations, bool useSparsity)
// btMatrixX: rows +0, cols +4, storage size +0x1c, storage data +0x28. btVectorX / btAlignedObjectArray: size +4, data +0x10.
// ---------------------------------------------------------------------------
using DantzigFn = bool (*)(void*, const uint8_t*, const uint8_t*, uint8_t*, const uint8_t*, const uint8_t*, const uint8_t*, int, uintptr_t);
DantzigFn g_orig = nullptr;
LONG g_verifyLeft = 0;

HANDLE g_failDump = INVALID_HANDLE_VALUE; LONG g_failDumpLeft = 10;
void DumpFailure(int n, const float* A, const float* b, const float* lo, const float* hi, const int* dep) {
    if (g_failDump == INVALID_HANDLE_VALUE || InterlockedDecrement(&g_failDumpLeft) < 0) return;
    EnterCriticalSection(&g_statsCs);
    int hdr[4] = {0x50434c4d, n, 0, 0};
    DWORD w;
    WriteFile(g_failDump, hdr, sizeof hdr, &w, nullptr);
    WriteFile(g_failDump, A, static_cast<DWORD>(sizeof(float) * n * n), &w, nullptr);
    WriteFile(g_failDump, b, sizeof(float) * n, &w, nullptr);
    WriteFile(g_failDump, lo, sizeof(float) * n, &w, nullptr);
    WriteFile(g_failDump, hi, sizeof(float) * n, &w, nullptr);
    WriteFile(g_failDump, dep, sizeof(int) * n, &w, nullptr);
    std::vector<float> zeros(n, 0.0f);
    WriteFile(g_failDump, zeros.data(), sizeof(float) * n, &w, nullptr);
    FlushFileBuffers(g_failDump);
    LeaveCriticalSection(&g_statsCs);
}

thread_local FastLcpScratch t_scratch;
thread_local std::vector<unsigned char> t_guess, t_interior;
thread_local std::vector<float> t_ours;

bool SolveHook(void* self, const uint8_t* A, const uint8_t* b, uint8_t* x, const uint8_t* lo, const uint8_t* hi, const uint8_t* dep, int numIterations, uintptr_t sparsity) {
    if (!g_enabled || InterlockedCompareExchange(&g_runtimeDisabled, 0, 0) != 0)
        return g_orig(self, A, b, x, lo, hi, dep, numIterations, sparsity);

    // The executable SHA-256 pin is checked before the vtable is hooked. These guards
    // additionally reject unexpected object pointers/layouts before reading solver buffers.
    if (!A || !b || !x || !lo || !hi || !dep ||
        !IsReadableRange(A, 0x30) || !IsReadableRange(b, 0x18) ||
        !IsReadableRange(x, 0x18) || !IsReadableRange(lo, 0x18) ||
        !IsReadableRange(hi, 0x18) || !IsReadableRange(dep, 0x18)) {
        EnterCriticalSection(&g_statsCs); ++g_stats.calls; ++g_stats.invalid; LeaveCriticalSection(&g_statsCs);
        return g_orig(self, A, b, x, lo, hi, dep, numIterations, sparsity);
    }

    const int rows = Read<int>(A), cols = Read<int>(A + 4), storage = Read<int>(A + 0x1c);
    if (rows <= 0 || rows > 4096 || rows != cols || storage != rows * rows) {
        EnterCriticalSection(&g_statsCs); ++g_stats.calls; ++g_stats.invalid; LeaveCriticalSection(&g_statsCs);
        return g_orig(self, A, b, x, lo, hi, dep, numIterations, sparsity);
    }

    const float* Ad = Read<const float*>(A + 0x28);
    const float* bD = Read<const float*>(b + 0x10);
    float* xD = Read<float*>(x + 0x10);
    const float* loD = Read<const float*>(lo + 0x10);
    const float* hiD = Read<const float*>(hi + 0x10);
    const int* depD = Read<const int*>(dep + 0x10);
    const size_t matrixBytes = sizeof(float) * static_cast<size_t>(rows) * static_cast<size_t>(rows);
    const size_t vectorBytes = sizeof(float) * static_cast<size_t>(rows);
    const size_t dependencyBytes = sizeof(int) * static_cast<size_t>(rows);
    const bool valid = Ad && bD && xD && loD && hiD && depD &&
                       IsReadableRange(Ad, matrixBytes) && IsReadableRange(bD, vectorBytes) &&
                       IsWritableRange(xD, vectorBytes) && IsReadableRange(loD, vectorBytes) &&
                       IsReadableRange(hiD, vectorBytes) && IsReadableRange(depD, dependencyBytes) &&
                       Read<int>(b + 4) == rows && Read<int>(x + 4) == rows &&
                       Read<int>(lo + 4) == rows && Read<int>(hi + 4) == rows &&
                       Read<int>(dep + 4) == rows;
    if (!valid) {
        EnterCriticalSection(&g_statsCs); ++g_stats.calls; ++g_stats.invalid; LeaveCriticalSection(&g_statsCs);
        return g_orig(self, A, b, x, lo, hi, dep, numIterations, sparsity);
    }
    if (rows < g_minRows) {
        EnterCriticalSection(&g_statsCs); ++g_stats.calls; ++g_stats.skipped; LeaveCriticalSection(&g_statsCs);
        return g_orig(self, A, b, x, lo, hi, dep, numIterations, sparsity);
    }

    const unsigned char* guess = nullptr;
    uint64_t key = 0;
    bool hit = false;
    if (g_warmStart) {
        key = HashProblem(rows, loD, hiD, depD);
        if (CacheLookup(key, rows, t_guess) && static_cast<int>(t_guess.size()) == rows) { guess = t_guess.data(); hit = true; }
    }

    // Verification test: keep the vanilla game's complete answer before attempting FastPhysics.
    const bool verifyNow = g_verifyLeft > 0 && InterlockedDecrement(&g_verifyLeft) >= 0;
    LONGLONG tOrig = 0;
    bool okOrig = false;
    if (verifyNow) {
        const LONGLONG t1 = Now();
        okOrig = g_orig(self, A, b, x, lo, hi, dep, numIterations, sparsity);
        tOrig = Now() - t1;
        t_ours.assign(xD, xD + rows);
    }

    t_interior.resize(rows);
    const LONGLONG t0 = Now();
    bool ok = FastSolveLcp(rows, Ad, bD, loD, hiD, depD, xD, t_scratch, guess, t_interior.data(), 0);
    bool retried = false;
    if (!ok && guess) { retried = true; ok = FastSolveLcp(rows, Ad, bD, loD, hiD, depD, xD, t_scratch, nullptr, t_interior.data(), 0); }
    if (!ok) { retried = true; ok = FastSolveLcp(rows, Ad, bD, loD, hiD, depD, xD, t_scratch, nullptr, t_interior.data(), 2); }
    if (!ok) { retried = true; ok = FastSolveLcp(rows, Ad, bD, loD, hiD, depD, xD, t_scratch, nullptr, t_interior.data(), 8); }
    bool validationFailed = false;
    if (ok && !ValidateLcpSolution(rows, Ad, bD, loD, hiD, depD, xD)) {
        ok = false;
        validationFailed = true;
    }
    const LONGLONG dt = Now() - t0;

    // Another worker may have detected a mismatch while this call was solving.
    // Do not let an in-flight FastPhysics result escape after the session circuit-breaker trips.
    if (InterlockedCompareExchange(&g_runtimeDisabled, 0, 0) != 0) {
        if (verifyNow && t_ours.size() == static_cast<size_t>(rows)) {
            memcpy(xD, t_ours.data(), vectorBytes);
            return okOrig;
        }
        return g_orig(self, A, b, x, lo, hi, dep, numIterations, sparsity);
    }

    if (verifyNow) {
        double maxDiff = 0.0, maxX = 0.0;
        bool gameFinite = true;
        for (int i = 0; i < rows; ++i) {
            if (!std::isfinite(t_ours[i])) gameFinite = false;
            const double d = fabs(static_cast<double>(t_ours[i]) - static_cast<double>(xD[i]));
            if (!std::isfinite(d)) maxDiff = INFINITY;
            else if (d > maxDiff) maxDiff = d;
            const double a = fabs(static_cast<double>(t_ours[i]));
            if (std::isfinite(a) && a > maxX) maxX = a;
        }
        const double tolerance = 1.0e-5 + g_verifyTolerance * (1.0 + maxX);
        const bool agree = okOrig && gameFinite && ok && maxDiff <= tolerance;
        Log("verify: %d rows | fast %.3f ms (ok %d%s), game %.3f ms (ok %d) | maxDiff %.6g, tolerance %.6g, warm start %s",
            rows, Ms(dt), ok ? 1 : 0, retried ? ", cold retry" : "", Ms(tOrig), okOrig ? 1 : 0,
            maxDiff, tolerance, hit ? "hit" : "miss");

        if (!agree) {
            // The vanilla result wins. Disable the custom solver for the rest of this process.
            // No unverified FastPhysics answer is handed to the simulation after a mismatch.
            if (t_ours.size() == static_cast<size_t>(rows))
                memcpy(xD, t_ours.data(), vectorBytes);
            InterlockedExchange(&g_runtimeDisabled, 1);
            Log("%s: restored vanilla solver output; FastPhysics disabled for this process",
                validationFailed ? "LCP validation failed" : "VERIFY MISMATCH");
            EnterCriticalSection(&g_statsCs);
            ++g_stats.calls; ++g_stats.fallback;
            if (retried) ++g_stats.retries;
            LeaveCriticalSection(&g_statsCs);
            return okOrig;
        }
        if (g_warmStart) CacheStore(key, rows, t_interior);
    } else if (!ok) {
        DumpFailure(rows, Ad, bD, loD, hiD, depD);
        if (validationFailed) {
            InterlockedExchange(&g_runtimeDisabled, 1);
            Log("LCP validation failed: FastPhysics disabled for the rest of this process");
        }
        EnterCriticalSection(&g_statsCs); ++g_stats.calls; ++g_stats.fallback; if (retried) ++g_stats.retries; LeaveCriticalSection(&g_statsCs);
        return g_orig(self, A, b, x, lo, hi, dep, numIterations, sparsity);
    } else if (g_warmStart) {
        CacheStore(key, rows, t_interior);
    }

    bool report = false;
    EnterCriticalSection(&g_statsCs);
    ++g_stats.calls; ++g_stats.fast; g_stats.tFast += dt; if (dt > g_stats.tFastMax) g_stats.tFastMax = dt;
    if (rows > g_stats.maxRows) g_stats.maxRows = rows;
    if (hit) ++g_stats.cacheHits;
    if (retried) ++g_stats.retries;
    if (verifyNow) { ++g_stats.origCalls; g_stats.tOrig += tOrig; }
    const LONGLONG now = Now();
    if (Ms(now - g_lastReport) >= 1000.0 * g_logInterval) { g_lastReport = now; report = true; }
    LeaveCriticalSection(&g_statsCs);
    if (report) Report();
    return true;
}

bool CpuSupportsAvx2Fma() {
    int info[4];
    __cpuid(info, 0);
    if (info[0] < 7) return false;
    __cpuid(info, 1);
    const bool fma = (info[2] & (1 << 12)) != 0, osxsave = (info[2] & (1 << 27)) != 0, avx = (info[2] & (1 << 28)) != 0;
    if (!fma || !osxsave || !avx) return false;
    const unsigned long long xcr0 = _xgetbv(0);
    if ((xcr0 & 6) != 6) return false;
    __cpuidex(info, 7, 0);
    return (info[1] & (1 << 5)) != 0;
}

void Init(HMODULE self) {
    if (!IsScrapMechanicExe()) return;
    wchar_t path[MAX_PATH];
    if (PathNextToModule(self, L"FastPhysics.log", path)) {
        g_log = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    InitializeCriticalSection(&g_statsCs);
    QueryPerformanceFrequency(&g_freq);
    g_lastReport = Now();
    wchar_t ini[MAX_PATH];
    wchar_t expectedHashW[80] = {};
    if (PathNextToModule(self, L"FastPhysics.ini", ini)) {
        g_enabled = static_cast<int>(GetPrivateProfileIntW(L"FastPhysics", L"Enabled", 1, ini));
        g_warmStart = static_cast<int>(GetPrivateProfileIntW(L"FastPhysics", L"WarmStart", 1, ini));
        g_verify = static_cast<int>(GetPrivateProfileIntW(L"FastPhysics", L"Verify", 512, ini));
        if (g_verify < 512) g_verify = 512;
        g_minRows = static_cast<int>(GetPrivateProfileIntW(L"FastPhysics", L"MinRows", 0, ini));
        g_logInterval = static_cast<int>(GetPrivateProfileIntW(L"FastPhysics", L"LogIntervalSeconds", 5, ini));
        if (g_logInterval < 1) g_logInterval = 1;
        g_dumpFailures = static_cast<int>(GetPrivateProfileIntW(L"FastPhysics", L"DumpFailures", 0, ini));
        // This diagnostic build never applies the two optional executable-code patches,
        // even when an older user FastPhysics.ini still contains ProbeSweepRadius=8/NavMeshFix=1.
        g_probeSweepRadius = 0;
        g_navMeshFix = 0;
        g_diagnoseSignatures = 1;
        int tolerancePpm = GetPrivateProfileIntW(L"FastPhysics", L"VerifyTolerancePPM", 1000, ini);
        if (tolerancePpm < 0 || tolerancePpm > 5000) tolerancePpm = 1000;
        g_verifyTolerance = tolerancePpm / 1000000.0;
        GetPrivateProfileStringW(L"FastPhysics", L"ExpectedGameExeSHA256", L"", expectedHashW,
                                 static_cast<DWORD>(sizeof(expectedHashW) / sizeof(expectedHashW[0])), ini);
    }
    if (g_dumpFailures && PathNextToModule(self, L"FastPhysics_fail.bin", path)) {
        g_failDump = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    g_verifyLeft = g_verify;
    Log("FastPhysics %s loaded (enabled %d, warm start %d, verify %d, min rows %d, probe sweep radius %d, navmesh fix %d)",
        FP_VERSION_STRING, g_enabled, g_warmStart, g_verify, g_minRows, g_probeSweepRadius, g_navMeshFix);
    if (!g_enabled) return;
    char actualHash[65] = {};
    wchar_t exePath[MAX_PATH] = {};
    const DWORD exePathLength = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (exePathLength == 0 || exePathLength >= MAX_PATH || !HashFileSha256(exePath, actualHash)) {
        Log("could not calculate ScrapMechanic.exe SHA-256; solver hook remains disabled");
        return;
    }
    Log("ScrapMechanic.exe SHA-256: %s", actualHash);

    char expectedHash[80] = {};
    if (WideCharToMultiByte(CP_ACP, 0, expectedHashW, -1, expectedHash, sizeof(expectedHash), nullptr, nullptr) <= 0 ||
        !IsSha256Hex(expectedHash)) {
        Log("ExpectedGameExeSHA256 is empty or invalid; diagnostic mode only, solver hook disabled");
        if (g_diagnoseSignatures && InitSections(reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr)))) {
            DiagnoseProbeSweepSignature();
            DiagnoseNavMeshSignature();
        }
        return;
    }
    if (_stricmp(actualHash, expectedHash) != 0) {
        Log("game executable hash does not match ExpectedGameExeSHA256; diagnostic mode only, solver hook disabled");
        if (g_diagnoseSignatures && InitSections(reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr)))) {
            DiagnoseProbeSweepSignature();
            DiagnoseNavMeshSignature();
        }
        return;
    }
    Log("game executable SHA-256 pin matched; compatibility gate passed");

    if (!InitSections(reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr)))) { Log("sections not found"); return; }
    if (g_diagnoseSignatures) {
        DiagnoseProbeSweepSignature();
        DiagnoseNavMeshSignature();
    }
    PatchProbeSweep();
    PatchNavMesh();
    if (!CpuSupportsAvx2Fma()) { Log("CPU without AVX2/FMA: the solver stays the game's own"); g_enabled = 0; return; }
    void** vt = FindVTable(".?AVbtDantzigSolver@@");
    if (!vt || !g_rdata.Contains(vt) || !g_text.Contains(vt[1])) { Log("btDantzigSolver vtable not found (%p), FastPhysics inactive", vt); return; }
    // Publish the original function pointer before the vtable can expose SolveHook.
    // If a game worker calls the hook immediately after the atomic exchange, g_orig is ready.
    void* original = vt[1];
    g_orig = reinterpret_cast<DantzigFn>(original);
    void* ignoredOriginal = nullptr;
    if (!PatchPointer(&vt[1], reinterpret_cast<void*>(&SolveHook), &ignoredOriginal)) {
        Log("vtable patch failed; the original solver pointer remains available");
        return;
    }
    Log("btDantzigSolver::solveMLCP hooked (original %p)", original);
}

DWORD WINAPI InitializeWorker(LPVOID context) {
    Init(static_cast<HMODULE>(context));
    return 0;
}

}  // namespace

#ifndef FP_SELFTEST
BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        // Hand work to a worker and never wait for it from DllMain.
        // File I/O, PE inspection and code patching run after the loader lock is released.
        // Keep thread notifications enabled: the DLL uses the static CRT (/MT) and TLS.
        HANDLE worker = CreateThread(nullptr, 0, InitializeWorker, module, 0, nullptr);
        if (worker) CloseHandle(worker);
        else OutputDebugStringW(L"FastPhysics: failed to create initialization worker");
    }
    return TRUE;
}
#endif
