// PhotosSlideshowHook.dll - injected into Photos.exe by PhotosSlideshow.exe
//
// Settings live in settings.ini next to PhotosSlideshowHook.dll
// (i.e. next to PhotosSlideshow.exe; falls back to %USERPROFILE%\PhotosSlideshow):
//   [Slideshow]
//   DurationMs=15000        (any positive value)
//   Loop=1|0                (optional; absent = keep Photos default)
//   Animations=1|0         (optional)
//   Transitions=1|0        (optional)
//
// Mechanism (tested against Microsoft.Windows.Photos_2026.11090.22001.0):
//  1) Patch the IAT slots that Slideshow.dll's delay-load descriptor uses for
//     chakra.dll imports (ResolveDelayLoadedAPI bypasses GetProcAddress, so we
//     locate the slots in the delay-import table and overwrite them directly):
//       - JsCreateNamedFunction -> inspect/substitute every Native* callback
//       - JsRunScript           -> rewrite imageItemDurationMs literals in the
//                                  bundled slideshow JS before it executes
//  2) Substitute callbacks for:
//       NativeGetImageItemDurationMs      -> return configured duration
//       NativeGetInitialSettings          -> orig result + INI overrides
//       NativeNotify{Loop,AreAnimationsActive,AreTransitionsActive}Changed
//                                          -> persist new value to INI
//         (stored under the shorter keys Loop/Animations/Transitions)
//  3) Patch the native duration constant in Slideshow.dll as a fallback.

#include <windows.h>
#include <stdio.h>
#include <string>
#include <wchar.h>
#include <wctype.h>

// RVA of the double constant returned by NativeGetImageItemDurationMs
// (VA 0x180402650 in Slideshow.dll with preferred image base 0x180000000)
static const DWORD_PTR kDurationRva = 0x402650;

static HMODULE g_hModule = NULL;

// Append suffix to out if it fits; returns false on overflow instead of
// letting wcscat_s invoke the invalid-parameter handler.
static bool PathCat(wchar_t* out, DWORD outSize, const wchar_t* suffix)
{
    size_t len = wcslen(out);
    if (len + wcslen(suffix) + 1 > outSize) return false;
    wcscat_s(out, outSize, suffix);
    return true;
}

// Directory containing PhotosSlideshowHook.dll (i.e. next to PhotosSlideshow.exe).
// Falls back to %USERPROFILE%\PhotosSlideshow if the module path can't be resolved.
static bool GetBaseDir(wchar_t* out, DWORD outSize)
{
    if (g_hModule)
    {
        wchar_t path[MAX_PATH];
        if (GetModuleFileNameW(g_hModule, path, MAX_PATH) > 0)
        {
            wchar_t* slash = wcsrchr(path, L'\\');
            if (slash) *slash = L'\0';
            return wcscpy_s(out, outSize, path) == 0;
        }
    }
    DWORD n = GetEnvironmentVariableW(L"USERPROFILE", out, outSize);
    if (n == 0 || n >= outSize)
        n = GetEnvironmentVariableW(L"LOCALAPPDATA", out, outSize);
    if (n == 0 || n >= outSize) return false;
    return PathCat(out, outSize, L"\\PhotosSlideshow");
}

// ---------------- logging ----------------

static void Log(const wchar_t* fmt, ...)
{
    wchar_t path[MAX_PATH];
    if (!GetBaseDir(path, MAX_PATH)) return;
    if (!PathCat(path, MAX_PATH, L"\\hook.log")) return;
    // rotate at ~1 MB so the log can't grow without bound
    WIN32_FILE_ATTRIBUTE_DATA fad;
    bool truncate = GetFileAttributesExW(path, GetFileExInfoStandard, &fad) &&
                    fad.nFileSizeHigh == 0 && fad.nFileSizeLow > 1024 * 1024;
    FILE* f = NULL;
    if (_wfopen_s(&f, path, truncate ? L"w" : L"a") == 0 && f)
    {
        va_list ap; va_start(ap, fmt);
        vfwprintf(f, fmt, ap);
        va_end(ap);
        fputc(L'\n', f);
        fclose(f);
    }
}

// ---------------- ini ----------------

static bool GetIniPath(wchar_t* out, DWORD outSize)
{
    if (!GetBaseDir(out, outSize)) return false;
    return PathCat(out, outSize, L"\\settings.ini");
}

static int ReadDurationMs()
{
    // Per-launch override written by PhotosSlideshow.exe when --duration is
    // combined with a folder/files argument. The launcher deletes the file
    // after injection, so it never persists.
    wchar_t dir[MAX_PATH];
    if (GetBaseDir(dir, MAX_PATH))
    {
        wchar_t path[MAX_PATH];
        wcscpy_s(path, dir);
        if (PathCat(path, MAX_PATH, L"\\session_duration.txt"))
        {
            FILE* f = NULL;
            if (_wfopen_s(&f, path, L"r") == 0 && f)
            {
                long long v = 0;
                bool ok = fwscanf(f, L"%lld", &v) == 1 && v > 0;
                fclose(f);
                if (ok) return (int)(v > 86400000 ? 86400000 : v);
            }
        }
    }

    wchar_t iniPath[MAX_PATH];
    if (!GetIniPath(iniPath, MAX_PATH)) return 5000;
    int v = GetPrivateProfileIntW(L"Slideshow", L"DurationMs", 5000, iniPath);
    // clamp to a sane range: non-positive or absurd values -> default
    return (v > 0 && v <= 86400000) ? v : 5000;
}

// returns -1 if key absent, else 0/1
static int ReadIniBool(const wchar_t* key)
{
    wchar_t iniPath[MAX_PATH];
    wchar_t buf[8];
    if (!GetIniPath(iniPath, MAX_PATH)) return -1;
    DWORD n = GetPrivateProfileStringW(L"Slideshow", key, L"", buf, 8, iniPath);
    if (n == 0) return -1;
    return (buf[0] == L'1' || buf[0] == L'y' || buf[0] == L't' || buf[0] == L'T') ? 1 : 0;
}

static void WriteIniBool(const wchar_t* key, bool v)
{
    wchar_t iniPath[MAX_PATH];
    if (!GetIniPath(iniPath, MAX_PATH)) return;
    WritePrivateProfileStringW(L"Slideshow", key, v ? L"1" : L"0", iniPath);
}

// ---------------- patch 1: Slideshow.dll native constant ----------------

static void PatchSlideshowConstant(HMODULE hSlideshow, int durationMs)
{
    BYTE* base = (BYTE*)hSlideshow;
    double* target = (double*)(base + kDurationRva);

    // This RVA is only valid for the tested Photos build. Before writing,
    // sanity-check that the slot actually holds something duration-like -
    // on any other build this address could be arbitrary data/code and
    // overwriting it would corrupt the process.
    double before = *target;
    if (!(before >= 100.0 && before <= 600000.0))
    {
        Log(L"[hook] duration const @%p holds %.1f - not a plausible duration, skipping patch", target, before);
        return;
    }
    double value = (double)durationMs;

    DWORD oldProtect = 0;
    if (!VirtualProtect(target, sizeof(double), PAGE_READWRITE, &oldProtect))
    {
        Log(L"[hook] VirtualProtect failed err=%lu", GetLastError());
        return;
    }
    *target = value;
    VirtualProtect(target, sizeof(double), oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), target, sizeof(double));
    Log(L"[hook] duration const @%p before=%.1f after=%.1f", target, before, *target);
}

// ---------------- JSRT types ----------------

typedef void* JsValueRef;
typedef void* JsPropertyIdRef;
typedef int JsErrorCode;

#define JS_NO_ERROR 0

typedef JsValueRef (CALLBACK *JsNativeFunction)(JsValueRef callee, bool isConstructCall,
                                                JsValueRef* arguments, unsigned short argumentCount,
                                                void* callbackState);

// resolved lazily once chakra.dll is loaded
typedef JsErrorCode (WINAPI *JsConvertValueToString_t)(JsValueRef, JsValueRef*);
typedef JsErrorCode (WINAPI *JsStringToPointer_t)(JsValueRef, const wchar_t**, size_t*);
typedef JsErrorCode (WINAPI *JsGetPropertyIdFromName_t)(const wchar_t*, JsPropertyIdRef*);
typedef JsErrorCode (WINAPI *JsSetProperty_t)(JsValueRef, JsPropertyIdRef, JsValueRef, bool);
typedef JsErrorCode (WINAPI *JsBoolToBoolean_t)(bool, JsValueRef*);
typedef JsErrorCode (WINAPI *JsBooleanToBool_t)(JsValueRef, bool*);
typedef JsErrorCode (WINAPI *JsDoubleToNumber_t)(double, JsValueRef*);

static JsConvertValueToString_t pJsConvertValueToString = NULL;
static JsStringToPointer_t pJsStringToPointer = NULL;
static JsGetPropertyIdFromName_t pJsGetPropertyIdFromName = NULL;
static JsSetProperty_t pJsSetProperty = NULL;
static JsBoolToBoolean_t pJsBoolToBoolean = NULL;
static JsBooleanToBool_t pJsBooleanToBool = NULL;
static JsDoubleToNumber_t pJsDoubleToNumber = NULL;

static bool ResolveJsRt()
{
    if (pJsConvertValueToString) return true;
    HMODULE h = GetModuleHandleW(L"chakra.dll");
    if (!h) return false;
    pJsConvertValueToString = (JsConvertValueToString_t)GetProcAddress(h, "JsConvertValueToString");
    pJsStringToPointer = (JsStringToPointer_t)GetProcAddress(h, "JsStringToPointer");
    pJsGetPropertyIdFromName = (JsGetPropertyIdFromName_t)GetProcAddress(h, "JsGetPropertyIdFromName");
    pJsSetProperty = (JsSetProperty_t)GetProcAddress(h, "JsSetProperty");
    pJsBoolToBoolean = (JsBoolToBoolean_t)GetProcAddress(h, "JsBoolToBoolean");
    pJsBooleanToBool = (JsBooleanToBool_t)GetProcAddress(h, "JsBooleanToBool");
    pJsDoubleToNumber = (JsDoubleToNumber_t)GetProcAddress(h, "JsDoubleToNumber");
    return pJsConvertValueToString && pJsStringToPointer &&
           pJsGetPropertyIdFromName && pJsSetProperty &&
           pJsBoolToBoolean && pJsBooleanToBool && pJsDoubleToNumber;
}

static int g_durationMs = 5000;

// convert a JsValueRef string to std::wstring
static bool JsValueToWString(JsValueRef v, std::wstring& out)
{
    if (!ResolveJsRt()) return false;
    JsValueRef s = NULL;
    if (pJsConvertValueToString(v, &s) != JS_NO_ERROR || !s) return false;
    const wchar_t* ptr = NULL;
    size_t len = 0;
    if (pJsStringToPointer(s, &ptr, &len) != JS_NO_ERROR || !ptr) return false;
    out.assign(ptr, len);
    return true;
}

// ---------------- substituted native callbacks ----------------

typedef JsErrorCode (WINAPI *JsCreateNamedFunction_t)(JsValueRef name, JsNativeFunction nativeFunction,
                                                    void* callbackState, JsValueRef* function);

static JsCreateNamedFunction_t g_realCreateNamed = NULL;

static JsNativeFunction g_origGetInitSettings = NULL;
static JsNativeFunction g_origNotifyLoop = NULL;
static JsNativeFunction g_origNotifyAnim = NULL;
static JsNativeFunction g_origNotifyTrans = NULL;

static int g_durCalls = 0;
static JsValueRef CALLBACK OurGetDuration(JsValueRef, bool, JsValueRef*, unsigned short, void*)
{
    JsValueRef v = NULL;
    JsErrorCode rc = pJsDoubleToNumber ? pJsDoubleToNumber((double)g_durationMs, &v) : -1;
    if (++g_durCalls <= 5 || (g_durCalls % 50) == 0)
        Log(L"[hook] OurGetDuration call #%d -> %d ms rc=%d v=%p", g_durCalls, g_durationMs, rc, v);
    return rc == JS_NO_ERROR ? v : NULL;
}

static JsValueRef CALLBACK OurGetInitSettings(JsValueRef callee, bool isCtor,
                                              JsValueRef* args, unsigned short argc, void* state)
{
    if (!g_origGetInitSettings) { Log(L"[hook] OurGetInitSettings: no orig!"); return NULL; }
    JsValueRef obj = g_origGetInitSettings(callee, isCtor, args, argc, state);
    static int initCalls = 0;
    if (++initCalls <= 5)
        Log(L"[hook] OurGetInitSettings call #%d obj=%p", initCalls, obj);
    if (!obj || !ResolveJsRt()) return obj;

    struct { const wchar_t* iniKey; const wchar_t* jsName; } map[] = {
        { L"Loop",        L"loop" },
        { L"Animations",  L"areAnimationsActive" },
        { L"Transitions", L"areTransitionsActive" },
    };
    for (auto& m : map)
    {
        int v = ReadIniBool(m.iniKey);
        if (v < 0) continue;
        JsPropertyIdRef pid = NULL;
        JsValueRef bv = NULL;
        if (pJsGetPropertyIdFromName(m.jsName, &pid) == JS_NO_ERROR && pid &&
            pJsBoolToBoolean(v == 1, &bv) == JS_NO_ERROR && bv)
            pJsSetProperty(obj, pid, bv, false);
    }
    return obj;
}

static JsValueRef CALLBACK OurNotifyBool(JsValueRef callee, bool isCtor,
                                         JsValueRef* args, unsigned short argc, void* state,
                                         JsNativeFunction orig, const wchar_t* iniKey)
{
    JsValueRef r = orig ? orig(callee, isCtor, args, argc, state) : NULL;
    if (args && pJsBooleanToBool)
    {
        // the bridge sometimes passes extra args; find the boolean one
        for (unsigned short i = 0; i < argc && i < 8; i++)
        {
            bool b = false;
            if (pJsBooleanToBool(args[i], &b) == JS_NO_ERROR)
            {
                Log(L"[hook] notify %s arg[%u]=%d", iniKey, i, (int)b);
                WriteIniBool(iniKey, b);
                break;
            }
        }
    }
    else
        Log(L"[hook] notify %s argc=%u (no args/boolfn)", iniKey, argc);
    return r;
}

static JsValueRef CALLBACK OurNotifyLoop(JsValueRef c, bool i, JsValueRef* a, unsigned short n, void* s)
{ return OurNotifyBool(c, i, a, n, s, g_origNotifyLoop, L"Loop"); }
static JsValueRef CALLBACK OurNotifyAnim(JsValueRef c, bool i, JsValueRef* a, unsigned short n, void* s)
{ return OurNotifyBool(c, i, a, n, s, g_origNotifyAnim, L"Animations"); }
static JsValueRef CALLBACK OurNotifyTrans(JsValueRef c, bool i, JsValueRef* a, unsigned short n, void* s)
{ return OurNotifyBool(c, i, a, n, s, g_origNotifyTrans, L"Transitions"); }

// ---------------- JsCreateNamedFunction hook ----------------

static int g_namedLogged = 0;

static JsErrorCode WINAPI HookJsCreateNamedFunction(JsValueRef name, JsNativeFunction nativeFunction,
                                                    void* callbackState, JsValueRef* function)
{
    std::wstring n;
    if (JsValueToWString(name, n) && n.size() < 200)
    {
        if (g_namedLogged < 120)
        {
            g_namedLogged++;
            Log(L"[hook] JsCreateNamedFunction: %s", n.c_str());
        }
        if (n == L"NativeGetImageItemDurationMs")
        {
            nativeFunction = OurGetDuration; // fully replaced; original unused
        }
        else if (n == L"NativeGetInitialSettings")
        {
            g_origGetInitSettings = nativeFunction;
            nativeFunction = OurGetInitSettings;
        }
        else if (n == L"NativeNotifyLoopChanged")
        {
            g_origNotifyLoop = nativeFunction;
            nativeFunction = OurNotifyLoop;
        }
        else if (n == L"NativeNotifyAreAnimationsActiveChanged")
        {
            g_origNotifyAnim = nativeFunction;
            nativeFunction = OurNotifyAnim;
        }
        else if (n == L"NativeNotifyAreTransitionsActiveChanged")
        {
            g_origNotifyTrans = nativeFunction;
            nativeFunction = OurNotifyTrans;
        }
    }
    return g_realCreateNamed(name, nativeFunction, callbackState, function);
}

// ---------------- delay-load IAT patching ----------------
//
// Slideshow.dll delay-loads chakra.dll. ResolveDelayLoadedAPI does not go
// through the GetProcAddress export, so we instead locate the IAT slot for
// JsCreateNamedFunction inside Slideshow.dll's delay-import descriptor and
// overwrite it once the slot has been bound to a chakra.dll address.

static bool IsChakraAddr(ULONG_PTR v)
{
    HMODULE h = GetModuleHandleW(L"chakra.dll");
    if (!h) return false;
    ULONG_PTR b = (ULONG_PTR)h;
    return v >= b && v < b + 0x400000; // chakra.dll is ~3-4 MB
}

// returns slot address for the named delay-import, or NULL
static BYTE* FindDelayImportSlot(HMODULE hSlideshow, const char* dllName, const char* funcName)
{
    BYTE* base = (BYTE*)hSlideshow;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return NULL;
    IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return NULL;
    DWORD ddRva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT].VirtualAddress;
    if (!ddRva || ddRva >= nt->OptionalHeader.SizeOfImage) return NULL;

    // IMAGE_DELAYLOAD_DESCRIPTOR (32 bytes, 8 dwords); cap iterations so a
    // malformed table can't loop us out of bounds
    struct DD { DWORD attr, name, hmod, iat, int_, bound, unload, ts; };
    for (DD* d = (DD*)(base + ddRva); d->name && d < (DD*)(base + nt->OptionalHeader.SizeOfImage); d++)
    {
        const char* dn = (d->attr & 1) ? (const char*)(base + d->name) : (const char*)d->name;
        if (_stricmp(dn, dllName) != 0) continue;

        // walk import name table; entries are RVA (attr&1) or VA of hint/name.
        // Cap iterations - a corrupt table must not read out of bounds.
        for (DWORD i = 0; i < 4096; i++)
        {
            ULONG_PTR ent = *(ULONG_PTR*)(base + d->int_ + i * 8);
            if (!ent) break;
            const char* hn;
            if (d->attr & 1) hn = (const char*)(base + (DWORD)ent) + 2;
            else hn = (const char*)ent + 2;
            if (strcmp(hn, funcName) == 0)
                return base + d->iat + i * 8;
        }
        return NULL;
    }
    return NULL;
}

static bool TryPatchIatSlot(HMODULE hSlideshow, const char* funcName, ULONG_PTR newAddr, ULONG_PTR* outOld)
{
    BYTE* slot = FindDelayImportSlot(hSlideshow, "chakra.dll", funcName);
    if (!slot) { Log(L"[hook] %S IAT slot not found", funcName); return false; }
    ULONG_PTR cur = *(ULONG_PTR*)slot;
    if (!IsChakraAddr(cur)) return false; // not bound yet
    if (outOld) *outOld = cur;
    DWORD old = 0;
    if (!VirtualProtect(slot, 8, PAGE_READWRITE, &old)) return false;
    *(ULONG_PTR*)slot = newAddr;
    VirtualProtect(slot, 8, old, &old);
    FlushInstructionCache(GetCurrentProcess(), slot, 8);
    Log(L"[hook] patched %S IAT @%p (was %p)", funcName, slot, (void*)cur);
    return true;
}

static ULONG_PTR g_realJsRunScript = 0;

// Legacy JSRT signature used by Windows chakra.dll:
//   JsErrorCode JsRunScript(const wchar_t* script, unsigned sourceContext,
//                           const wchar_t* sourceUrl, JsValueRef* result)
typedef JsErrorCode (WINAPI *JsRunScript_t)(const wchar_t* script, unsigned sourceContext,
                                            const wchar_t* sourceUrl, JsValueRef* result);

// Replace numeric literals that directly follow "imageItemDurationMs"
// (e.g. "imageItemDurationMs:4e3", "imageItemDurationMs??4e3") with g_durationMs.
static bool RewriteDurationLiterals(std::wstring& s)
{
    const wchar_t* marker = L"imageItemDurationMs";
    std::wstring rep;
    bool changed = false;
    size_t pos = 0;
    while ((pos = s.find(marker, pos)) != std::wstring::npos)
    {
        size_t i = pos + wcslen(marker);
        // allow only ":?= \t\n\r" between marker and the number
        size_t j = i;
        while (j < s.size() && (s[j] == L':' || s[j] == L'?' || s[j] == L'=' ||
                                s[j] == L' ' || s[j] == L'\t' || s[j] == L'\n' || s[j] == L'\r'))
            j++;
        if (j - i > 8 || j >= s.size() ||
            !(iswdigit(s[j]) || s[j] == L'.')) { pos = i; continue; }
        size_t numStart = j;
        // digits / '.' / exponent marker; a sign is only valid right after e/E
        // (so "5000-1" isn't swallowed as one "number")
        while (j < s.size() && (iswdigit(s[j]) || s[j] == L'.' || s[j] == L'e' || s[j] == L'E'))
            j++;
        if (j < s.size() && (s[j] == L'+' || s[j] == L'-') && j > numStart &&
            (s[j - 1] == L'e' || s[j - 1] == L'E'))
        {
            j++;
            while (j < s.size() && iswdigit(s[j])) j++;
        }
        // must look like a real number literal (digits present, sane length)
        std::wstring num = s.substr(numStart, j - numStart);
        if (num.size() > 12 || num.find_first_of(L"0123456789") == std::wstring::npos) { pos = i; continue; }
        if (rep.empty()) rep = std::to_wstring(g_durationMs);
        s.replace(numStart, j - numStart, rep);
        changed = true;
        pos = numStart + rep.size();
    }
    return changed;
}

static JsErrorCode WINAPI HookJsRunScript(const wchar_t* script, unsigned sourceContext,
                                          const wchar_t* sourceUrl, JsValueRef* result)
{
    static JsRunScript_t realFn = NULL;
    if (!realFn) realFn = (JsRunScript_t)g_realJsRunScript;
    // Cheap pre-filter: the marker only exists in slideshowNative.js, so skip
    // the full-script scan for anything else (JsRunScript is called often).
    if (script && sourceUrl && !wcsstr(sourceUrl, L"slideshow"))
        return realFn(script, sourceContext, sourceUrl, result);
    if (script && wcsstr(script, L"imageItemDurationMs"))
    {
        std::wstring s(script);
        if (RewriteDurationLiterals(s))
        {
            static int logged = 0;
            if (++logged <= 3)
                Log(L"[hook] rewrote imageItemDurationMs literals in slideshow JS (%zu chars)", s.size());
            return realFn(s.c_str(), sourceContext, sourceUrl, result);
        }
    }
    return realFn(script, sourceContext, sourceUrl, result);
}

static bool TryPatchJsRunScript(HMODULE hSlideshow)
{
    ULONG_PTR old = 0;
    if (!TryPatchIatSlot(hSlideshow, "JsRunScript", (ULONG_PTR)HookJsRunScript, &old))
        return false;
    g_realJsRunScript = old;
    return true;
}

// ---------------- main thread ----------------

static DWORD WINAPI PatchThread(LPVOID)
{
    Log(L"[hook] thread started, pid=%lu", GetCurrentProcessId());
    g_durationMs = ReadDurationMs();

    HMODULE hSlideshow = NULL;
    bool slideshowPatched = false;
    bool createNamedDone = false;
    bool runScriptDone = false;
    int polls = 0;

    for (;;)
    {
        if (!hSlideshow)
            hSlideshow = GetModuleHandleW(L"Slideshow.dll");

        if (hSlideshow && !slideshowPatched)
        {
            PatchSlideshowConstant(hSlideshow, g_durationMs);
            slideshowPatched = true;
        }

        // Once chakra.dll is loaded, Slideshow.dll's delay-load helper binds
        // its IAT; patch the slots as soon as they resolve.
        if (hSlideshow && GetModuleHandleW(L"chakra.dll"))
        {
            if (!createNamedDone)
            {
                ULONG_PTR old = 0;
                if (TryPatchIatSlot(hSlideshow, "JsCreateNamedFunction",
                                    (ULONG_PTR)HookJsCreateNamedFunction, &old))
                {
                    g_realCreateNamed = (JsCreateNamedFunction_t)old;
                    createNamedDone = true;
                }
            }
            if (!runScriptDone)
                runScriptDone = TryPatchJsRunScript(hSlideshow);
        }

        if (slideshowPatched && createNamedDone && runScriptDone) break;

        // give up after ~60 s rather than polling forever inside Photos.exe
        // (e.g. if a future build lacks one of the delay-load slots)
        if (++polls > 30000)
        {
            Log(L"[hook] timed out waiting for patches (slideshow=%d cn=%d rs=%d)",
                (int)slideshowPatched, (int)createNamedDone, (int)runScriptDone);
            break;
        }
        if (polls % 200 == 0)
            Log(L"[hook] waiting (poll %d, slideshow=%p chakra=%p cn=%d rs=%d)", polls, hSlideshow,
                GetModuleHandleW(L"chakra.dll"), (int)createNamedDone, (int)runScriptDone);
        Sleep(2);
    }
    Log(L"[hook] init done, duration=%d ms", g_durationMs);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_hModule = hModule;
        DisableThreadLibraryCalls(hModule);
        HANDLE t = CreateThread(NULL, 0, PatchThread, NULL, 0, NULL);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
