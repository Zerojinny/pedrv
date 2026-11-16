// pedrv - WinPE offline driver installer
//
// Enumerates the hardware of the machine it runs on (normally from WinPE),
// finds the matching driver packages in the given sources (folders, .zip,
// .7z, .wim) and injects them into an offline Windows installation.
//
// Console messages are English on purpose: WinPE usually has no CJK fonts.

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <map>
#include <set>
#include <string>
#include <vector>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "version.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "cfgmgr32.lib")

using std::wstring;
using std::vector;

#define PEDRV_VERSION L"1.1"
#define PEDRV_BANNER  L"pedrv " PEDRV_VERSION L" (built " _CRT_WIDE(__DATE__) L")"

// Set by the first Ctrl+C: the current step finishes, then pedrv cleans up and exits.
static volatile LONG g_abort = 0;

// Child processes that install a driver are killed after this long (-2 from Run).
static const DWORD kInstallTimeout = 30 * 60 * 1000;

// ---------------------------------------------------------------- output ---

static void Out(const wchar_t* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = _vscwprintf(fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    wstring s((size_t)n + 1, L'\0');
    va_start(ap, fmt);
    vswprintf(&s[0], s.size(), fmt, ap);
    va_end(ap);
    s.resize((size_t)n);

    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode, written;
    if (GetConsoleMode(h, &mode)) {
        WriteConsoleW(h, s.c_str(), (DWORD)s.size(), &written, nullptr);
    } else {
        int len = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
        std::string u((size_t)len, '\0');
        WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &u[0], len, nullptr, nullptr);
        WriteFile(h, u.data(), (DWORD)u.size(), &written, nullptr);
    }
}

static wstring ErrText(DWORD code)
{
    wchar_t buf[512] = L"";
    DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code,
                             MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), buf, 512, nullptr);
    if (!n) n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, buf, 512, nullptr);
    while (n && (buf[n - 1] == L'\r' || buf[n - 1] == L'\n' || buf[n - 1] == L' ' || buf[n - 1] == L'.')) buf[--n] = 0;
    wchar_t out[600];
    swprintf(out, 600, n ? L"0x%08X (%s)" : L"0x%08X", code, buf);
    return out;
}

// ----------------------------------------------------------- path helpers ---

static wstring Upper(wstring s)
{
    if (!s.empty()) CharUpperBuffW(&s[0], (DWORD)s.size());
    return s;
}

static wstring Join(const wstring& a, const wstring& b)
{
    if (a.empty()) return b;
    if (b.empty()) return a;
    return (a.back() == L'\\' || a.back() == L'/') ? a + b : a + L'\\' + b;
}

static wstring DirOf(const wstring& p)
{
    size_t i = p.find_last_of(L"\\/");
    return i == wstring::npos ? L"" : p.substr(0, i);
}

static wstring NameOf(const wstring& p)
{
    size_t i = p.find_last_of(L"\\/");
    return i == wstring::npos ? p : p.substr(i + 1);
}

static wstring ExtOf(const wstring& p)
{
    wstring n = NameOf(p);
    size_t i = n.rfind(L'.');
    return i == wstring::npos ? L"" : Upper(n.substr(i));
}

// Long-path form ("\\?\C:\..." / "\\?\UNC\...") of an absolute path for the file APIs,
// so deep folders (extracted archives, slim copies) do not hit MAX_PATH. Short paths are
// returned unchanged. The path must be normalized (no "." / ".." / forward slashes).
static wstring LongPath(const wstring& p)
{
    if (p.size() < 240 || !p.compare(0, 4, L"\\\\?\\")) return p;
    if (p.size() > 2 && p[1] == L':' && p[2] == L'\\') return L"\\\\?\\" + p;
    if (!p.compare(0, 2, L"\\\\")) return L"\\\\?\\UNC\\" + p.substr(2);
    return p;
}

// Path of p relative to the folder root, which may be a drive root such as "D:\".
static wstring RelPath(const wstring& root, const wstring& p)
{
    size_t n = root.size();
    if (n && root.back() != L'\\' && root.back() != L'/') n++;
    return n <= p.size() ? p.substr(n) : p;
}

static bool IsFile(const wstring& p)
{
    DWORD a = GetFileAttributesW(LongPath(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool IsDir(const wstring& p)
{
    DWORD a = GetFileAttributesW(LongPath(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static wstring FullPath(const wstring& p)
{
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetFullPathNameW(p.c_str(), ARRAYSIZE(buf), buf, nullptr);
    if (!n || n >= ARRAYSIZE(buf)) return p;
    wstring r = buf;
    while (r.size() > 3 && r.back() == L'\\') r.pop_back();
    return r;
}

static bool MakeDirs(const wstring& p)
{
    if (IsDir(p)) return true;
    wstring parent = DirOf(p);
    if (!parent.empty() && parent.size() > 2 && !IsDir(parent)) MakeDirs(parent);
    return CreateDirectoryW(LongPath(p).c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

// Deletes a tree. Never descends into reparse points (junctions, mounted images).
static void DeleteTree(const wstring& dir)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(LongPath(Join(dir, L"*")).c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            wstring p = Join(dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY)
                SetFileAttributesW(LongPath(p).c_str(), fd.dwFileAttributes & ~FILE_ATTRIBUTE_READONLY);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) RemoveDirectoryW(LongPath(p).c_str());
                else DeleteTree(p);
            } else {
                DeleteFileW(LongPath(p).c_str());
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(LongPath(dir).c_str());
}

// Total size of the files under a folder.
static ULONGLONG TreeSize(const wstring& dir)
{
    ULONGLONG total = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(LongPath(Join(dir, L"*")).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) total += TreeSize(Join(dir, fd.cFileName));
        } else {
            total += ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return total;
}

static ULONGLONG FileSize(const wstring& p)
{
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(LongPath(p).c_str(), GetFileExInfoStandard, &d)) return 0;
    return ((ULONGLONG)d.nFileSizeHigh << 32) | d.nFileSizeLow;
}

static bool ReadWholeFile(const wstring& p, std::string& data)
{
    data.clear();
    HANDLE h = CreateFileW(LongPath(p).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size = {};
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart < (64 << 20);
    if (ok && size.QuadPart) {
        data.resize((size_t)size.QuadPart);
        DWORD got = 0;
        ok = ReadFile(h, &data[0], (DWORD)data.size(), &got, nullptr) && got == data.size();
    }
    CloseHandle(h);
    return ok;
}

// Recursive. Like TreeSize and DeleteTree it never follows a junction or symbolic link:
// a source such as C:\ has junction loops ("Application Data") and links to other volumes.
static void FindInfs(const wstring& dir, vector<wstring>& out)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(LongPath(Join(dir, L"*")).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        wstring p = Join(dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) FindInfs(p, out);
        }
        else if (ExtOf(p) == L".INF") out.push_back(p);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// Command line quoting that survives trailing backslashes ("C:\" problem).
static wstring Q(const wstring& s)
{
    wstring r = L"\"";
    size_t bs = 0;
    for (wchar_t c : s) {
        if (c == L'\\') { bs++; r += c; continue; }
        if (c == L'"') { r.append(bs + 1, L'\\'); }
        bs = 0;
        r += c;
    }
    r.append(bs, L'\\');
    r += L'"';
    return r;
}

// Runs a child process. Its stdout/stderr go to outFile if given, else are discarded.
// Returns the exit code, -1 if it cannot be started, -2 if it ran longer than timeoutMs
// (it is then terminated).
static int Run(const wstring& cmdline, DWORD timeoutMs = INFINITE, const wstring& outFile = wstring())
{
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    HANDLE out = outFile.empty()
        ? CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr)
        : CreateFileW(LongPath(outFile).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS, 0, nullptr);
    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nullptr;
    si.hStdOutput = out;
    si.hStdError = out;
    PROCESS_INFORMATION pi = {};
    wstring cmd = cmdline;
    BOOL ok = CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi);
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    if (!ok) return -1;
    if (WaitForSingleObject(pi.hProcess, timeoutMs) == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, (UINT)-2);
        WaitForSingleObject(pi.hProcess, 10000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return -2;
    }
    DWORD code = (DWORD)-1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}

static wstring ExeDir()
{
    wchar_t buf[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, buf, ARRAYSIZE(buf));
    return DirOf(buf);
}

static wstring SearchExe(const wchar_t* name)
{
    wchar_t buf[MAX_PATH * 2];
    DWORD n = SearchPathW(nullptr, name, nullptr, ARRAYSIZE(buf), buf, nullptr);
    return (n && n < ARRAYSIZE(buf)) ? wstring(buf) : wstring();
}

// ------------------------------------------------------- target platform ---

struct Platform {
    wstring arch;           // AMD64 / X86 / ARM64 / ARM
    DWORD major = 0, minor = 0, build = 0;
    bool verKnown = false;
    DWORD productType = 0;  // 1 workstation, 2 domain controller, 3 server (INF values); 0 unknown
};

// Machine type from the PE header of an executable, 0 if it is not one.
static WORD PeMachine(const wstring& path)
{
    HANDLE h = CreateFileW(LongPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0;
    IMAGE_DOS_HEADER dos = {};
    DWORD rd = 0, sig = 0;
    IMAGE_FILE_HEADER fh = {};
    bool ok = ReadFile(h, &dos, sizeof(dos), &rd, nullptr) && rd == sizeof(dos) && dos.e_magic == IMAGE_DOS_SIGNATURE
           && SetFilePointer(h, dos.e_lfanew, nullptr, FILE_BEGIN) != INVALID_SET_FILE_POINTER
           && ReadFile(h, &sig, 4, &rd, nullptr) && rd == 4 && sig == IMAGE_NT_SIGNATURE
           && ReadFile(h, &fh, sizeof(fh), &rd, nullptr) && rd == sizeof(fh);
    CloseHandle(h);
    return ok ? fh.Machine : 0;
}

static const wchar_t* ArchName(WORD machine)
{
    switch (machine) {
    case IMAGE_FILE_MACHINE_AMD64: return L"AMD64";
    case IMAGE_FILE_MACHINE_I386:  return L"X86";
    case IMAGE_FILE_MACHINE_ARM64: return L"ARM64";
    case IMAGE_FILE_MACHINE_ARMNT: return L"ARM";
    }
    return L"";
}

// Reads architecture and version of a Windows installation from its kernel.
static bool ReadPlatform(const wstring& winDir, Platform& p)
{
    wstring kernel = Join(winDir, L"System32\\ntoskrnl.exe");
    p.arch = ArchName(PeMachine(kernel));
    if (p.arch.empty()) return false;

    // Read the version resource directly: GetFileVersionInfo is subject to the
    // version-lie compatibility shim (reports 6.2 for system files).
    HMODULE mod = LoadLibraryExW(kernel.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (mod) {
        HRSRC res = FindResourceW(mod, MAKEINTRESOURCEW(1), RT_VERSION);
        HGLOBAL mem = res ? LoadResource(mod, res) : nullptr;
        const BYTE* data = mem ? (const BYTE*)LockResource(mem) : nullptr;
        DWORD size = res ? SizeofResource(mod, res) : 0;
        for (DWORD i = 0; data && i + sizeof(VS_FIXEDFILEINFO) <= size; i += 4) {
            const VS_FIXEDFILEINFO* ffi = (const VS_FIXEDFILEINFO*)(data + i);
            if (ffi->dwSignature != VS_FFI_SIGNATURE) continue;
            p.major = HIWORD(ffi->dwFileVersionMS);
            p.minor = LOWORD(ffi->dwFileVersionMS);
            p.build = HIWORD(ffi->dwFileVersionLS);
            p.verKnown = true;
            break;
        }
        FreeLibrary(mod);
    }
    return true;
}

// ProductOptions\ProductType of a SYSTEM hive as the INF ProductType value.
static DWORD ProductTypeOf(HKEY root, const wchar_t* optionsKey)
{
    wchar_t type[64] = L"";
    DWORD size = sizeof(type);
    if (RegGetValueW(root, optionsKey, L"ProductType", RRF_RT_REG_SZ, nullptr, type, &size) != ERROR_SUCCESS) return 0;
    if (!_wcsicmp(type, L"WinNT")) return 1;
    if (!_wcsicmp(type, L"LanmanNT")) return 2;
    if (!_wcsicmp(type, L"ServerNT")) return 3;
    return 0;
}

static DWORD RunningProductType()
{
    return ProductTypeOf(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\ProductOptions");
}

// Product type of an offline Windows. Its SYSTEM hive is loaded from a copy in workDir
// (with the transaction logs, so a dirty hive is recovered), never from the image itself:
// DISM opens that file later. Returns 0 if it cannot be read.
static DWORD OfflineProductType(const wstring& winDir, const wstring& workDir)
{
    wstring config = Join(winDir, L"System32\\config"), dir = Join(workDir, L"hive");
    if (!MakeDirs(dir) || !CopyFileW(LongPath(Join(config, L"SYSTEM")).c_str(), LongPath(Join(dir, L"SYSTEM")).c_str(), FALSE))
        return 0;
    CopyFileW(LongPath(Join(config, L"SYSTEM.LOG1")).c_str(), LongPath(Join(dir, L"SYSTEM.LOG1")).c_str(), FALSE);
    CopyFileW(LongPath(Join(config, L"SYSTEM.LOG2")).c_str(), LongPath(Join(dir, L"SYSTEM.LOG2")).c_str(), FALSE);
    HKEY hive;
    if (RegLoadAppKeyW(Join(dir, L"SYSTEM").c_str(), &hive, KEY_READ, REG_PROCESS_APPKEY, 0) != ERROR_SUCCESS) return 0;
    DWORD current = 1, size = sizeof(current);
    RegGetValueW(hive, L"Select", L"Current", RRF_RT_REG_DWORD, nullptr, &current, &size);
    wchar_t key[64];
    swprintf(key, 64, L"ControlSet%03u\\Control\\ProductOptions", current);
    DWORD type = ProductTypeOf(hive, key);
    RegCloseKey(hive);
    return type;
}

// A [Manufacturer] TargetOSVersion decoration:
// NT[Architecture][.[OSMajor][.[OSMinor][.[ProductType][.[SuiteMask][.[BuildNumber]]]]]]
struct Decoration {
    bool valid = false;
    wstring arch;
    DWORD major = 0, minor = 0, productType = 0, suite = 0, build = 0;
    size_t parts = 0;
};

static Decoration ParseDecoration(const wstring& text)
{
    vector<wstring> parts;
    size_t start = 0;
    for (;;) {
        size_t dot = text.find(L'.', start);
        parts.push_back(text.substr(start, dot == wstring::npos ? wstring::npos : dot - start));
        if (dot == wstring::npos) break;
        start = dot + 1;
    }
    Decoration d;
    wstring head = Upper(parts[0]);
    if (head.compare(0, 2, L"NT") != 0) return d;
    auto num = [&](size_t i, int base) -> DWORD { return i < parts.size() ? wcstoul(parts[i].c_str(), nullptr, base) : 0; };
    d.valid = true;
    d.arch = head.substr(2);
    d.parts = parts.size();
    d.major = num(1, 10);
    d.minor = num(2, 10);
    d.productType = num(3, 0);     // may be written as 0x0000003
    d.suite = num(4, 0);
    d.build = num(5, 10);
    return d;
}

// Does the decoration apply to the target?
static bool DecorationApplies(const Decoration& d, const Platform& p)
{
    if (!d.valid) return false;
    if (!d.arch.empty() && d.arch != p.arch) return false;
    if (d.arch.empty() && p.arch != L"X86" && d.parts == 1) return false;   // bare ".NT" is x86 legacy
    if (d.productType && p.productType && d.productType != p.productType) return false;
    if (!p.verKnown) return true;
    if (d.major) {
        if (p.major < d.major || (p.major == d.major && p.minor < d.minor)) return false;
        if (p.major == d.major && p.minor == d.minor && d.build && p.build < d.build) return false;
    }
    return true;
}

// Of two applicable decorations, is a the closer match? Like Windows: the highest OS
// version wins, then the one that names the architecture, then product type and suite.
static bool DecorationBetter(const Decoration& a, const Decoration& b)
{
    if (a.major != b.major) return a.major > b.major;
    if (a.minor != b.minor) return a.minor > b.minor;
    if (a.build != b.build) return a.build > b.build;
    if (a.arch.empty() != b.arch.empty()) return !a.arch.empty();
    if ((a.productType != 0) != (b.productType != 0)) return a.productType != 0;
    return a.suite != 0 && b.suite == 0;
}

// ----------------------------------------------------------------- devices ---

struct Device {
    wstring desc;
    wstring firstId;
    wstring instance;
};

struct IdRef { int dev; int rank; };   // rank: lower is a better match

static vector<Device> g_devices;
static std::map<wstring, vector<IdRef>> g_idMap;

static bool GetMultiSz(HDEVINFO set, SP_DEVINFO_DATA& dev, DWORD prop, vector<wstring>& out)
{
    DWORD type = 0, need = 0;
    SetupDiGetDeviceRegistryPropertyW(set, &dev, prop, &type, nullptr, 0, &need);
    if (!need) return false;
    vector<BYTE> buf(need + 4, 0);
    if (!SetupDiGetDeviceRegistryPropertyW(set, &dev, prop, &type, buf.data(), need, nullptr)) return false;
    const wchar_t* p = (const wchar_t*)buf.data();
    if (type == REG_SZ) { if (*p) out.push_back(p); return !out.empty(); }
    for (; *p; p += wcslen(p) + 1) out.push_back(p);
    return !out.empty();
}

// Drive WinPE booted from (the one holding boot.wim), e.g. "D:\"; empty if not WinPE or not
// recorded. WinPE writes it to Control\PEBootRamdiskSourceDrive at boot.
static wstring PEBootDrive()
{
    wchar_t value[64] = L"";
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control", L"PEBootRamdiskSourceDrive",
                     RRF_RT_REG_SZ, nullptr, value, &size) != ERROR_SUCCESS)
        return wstring();
    if (!iswalpha(value[0]) || value[1] != L':') return wstring();
    wchar_t root[] = { (wchar_t)towupper(value[0]), L':', L'\\', 0 };
    return GetDriveTypeW(root) == DRIVE_NO_ROOT_DIR ? wstring() : wstring(root);
}

static bool IsWinPE()
{
    HKEY key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\MiniNT", 0, KEY_READ, &key) != ERROR_SUCCESS)
        return false;
    RegCloseKey(key);
    return true;
}

// Some WinPE/WinRE images have GraphicsDrivers\ForceBddFallbackOnly=1 in
// their SYSTEM hive, which makes dxgkrnl reject every
// display driver except the Basic Display Driver (device ends up with code 31).
// dxgkrnl reads it at boot: clearing it in the running PE has no effect, it
// has to be removed from the PE image's SYSTEM hive at build time.
static bool DisplayDriversBlocked()
{
    HKEY key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers", 0,
                      KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return false;
    DWORD value = 0, size = sizeof(value), type = 0;
    bool blocked = RegQueryValueExW(key, L"ForceBddFallbackOnly", nullptr, &type, (BYTE*)&value, &size) == ERROR_SUCCESS
                && type == REG_DWORD && value != 0;
    RegCloseKey(key);
    return blocked;
}

// Asks PnP to re-enumerate the device tree and waits for installs to settle.
static void RescanDevices()
{
    DEVINST root;
    if (CM_Locate_DevNodeW(&root, nullptr, CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS)
        CM_Reenumerate_DevNode(root, CM_REENUMERATE_SYNCHRONOUS);
    Sleep(2000);
    CMP_WaitNoPendingInstallEvents(15000);
}

static void EnumerateDevices(bool hardwareIdsOnly)
{
    g_devices.clear();
    g_idMap.clear();
    HDEVINFO set = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return;
    SP_DEVINFO_DATA dev = { sizeof(dev) };
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &dev); i++) {
        vector<wstring> hw, compat, desc;
        GetMultiSz(set, dev, SPDRP_HARDWAREID, hw);
        if (!hardwareIdsOnly) GetMultiSz(set, dev, SPDRP_COMPATIBLEIDS, compat);
        if (hw.empty() && compat.empty()) continue;
        if (!GetMultiSz(set, dev, SPDRP_FRIENDLYNAME, desc)) GetMultiSz(set, dev, SPDRP_DEVICEDESC, desc);

        Device d;
        d.firstId = hw.empty() ? compat[0] : hw[0];
        d.desc = desc.empty() ? d.firstId : desc[0];
        wchar_t instance[MAX_DEVICE_ID_LEN + 1];
        if (SetupDiGetDeviceInstanceIdW(set, &dev, instance, ARRAYSIZE(instance), nullptr)) d.instance = instance;
        int index = (int)g_devices.size();
        g_devices.push_back(d);
        for (size_t k = 0; k < hw.size(); k++) g_idMap[Upper(hw[k])].push_back({ index, (int)k });
        for (size_t k = 0; k < compat.size(); k++) g_idMap[Upper(compat[k])].push_back({ index, 0x1000 + (int)k });
    }
    SetupDiDestroyDeviceInfoList(set);
}

// --------------------------------------------------------------------- INF ---

struct Inf {
    int source = 0;
    wstring path;
    wstring cls, provider, verText;
    unsigned date = 0;                  // yyyymmdd
    unsigned long long version = 0;
    vector<wstring> ids;                // upper-case IDs of the models sections that apply to the target
    std::map<int, int> devRank;         // device index -> best rank (recomputed every pass)
    bool selected = false;
};

static wstring InfField(PINFCONTEXT ctx, DWORD index)
{
    wchar_t buf[4096];
    if (!SetupGetStringFieldW(ctx, index, buf, ARRAYSIZE(buf), nullptr)) return L"";
    return buf;
}

static void ReadModelsSection(HINF h, const wstring& section, vector<wstring>& ids)
{
    INFCONTEXT ctx;
    if (!SetupFindFirstLineW(h, section.c_str(), nullptr, &ctx)) return;
    do {
        DWORD fields = SetupGetFieldCount(&ctx);
        for (DWORD f = 2; f <= fields; f++) {
            wstring id = Upper(InfField(&ctx, f));
            if (!id.empty()) ids.push_back(id);
        }
    } while (SetupFindNextLine(&ctx, &ctx));
}

// Matches the IDs of an INF against the current devices (g_idMap).
static void MatchInf(Inf& inf)
{
    inf.devRank.clear();
    for (const wstring& id : inf.ids) {
        auto it = g_idMap.find(id);
        if (it == g_idMap.end()) continue;
        for (const IdRef& r : it->second) {
            auto cur = inf.devRank.find(r.dev);
            if (cur == inf.devRank.end() || r.rank < cur->second) inf.devRank[r.dev] = r.rank;
        }
    }
}

static wstring g_runDir;        // per-run scratch directory, removed on exit

// SetupAPI cannot open an INF by a long ("\\?\") path; such an INF is read from a short
// copy in the scratch directory instead.
static HINF OpenInf(const wstring& path)
{
    if (path.size() < 240 || g_runDir.empty()) return SetupOpenInfFileW(path.c_str(), nullptr, INF_STYLE_WIN4, nullptr);
    wstring copy = Join(g_runDir, L"longpath.inf");
    if (!CopyFileW(LongPath(path).c_str(), copy.c_str(), FALSE)) return INVALID_HANDLE_VALUE;
    return SetupOpenInfFileW(copy.c_str(), nullptr, INF_STYLE_WIN4, nullptr);
}

// Parses an INF and collects the device IDs it offers the target. Returns false if it is
// not a usable driver INF for the target.
static bool ParseInf(const wstring& path, const Platform& plat, Inf& inf)
{
    HINF h = OpenInf(path);
    if (h == INVALID_HANDLE_VALUE) return false;
    inf.path = path;

    INFCONTEXT ctx;
    if (SetupFindFirstLineW(h, L"Version", L"Class", &ctx)) inf.cls = InfField(&ctx, 1);
    if (SetupFindFirstLineW(h, L"Version", L"Provider", &ctx)) inf.provider = InfField(&ctx, 1);
    if (SetupFindFirstLineW(h, L"Version", L"DriverVer", &ctx)) {
        wstring date = InfField(&ctx, 1), ver = InfField(&ctx, 2);
        unsigned m = 0, d = 0, y = 0, a = 0, b = 0, c = 0, e = 0;
        if (swscanf(date.c_str(), L"%u/%u/%u", &m, &d, &y) == 3) inf.date = y * 10000 + m * 100 + d;
        swscanf(ver.c_str(), L"%u.%u.%u.%u", &a, &b, &c, &e);
        inf.version = ((unsigned long long)(a & 0xFFFF) << 48) | ((unsigned long long)(b & 0xFFFF) << 32)
                    | ((unsigned long long)(c & 0xFFFF) << 16) | (e & 0xFFFF);
        inf.verText = ver.empty() ? date : ver + L" " + date;
    }

    // Windows uses one models section per manufacturer line: the closest applicable
    // decoration, else (x86 only) the undecorated section. With an unknown OS version the
    // closest one cannot be told, so every applicable section is used.
    if (SetupFindFirstLineW(h, L"Manufacturer", nullptr, &ctx)) {
        do {
            wstring models = InfField(&ctx, 1);
            if (models.empty()) continue;
            DWORD fields = SetupGetFieldCount(&ctx);
            Decoration best;
            wstring bestText;
            for (DWORD f = 2; f <= fields; f++) {
                wstring text = InfField(&ctx, f);
                if (text.empty()) continue;
                Decoration d = ParseDecoration(text);
                if (!DecorationApplies(d, plat)) continue;
                if (!plat.verKnown) { ReadModelsSection(h, models + L"." + text, inf.ids); continue; }
                if (bestText.empty() || DecorationBetter(d, best)) { best = d; bestText = text; }
            }
            if (!bestText.empty()) ReadModelsSection(h, models + L"." + bestText, inf.ids);
            else if (plat.arch == L"X86") ReadModelsSection(h, models, inf.ids);
        } while (SetupFindNextLine(&ctx, &ctx));
    }
    SetupCloseInfFile(h);
    std::sort(inf.ids.begin(), inf.ids.end());
    inf.ids.erase(std::unique(inf.ids.begin(), inf.ids.end()), inf.ids.end());
    return !inf.ids.empty();
}

// -------------------------------------------------------------------- slim ---
//
// Display driver packages have grown to gigabytes, most of it irrelevant for
// showing a desktop in WinPE (WSL libraries, 32-bit DLLs, CUDA, video codecs...).
// A slim copy keeps the package layout but leaves those files empty, so it fits
// a RAM disk. The INF and catalog are untouched; WinPE ignores the hash mismatch
// of the emptied files ("tampered, but error is ignored in WinPE"). Never used
// for an offline Windows, which needs the complete, verifiable package.

struct SlimRules {
    std::set<wstring> wsl;      // upper-case names of files the INF copies to a WSL (lxss) folder
    bool nvidia = false;        // NVIDIA display package: drop the compute/AI/capture components
    bool amd = false;           // AMD display package: drop FidelityFX, AMF runtime, bundled installers
    bool keep32 = true;         // target can run 32-bit code (x86, or WinPE with WoW64)
};

struct SlimStats {
    ULONGLONG before = 0, after = 0;
    unsigned emptied = 0;
};

static void AddSlimRules(const Inf& inf, SlimRules& r)
{
    if (!_wcsicmp(inf.cls.c_str(), L"Display")) {
        wstring prov = Upper(inf.provider);
        if (prov.find(L"NVIDIA") != wstring::npos) r.nvidia = true;
        if (prov.find(L"ADVANCED MICRO DEVICES") != wstring::npos || !prov.compare(0, 3, L"AMD")) r.amd = true;
    }
    HINF h = OpenInf(inf.path);
    if (h == INVALID_HANDLE_VALUE) return;
    INFCONTEXT ctx;
    if (SetupFindFirstLineW(h, L"DestinationDirs", nullptr, &ctx)) {
        do {                                        // section = dirid,subdir
            if (Upper(InfField(&ctx, 2)).find(L"LXSS") == wstring::npos) continue;
            wstring section = InfField(&ctx, 0);
            INFCONTEXT file;
            if (!SetupFindFirstLineW(h, section.c_str(), nullptr, &file)) continue;
            do {                                    // target[,source]
                wstring src = InfField(&file, 2);
                if (src.empty()) src = InfField(&file, 1);
                if (!src.empty()) r.wsl.insert(Upper(src));
            } while (SetupFindNextLine(&file, &file));
        } while (SetupFindNextLine(&ctx, &ctx));
    }
    SetupCloseInfFile(h);
}

static bool IsX86Image(const wstring& path) { return PeMachine(path) == IMAGE_FILE_MACHINE_I386; }

// rel: path of the file relative to the package folder.
static bool SlimDrops(const wstring& path, const wstring& rel, const SlimRules& r)
{
    wstring name = Upper(NameOf(path)), ext = ExtOf(path);
    if (ext == L".INF" || ext == L".CAT" || ext == L".SYS") return false;
    if (r.wsl.count(name)) return true;
    if (ext == L".SO" || name.find(L".SO.") != wstring::npos) return true;         // Linux libraries for WSL
    if (r.nvidia) {
        if (Upper(rel).compare(0, 9, L"NVCAMERA\\") == 0) return true;             // Ansel
        // CUDA, OptiX, NVVM, OpenCL, video codecs, DirectML kernels, NGX/DLSS, telemetry.
        // Kept on purpose: the kernel driver and its firmware, D3D/OpenGL/Vulkan drivers,
        // nvgpucomp (shader compiler), control panel and container service.
        static const wchar_t* const drop[] = {
            L"NVCUDA", L"NVCUBINS", L"NVOPTIX", L"NVRTUM", L"NVVM", L"NVPTXJIT", L"NVOPENCL", L"OPENCL",
            L"NVCUVID", L"NVENCODEAPI", L"NVDXDLKERNELS", L"NVTILEIRAS", L"NVDLPPX", L"NVDIRECTSR", L"NVNGX",
            L"NVAIVPX", L"NVOBJECTLOADER", L"NVTELEMETRY", L"NVGAMES", L"NVIDIA-PCC",
        };
        for (const wchar_t* p : drop)
            if (!name.compare(0, wcslen(p), p)) return true;
    }
    if (r.amd) {
        // FidelityFX library (loaded by the DX12/Vulkan drivers on demand), the alternative
        // Vulkan driver cabinet, the AMF video runtime and the bundled software installer.
        // Kept on purpose: everything the D3D11 driver loads in each process (amdn1b, amdenc,
        // amdihk), the D3D9/D3D12/OpenGL/Vulkan drivers and the video firmware (.dat).
        static const wchar_t* const drop[] = { L"AMDXCFFX", L"AMDVLK_NEXUS", L"AMFRTDRV", L"AMDINSTALLMANAGER" };
        for (const wchar_t* p : drop)
            if (!name.compare(0, wcslen(p), p)) return true;
    }
    return !r.keep32 && IsX86Image(path);
}

// Copies a package folder, leaving the files the rules drop empty. With an empty
// dst nothing is written (measures only). link: hard-link kept files where possible.
static bool SlimCopy(const wstring& src, const wstring& dst, const wstring& rel, const SlimRules& r, bool link, SlimStats& st)
{
    if (!dst.empty() && !MakeDirs(dst)) return false;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(LongPath(Join(src, L"*")).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        wstring from = Join(src, fd.cFileName), to = dst.empty() ? wstring() : Join(dst, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
            ok = SlimCopy(from, to, rel + fd.cFileName + L"\\", r, link, st);
            continue;
        }
        ULONGLONG size = ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        st.before += size;
        if (size && SlimDrops(from, rel + fd.cFileName, r)) {
            st.emptied++;
            if (to.empty()) continue;
            HANDLE f = CreateFileW(LongPath(to).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f == INVALID_HANDLE_VALUE) ok = false;
            else CloseHandle(f);
            continue;
        }
        st.after += size;
        if (to.empty()) continue;
        // A read-only file is copied: DeleteTree clears that attribute, and on a link
        // this would change the original as well.
        if (link && !(fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY) && CreateHardLinkW(LongPath(to).c_str(), LongPath(from).c_str(), nullptr)) continue;
        if (!CopyFileW(LongPath(from).c_str(), LongPath(to).c_str(), FALSE)) ok = false;
    } while (ok && FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}

// ----------------------------------------------------------------- sources ---

enum SourceKind { SRC_DIR, SRC_ARCHIVE, SRC_WIM };

struct Source {
    wstring spec;           // what the user gave us
    wstring file;           // archive / wim path
    SourceKind kind = SRC_DIR;
    DWORD wimIndex = 1;
    wstring root;           // directory holding the (possibly partial) content
    bool partial = false;   // only *.inf extracted so far
    bool mounted = false;
    wstring inboxInfDir;    // driver store of an offline Windows: its \Windows\INF, used to tell inbox drivers apart
};

// Inbox drivers are published in \Windows\INF under their own name, third-party ones as
// oemN.inf, a byte copy of the package INF. A vendor package may share its INF name with an
// inbox one (Realtek rt640x64.inf, for one), so a name match alone does not make it inbox:
// it is third-party if an oemN.inf has the same content.
struct OemInfs {
    bool loaded = false;
    std::set<std::pair<ULONGLONG, ULONGLONG>> keys;     // size, FNV-1a hash
};

static ULONGLONG Fnv1a(const std::string& data)
{
    ULONGLONG h = 14695981039346656037ull;
    for (unsigned char c : data) { h ^= c; h *= 1099511628211ull; }
    return h;
}

static bool IsInboxInf(const wstring& inf, const wstring& winInfDir, OemInfs& oem)
{
    if (!IsFile(Join(winInfDir, NameOf(inf)))) return false;
    if (!oem.loaded) {
        oem.loaded = true;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(LongPath(Join(winInfDir, L"oem*.inf")).c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            std::string data;
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && ReadWholeFile(Join(winInfDir, fd.cFileName), data))
                    oem.keys.insert({ (ULONGLONG)data.size(), Fnv1a(data) });
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    std::string data;
    if (!ReadWholeFile(inf, data)) return true;
    return !oem.keys.count({ (ULONGLONG)data.size(), Fnv1a(data) });
}

typedef BOOL(WINAPI* PFN_WIMMountImage)(PCWSTR, PCWSTR, DWORD, PCWSTR);
typedef BOOL(WINAPI* PFN_WIMUnmountImage)(PCWSTR, PCWSTR, DWORD, BOOL);

static vector<Source> g_sources;
static wstring g_7z, g_tar;
static bool g_7zIsFull = false; // 7z.exe/7za.exe (zip + 7z); 7zr.exe is 7z only
static PFN_WIMUnmountImage g_wimUnmount = nullptr;

static void Cleanup()
{
    bool safe = true;
    for (Source& s : g_sources) {
        if (!s.mounted) continue;
        if (g_wimUnmount && g_wimUnmount(s.root.c_str(), s.file.c_str(), s.wimIndex, FALSE)) s.mounted = false;
        else {
            safe = false;
            Out(L"WARNING: could not unmount %s (%s)\n", s.root.c_str(), ErrText(GetLastError()).c_str());
        }
    }
    if (g_runDir.empty()) return;
    if (safe) DeleteTree(g_runDir);
    else Out(L"WARNING: scratch directory left in place: %s\n", g_runDir.c_str());
    g_runDir.clear();
}

// The first Ctrl+C only sets g_abort: the main thread stops after the current step and
// takes its normal exit path (DISM session closed, WIMs unmounted, scratch removed), so no
// cleanup runs underneath a DISM call or a running 7-Zip. A second Ctrl+C, or closing the
// console, exits at once.
static BOOL WINAPI CtrlHandler(DWORD type)
{
    if ((type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) && !InterlockedExchange(&g_abort, 1)) {
        Out(L"\nInterrupted: stopping after the current step (Ctrl+C again to quit at once)...\n");
        return TRUE;
    }
    Out(L"\nInterrupted, cleaning up...\n");
    Cleanup();
    ExitProcess(3);
}

// targetRoots: roots of the offline Windows in use (/image, /fromwin); their
// Program Files\7-Zip is used only if it can run here (same architecture).
static void Locate7z(const wstring& userPath, const vector<wstring>& targetRoots, const wstring& runArch)
{
    // Order: /7z:<file or folder>; else the folder of pedrv.exe, the target Windows,
    // then "<system drive>\Program Files\7-Zip". Nothing else is searched.
    vector<wstring> candidates;
    std::set<wstring> checkArch;
    const wchar_t* names[] = { L"7z.exe", L"7za.exe", L"7zr.exe" };
    if (!userPath.empty()) {
        if (IsDir(userPath)) { for (auto n : names) candidates.push_back(Join(userPath, n)); }
        else candidates.push_back(userPath);
    } else {
        for (auto n : names) candidates.push_back(Join(ExeDir(), n));
        for (const wstring& root : targetRoots) {
            if (root.empty()) continue;
            wstring dir = Join(root, L"Program Files\\7-Zip");
            for (auto n : names) { candidates.push_back(Join(dir, n)); checkArch.insert(Upper(candidates.back())); }
        }
        wchar_t win[MAX_PATH] = L"";
        if (GetSystemWindowsDirectoryW(win, MAX_PATH) >= 3) {
            wstring dir = wstring(win, 3) + L"Program Files\\7-Zip";
            for (auto n : names) candidates.push_back(Join(dir, n));
        }
    }
    for (const wstring& c : candidates) {
        if (!IsFile(c)) continue;
        if (checkArch.count(Upper(c)) && ArchName(PeMachine(c)) != runArch) continue;
        g_7z = c;
        g_7zIsFull = Upper(NameOf(c)) != L"7ZR.EXE";
        break;
    }
    if (g_7z.empty() && !userPath.empty()) Out(L"WARNING: no 7-Zip found at %s\n", userPath.c_str());
    g_tar = SearchExe(L"tar.exe");
}

static bool MountWim(Source& s)
{
    HMODULE lib = LoadLibraryW(L"wimgapi.dll");
    if (!lib) return false;
    auto mount = (PFN_WIMMountImage)GetProcAddress(lib, "WIMMountImage");
    g_wimUnmount = (PFN_WIMUnmountImage)GetProcAddress(lib, "WIMUnmountImage");
    if (!mount || !g_wimUnmount) return false;
    if (!mount(s.root.c_str(), s.file.c_str(), s.wimIndex, nullptr)) {
        Out(L"  WIM mount failed: %s\n", ErrText(GetLastError()).c_str());
        return false;
    }
    s.mounted = true;
    return true;
}

// Makes the INF files of a source available under s.root.
static bool PrepareSource(Source& s, int index, bool everything)
{
    if (s.kind == SRC_DIR) { s.root = s.file; return true; }

    wchar_t sub[32];
    swprintf(sub, 32, L"s%d", index);
    s.root = Join(g_runDir, sub);
    if (!MakeDirs(s.root)) {
        Out(L"  cannot create %s: %s\n", s.root.c_str(), ErrText(GetLastError()).c_str());
        return false;
    }

    if (s.kind == SRC_WIM) {
        if (MountWim(s)) return true;
        if (g_7z.empty() || !g_7zIsFull) return false;
        Out(L"  falling back to 7-Zip extraction\n");
    }

    bool zip = ExtOf(s.file) == L".ZIP";
    if (!g_7z.empty() && (g_7zIsFull || ExtOf(s.file) == L".7Z")) {
        wstring cmd = Q(g_7z) + L" x " + Q(s.file) + L" -o" + Q(s.root) + L" -y";
        if (!everything) { cmd += L" -r *.inf"; s.partial = true; }
        int rc = Run(cmd);
        if (rc > 1) { Out(L"  7-Zip failed (exit code %d)\n", rc); return false; }   // 1 = warnings
        return true;
    }
    if (zip && !g_tar.empty()) {
        int rc = Run(Q(g_tar) + L" -xf " + Q(s.file) + L" -C " + Q(s.root));
        if (rc != 0) { Out(L"  tar failed (exit code %d)\n", rc); return false; }
        return true;
    }
    Out(L"  no extractor available. Put 7z.exe + 7z.dll (or 7za.exe) next to pedrv.exe or use /7z:<path>\n");
    return false;
}

// Second pass for archives: extract the complete folders of the selected INFs.
static bool ExtractSelected(Source& s, int index, const vector<Inf*>& todo)
{
    std::set<wstring> dirs;
    bool all = false;
    for (const Inf* inf : todo) {
        if (inf->source != index) continue;
        wstring dir = DirOf(inf->path);
        if (dir.size() <= s.root.size()) { all = true; break; }     // INF at archive root
        dirs.insert(RelPath(s.root, dir));
    }
    if (!all && dirs.empty()) return true;

    wstring cmd = Q(g_7z) + L" x " + Q(s.file) + L" -o" + Q(s.root) + L" -y";
    if (!all) {
        wstring listFile = s.root + L".lst";
        std::string utf8;
        for (const wstring& d : dirs) {
            int len = WideCharToMultiByte(CP_UTF8, 0, d.c_str(), (int)d.size(), nullptr, 0, nullptr, nullptr);
            size_t at = utf8.size();
            utf8.resize(at + len);
            WideCharToMultiByte(CP_UTF8, 0, d.c_str(), (int)d.size(), &utf8[at], len, nullptr, nullptr);
            utf8 += "\r\n";
        }
        HANDLE h = CreateFileW(listFile.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        DWORD written;
        WriteFile(h, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
        CloseHandle(h);
        cmd += L" -scsUTF-8 @" + Q(listFile);
    }
    int rc = Run(cmd);
    if (rc > 1) { Out(L"  7-Zip failed (exit code %d)\n", rc); return false; }
    if (all) s.partial = false;
    return true;
}

// ---------------------------------------------------------------- installer ---

typedef UINT DismSession;
#pragma pack(push, 1)
struct DismString { PCWSTR Value; };
#pragma pack(pop)

struct Installer {
    HMODULE lib = nullptr;
    DismSession session = 0;
    bool api = false;
    bool live = false, pnputil = false;
    wstring image, scratch, log, dismExe, liveExe;

    HRESULT(WINAPI* pInitialize)(int, PCWSTR, PCWSTR) = nullptr;
    HRESULT(WINAPI* pOpenSession)(PCWSTR, PCWSTR, PCWSTR, DismSession*) = nullptr;
    HRESULT(WINAPI* pAddDriver)(DismSession, PCWSTR, BOOL) = nullptr;
    HRESULT(WINAPI* pCloseSession)(DismSession) = nullptr;
    HRESULT(WINAPI* pShutdown)() = nullptr;
    HRESULT(WINAPI* pGetLastErrorMessage)(DismString**) = nullptr;
    HRESULT(WINAPI* pDelete)(const void*) = nullptr;

    wstring LastError(HRESULT hr)
    {
        wstring text = ErrText((DWORD)hr);
        DismString* msg = nullptr;
        if (pGetLastErrorMessage && SUCCEEDED(pGetLastErrorMessage(&msg)) && msg) {
            if (msg->Value && *msg->Value) {
                wstring m = msg->Value;
                while (!m.empty() && (m.back() == L'\r' || m.back() == L'\n' || m.back() == L' ')) m.pop_back();
                wchar_t code[16];
                swprintf(code, 16, L"0x%08X ", (DWORD)hr);
                text = code + m;
            }
            if (pDelete) pDelete(msg);
        }
        return text;
    }

    bool Open(bool peMode, const wstring& imagePath, const wstring& scratchDir, const wstring& logFile)
    {
        image = imagePath; scratch = scratchDir; log = logFile;
        if (peMode) {                       // running WinPE: drvload, else pnputil
            live = true;
            liveExe = SearchExe(L"drvload.exe");
            if (liveExe.empty()) { liveExe = SearchExe(L"pnputil.exe"); pnputil = true; }
            if (liveExe.empty()) { Out(L"ERROR: neither drvload.exe nor pnputil.exe found.\n"); return false; }
            // The package is staged in %TEMP% before it goes to the driver store. Point that
            // at the scratch directory so /scratch:<disk folder> keeps it off the RAM disk.
            SetEnvironmentVariableW(L"TEMP", scratch.c_str());
            SetEnvironmentVariableW(L"TMP", scratch.c_str());
            return true;
        }
        lib = LoadLibraryW(L"dismapi.dll");
        if (lib) {
#define BIND(member, name) member = (decltype(member))GetProcAddress(lib, name)
            BIND(pInitialize, "DismInitialize");
            BIND(pOpenSession, "DismOpenSession");
            BIND(pAddDriver, "DismAddDriver");
            BIND(pCloseSession, "DismCloseSession");
            BIND(pShutdown, "DismShutdown");
            BIND(pGetLastErrorMessage, "DismGetLastErrorMessage");
            BIND(pDelete, "DismDelete");
#undef BIND
            if (pInitialize && pOpenSession && pAddDriver && pCloseSession && pShutdown) {
                HRESULT hr = pInitialize(1 /*errors+warnings*/, log.empty() ? nullptr : log.c_str(),
                                         scratch.empty() ? nullptr : scratch.c_str());
                if (SUCCEEDED(hr)) {
                    hr = pOpenSession(image.c_str(), nullptr, nullptr, &session);
                    if (SUCCEEDED(hr)) { api = true; return true; }
                    Out(L"DISM API: cannot open image: %s\n", LastError(hr).c_str());
                    pShutdown();
                } else {
                    Out(L"DISM API: initialization failed: %s\n", ErrText((DWORD)hr).c_str());
                }
            }
        }
        dismExe = SearchExe(L"dism.exe");
        if (dismExe.empty()) { Out(L"ERROR: neither dismapi.dll nor dism.exe is usable.\n"); return false; }
        Out(L"Using dism.exe (DISM API not available).\n");
        return true;
    }

    // What setupapi logged for this INF since offset 'from': the log section of the
    // install, or everything new if the section cannot be told apart.
    static std::string SetupLogSection(const wstring& setupLog, ULONGLONG from, const wstring& inf)
    {
        HANDLE h = CreateFileW(setupLog.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) return std::string();
        LARGE_INTEGER size = {}, pos;
        GetFileSizeEx(h, &size);
        if (from > (ULONGLONG)size.QuadPart) from = 0;              // log was replaced
        const ULONGLONG maxRead = 8 << 20;
        if ((ULONGLONG)size.QuadPart - from > maxRead) from = (ULONGLONG)size.QuadPart - maxRead;
        pos.QuadPart = (LONGLONG)from;
        SetFilePointerEx(h, pos, nullptr, FILE_BEGIN);
        std::string text((size_t)((ULONGLONG)size.QuadPart - from), '\0');
        DWORD got = 0;
        if (!text.empty() && !ReadFile(h, &text[0], (DWORD)text.size(), &got, nullptr)) got = 0;
        CloseHandle(h);
        text.resize(got);

        // ">>>  [Device Install (DiInstallDriver) - <path>\name.inf]" ... "<<<  [Exit status: ...]"
        wstring name = NameOf(inf);
        std::string key = "\\";
        for (wchar_t c : name) key += (char)c;
        key += "]";
        size_t start = std::string::npos;
        for (size_t at = text.find(">>>  ["); at != std::string::npos; at = text.find(">>>  [", at + 1)) {
            size_t eol = text.find('\n', at);
            std::string head = text.substr(at, eol == std::string::npos ? std::string::npos : eol - at);
            if (head.size() >= key.size() && std::search(head.begin(), head.end(), key.begin(), key.end(),
                    [](char x, char y) { return tolower((unsigned char)x) == tolower((unsigned char)y); }) != head.end())
                start = at;
        }
        if (start == std::string::npos) return text;
        size_t end = text.find("<<<  [Exit status", start);
        if (end != std::string::npos) end = text.find('\n', end);
        return text.substr(start, end == std::string::npos ? std::string::npos : end - start);
    }

    // Why a live install failed: free space where the package is copied to, and the
    // errors ("!!!" lines) of its setupapi log section.
    wstring LiveFailureDetail(const wstring& inf, const std::string& text)
    {
        // The package is staged in %TEMP% first, then copied to the driver store.
        wstring detail;
        wchar_t win[MAX_PATH] = L"", tmp[MAX_PATH] = L"";
        GetWindowsDirectoryW(win, MAX_PATH);
        GetTempPathW(MAX_PATH, tmp);
        ULONGLONG need = TreeSize(DirOf(inf));
        const wchar_t* places[] = { tmp, win };
        for (const wchar_t* place : places) {
            ULARGE_INTEGER avail = {};
            if (place == win && towupper(win[0]) == towupper(tmp[0])) continue;
            if (!GetDiskFreeSpaceExW(place, &avail, nullptr, nullptr) || need <= avail.QuadPart / 2) continue;
            wchar_t line[200];
            swprintf(line, 200, L"\n      driver folder is %llu MB, %llu MB free on %.2s (%s)",
                     need >> 20, avail.QuadPart >> 20, place, place == tmp ? L"staging, see /scratch" : L"driver store");
            detail += line;
        }

        int shown = 0;
        for (size_t at = 0; at < text.size() && shown < 6; ) {
            size_t end = text.find('\n', at);
            if (end == std::string::npos) end = text.size();
            std::string line = text.substr(at, end - at);
            at = end + 1;
            if (line.compare(0, 3, "!!!")) continue;
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            // "!!!  sto:        message" -> "sto: message"
            std::string s;
            bool space = true;
            for (size_t i = 3; i < line.size(); i++) {
                bool sp = line[i] == ' ' || line[i] == '\t';
                if (sp && space) continue;
                s += sp ? ' ' : line[i];
                space = sp;
            }
            int len = MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.size(), nullptr, 0);
            wstring w((size_t)len, L'\0');
            MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.size(), &w[0], len);
            detail += L"\n      " + w;
            shown++;
        }
        return detail;
    }

    // Returns true if the driver was added. 'error' is the reason if not; on success it
    // may hold a remark to print after "OK".
    bool Add(const wstring& inf, bool forceUnsigned, wstring& error)
    {
        if (live) {
            wchar_t win[MAX_PATH] = L"";
            GetWindowsDirectoryW(win, MAX_PATH);
            wstring setupLog = Join(win, L"INF\\setupapi.dev.log");
            ULONGLONG logSize = FileSize(setupLog);
            int rc = Run(pnputil ? Q(liveExe) + L" /add-driver " + Q(inf) + L" /install" : Q(liveExe) + L" " + Q(inf),
                         kInstallTimeout);
            // pnputil: 3010 = reboot required, 259 = already up to date on all devices
            if (rc == 0 || (pnputil && (rc == 3010 || rc == 259))) return true;
            if (rc == -1) { error = L"cannot start " + NameOf(liveExe); return false; }
            if (rc == -2) { error = NameOf(liveExe) + L" did not finish within 30 minutes and was stopped"; return false; }

            // The exit code of drvload is not a Win32 error (it is 3 for a full disk and
            // for a device that needs a restart alike): the setupapi log has the outcome.
            std::string text = SetupLogSection(setupLog, logSize, inf);
            size_t st = text.rfind("[Exit status: ");
            if (st != std::string::npos && !text.compare(st + 14, 7, "SUCCESS")) {
                if (text.find("Reboot needed") != std::string::npos || text.find("required reboot") != std::string::npos)
                    error = L", but the device is in use: its driver is replaced only after a restart";
                return true;
            }
            DWORD code = 0;
            if (st != std::string::npos && !text.compare(st + 14, 8, "FAILURE("))
                code = strtoul(text.c_str() + st + 22, nullptr, 16);
            wchar_t exitCode[64];
            swprintf(exitCode, 64, L"%s exit code %d", NameOf(liveExe).c_str(), rc);
            error = (code ? ErrText(code) : wstring(exitCode)) + LiveFailureDetail(inf, text);
            return false;
        }
        if (api) {
            HRESULT hr = pAddDriver(session, inf.c_str(), forceUnsigned);
            if (SUCCEEDED(hr)) return true;
            error = LastError(hr);
            return false;
        }
        wstring cmd = Q(dismExe) + L" /English /Image:" + Q(image) + L" /Add-Driver /Driver:" + Q(inf);
        if (forceUnsigned) cmd += L" /ForceUnsigned";
        if (!scratch.empty()) cmd += L" /ScratchDir:" + Q(scratch);
        if (!log.empty()) cmd += L" /LogPath:" + Q(log);
        wstring outFile = Join(scratch.empty() ? ExeDir() : scratch, L"dism.out");
        int rc = Run(cmd, kInstallTimeout, outFile);
        if (rc == 0) { DeleteFileW(LongPath(outFile).c_str()); return true; }
        if (rc == -1) error = L"cannot start dism.exe";
        else if (rc == -2) error = L"dism.exe did not finish within 30 minutes and was stopped";
        else error = ErrText((DWORD)rc) + DismOutputDetail(outFile);
        DeleteFileW(LongPath(outFile).c_str());
        return false;
    }

    // The error lines dism.exe printed: from its "Error:" line on, else its last lines.
    static wstring DismOutputDetail(const wstring& outFile)
    {
        std::string raw;
        if (!ReadWholeFile(outFile, raw) || raw.empty()) return wstring();
        wstring text;
        if (raw.size() >= 2 && (unsigned char)raw[0] == 0xFF && (unsigned char)raw[1] == 0xFE) {
            text.assign((const wchar_t*)(raw.data() + 2), (raw.size() - 2) / 2);
        } else {
            int len = MultiByteToWideChar(CP_OEMCP, 0, raw.data(), (int)raw.size(), nullptr, 0);
            text.assign((size_t)len, L'\0');
            MultiByteToWideChar(CP_OEMCP, 0, raw.data(), (int)raw.size(), &text[0], len);
        }
        vector<wstring> lines;
        for (size_t at = 0; at < text.size(); ) {
            size_t end = text.find(L'\n', at);
            if (end == wstring::npos) end = text.size();
            wstring line = text.substr(at, end - at);
            at = end + 1;
            while (!line.empty() && (line.back() == L'\r' || line.back() == L' ')) line.pop_back();
            size_t lead = line.find_first_not_of(L' ');
            if (lead == wstring::npos || line.find(L"DISM log file") != wstring::npos) continue;
            lines.push_back(line.substr(lead));
        }
        size_t first = lines.size() > 3 ? lines.size() - 3 : 0;
        for (size_t i = 0; i < lines.size(); i++)
            if (!lines[i].compare(0, 6, L"Error:")) { first = i; break; }
        wstring detail;
        for (size_t i = first; i < lines.size() && i < first + 6; i++) detail += L"\n      " + lines[i];
        return detail;
    }

    void Close()
    {
        if (api) { pCloseSession(session); pShutdown(); api = false; }
    }
};

// ------------------------------------------------------------------- main ---

static void Usage()
{
    Out(PEDRV_BANNER L"\n"
        L"Installs the drivers for this machine into an offline Windows (for WinPE)\n"
        L"\n"
        L"Usage: pedrv [options] <source> [<source> ...]\n"
        L"\n"
        L"  <source>           folder with INF files (searched recursively),\n"
        L"                     or a .zip / .7z / .wim file (use file.wim:N for image index N).\n"
        L"                     Archives directly inside a source folder are used as well.\n"
        L"                     ?:\\<path> uses the first drive (C: to Z:) that has <path>.\n"
        L"                     Default: the \"Drivers\" folder next to pedrv.exe\n"
        L"\n"
        L"  /image:<path>      root of the offline Windows (e.g. C:\\). Auto-detected if omitted.\n"
        L"  /pe                install into the running WinPE instead (drvload), repeating\n"
        L"                     until no newly exposed device finds a driver\n"
        L"  /fromwin[:<path>]  like /pe, with the driver store of the offline Windows as a source\n"
        L"                     (auto-detected if no path). Third-party drivers only; add /inbox\n"
        L"                     to include the drivers that ship with Windows. No <source> needed.\n"
        L"  /list              only show the matching drivers, install nothing\n"
        L"  /all               install every driver in the sources, skip hardware matching\n"
        L"  /allmatches        install every matching driver. Default: only the best matching\n"
        L"                     driver per device (ID rank, then date/version)\n"
        L"  /hwonly            match hardware IDs only, ignore compatible IDs\n"
        L"  /exclude:<classes> skip these device classes, e.g. /exclude:Display,Media\n"
        L"  /slim              WinPE: always load slim copies of the driver packages (WSL, 32-bit,\n"
        L"                     and for NVIDIA/AMD the compute/upscaler/codec files left empty). Without it\n"
        L"                     this is done only for a package that does not fit the RAM disk\n"
        L"  /export:<dir>      install nothing; write slim copies of the selected packages to <dir>\n"
        L"                     (load them later with drvload or /pe). /keep32 keeps the 32-bit files\n"
        L"  /forceunsigned     allow unsigned drivers (DISM /ForceUnsigned)\n"
        L"  /scratch:<dir>     working directory. Default: <image>\\$PeDrv; /pe: <boot drive>\\Temp\\PeDrv\n"
        L"                     (the drive WinPE booted from); else, or if not writable, %%TEMP%%\n"
        L"  /7z:<path>         7z.exe / 7za.exe / 7zr.exe, or the folder containing it. Default:\n"
        L"                     folder of pedrv.exe, then Program Files\\7-Zip of the target\n"
        L"                     Windows (/image, /fromwin), then of the system drive\n"
        L"  /log:<file>        DISM log file\n"
        L"  /version           show the version\n"
        L"\n"
        L"Exit codes: 0 ok, 1 some drivers failed, 2 usage/setup error, 3 interrupted\n"
        L"\n"
        L"Copyright (c) 2026 Zerojinny. Released under the MIT License.\n");
}

static bool IsArchiveExt(const wstring& ext) { return ext == L".ZIP" || ext == L".7Z" || ext == L".WIM"; }

// "?:\path": the path on the first drive (C: to Z:) that has it. The drive letter of the
// boot media differs from machine to machine, so a PE startup script cannot name it.
// Returns the spec unchanged if it does not start with "?:" or no drive has the path.
static wstring ResolveAnyDrive(const wstring& spec)
{
    if (spec.size() < 3 || spec[0] != L'?' || spec[1] != L':') return spec;
    wstring rest = spec.substr(2), bare = rest;
    size_t colon = rest.rfind(L':');                // file.wim:N
    if (colon != wstring::npos && colon + 1 < rest.size()
        && rest.find_first_not_of(L"0123456789", colon + 1) == wstring::npos)
        bare = rest.substr(0, colon);
    UINT oldMode = SetErrorMode(SEM_FAILCRITICALERRORS);   // no "insert a disk" box for an empty drive
    wstring found = spec;
    for (wchar_t d = L'C'; d <= L'Z'; d++) {
        wchar_t root[] = { d, L':', L'\\', 0 };
        if (GetDriveTypeW(root) == DRIVE_NO_ROOT_DIR) continue;
        wstring p = wstring(root, 2) + bare;
        if (IsFile(p) || IsDir(p)) { found = wstring(root, 2) + rest; break; }
    }
    SetErrorMode(oldMode);
    return found;
}

static bool AddSource(const wstring& spec)
{
    Source s;
    s.spec = spec;
    wstring path = ResolveAnyDrive(spec);
    if (path.size() > 1 && path[0] == L'?' && path[1] == L':') {
        Out(L"ERROR: %s not found on any drive\n", spec.c_str() + 2);
        return false;
    }
    if (path != spec) s.spec = spec + L" -> " + path;
    if (!IsFile(path) && !IsDir(path)) {            // file.wim:N
        size_t colon = path.rfind(L':');
        if (colon != wstring::npos && colon > 2 && colon + 1 < path.size()
            && path.find_first_not_of(L"0123456789", colon + 1) == wstring::npos) {
            s.wimIndex = wcstoul(path.c_str() + colon + 1, nullptr, 10);
            path.resize(colon);
        }
    }
    s.file = FullPath(path);
    if (IsDir(s.file)) {
        s.kind = SRC_DIR;
        g_sources.push_back(s);
        WIN32_FIND_DATAW fd;                        // archives directly inside the folder
        HANDLE h = FindFirstFileW(LongPath(Join(s.file, L"*")).c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                wstring ext = ExtOf(fd.cFileName);
                if (!IsArchiveExt(ext)) continue;
                Source a;
                a.file = Join(s.file, fd.cFileName);
                a.spec = a.file;
                a.kind = ext == L".WIM" ? SRC_WIM : SRC_ARCHIVE;
                g_sources.push_back(a);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        return true;
    }
    if (IsFile(s.file)) {
        wstring ext = ExtOf(s.file);
        if (!IsArchiveExt(ext)) { Out(L"ERROR: unsupported source type: %s\n", spec.c_str()); return false; }
        s.kind = ext == L".WIM" ? SRC_WIM : SRC_ARCHIVE;
        g_sources.push_back(s);
        return true;
    }
    Out(L"ERROR: source not found: %s\n", spec.c_str());
    return false;
}

// Finds offline Windows installations on fixed drives (not the running one).
static vector<wstring> DetectImages()
{
    vector<wstring> found;
    wchar_t sys[MAX_PATH] = L"";
    GetSystemWindowsDirectoryW(sys, MAX_PATH);
    wchar_t running = (wchar_t)towupper(sys[0]);
    for (wchar_t d = L'C'; d <= L'Z'; d++) {
        if (d == running) continue;
        wchar_t root[] = { d, L':', L'\\', 0 };
        if (GetDriveTypeW(root) != DRIVE_FIXED) continue;
        wstring r = root;
        if (IsFile(r + L"Windows\\System32\\config\\SYSTEM") && IsFile(r + L"Windows\\System32\\ntoskrnl.exe"))
            found.push_back(r);
    }
    return found;
}

static bool OptValue(const wstring& arg, const wchar_t* name, wstring& value)
{
    size_t n = wcslen(name);
    if (_wcsnicmp(arg.c_str() + 1, name, n) != 0 || arg.size() < n + 2 || arg[n + 1] != L':') return false;
    value = arg.substr(n + 2);
    return true;
}

int wmain(int argc, wchar_t** argv)
{
    bool list = false, all = false, best = true, hwOnly = false, forceUnsigned = false, pe = false;
    bool forceSlim = false, keep32 = false;
    wstring image, scratch, sevenZip, logFile, exclude, fromWinPath, exportDir;
    bool fromWin = false, inbox = false;
    vector<wstring> specs;

    for (int i = 1; i < argc; i++) {
        wstring a = argv[i];
        if (a[0] == L'/' || a[0] == L'-') {
            const wchar_t* o = a.c_str() + 1;
            if (!_wcsicmp(o, L"?") || !_wcsicmp(o, L"h") || !_wcsicmp(o, L"help")) { Usage(); return 0; }
            else if (!_wcsicmp(o, L"version")) { Out(L"%s\n", PEDRV_BANNER); return 0; }
            else if (!_wcsicmp(o, L"list")) list = true;
            else if (!_wcsicmp(o, L"all")) all = true;
            else if (!_wcsicmp(o, L"pe")) pe = true;
            else if (!_wcsicmp(o, L"fromwin")) fromWin = true;
            else if (OptValue(a, L"fromwin", fromWinPath)) fromWin = true;
            else if (!_wcsicmp(o, L"inbox")) inbox = true;
            else if (!_wcsicmp(o, L"best")) best = true;            // default now, kept for old command lines
            else if (!_wcsicmp(o, L"allmatches")) best = false;
            else if (!_wcsicmp(o, L"hwonly")) hwOnly = true;
            else if (!_wcsicmp(o, L"forceunsigned")) forceUnsigned = true;
            else if (!_wcsicmp(o, L"slim")) forceSlim = true;
            else if (!_wcsicmp(o, L"keep32")) keep32 = true;
            else if (OptValue(a, L"export", exportDir)) {}
            else if (OptValue(a, L"image", image) || OptValue(a, L"scratch", scratch)
                  || OptValue(a, L"7z", sevenZip) || OptValue(a, L"log", logFile)
                  || OptValue(a, L"exclude", exclude)) {}
            else { Out(L"ERROR: unknown option %s\n\n", a.c_str()); Usage(); return 2; }
        } else {
            specs.push_back(a);
        }
    }

    // ---- /export: write the selected packages to a folder instead of installing ----
    const bool exporting = !exportDir.empty() && !list;
    if (!exportDir.empty()) {
        if (!image.empty()) { Out(L"ERROR: /export and /image cannot be combined.\n"); return 2; }
        exportDir = FullPath(exportDir);
    }

    // ---- /fromwin: the driver store of the offline Windows is the source ----
    if (fromWin) {
        if (!image.empty()) { Out(L"ERROR: /fromwin and /image cannot be combined.\n"); return 2; }
        pe = true;
        if (fromWinPath.empty()) {
            vector<wstring> found = DetectImages();
            if (found.empty()) {
                Out(L"ERROR: no offline Windows installation found. Use /fromwin:<path>\n");
                return 2;
            }
            if (found.size() > 1) {
                Out(L"Several Windows installations found, choose one with /fromwin:<path>\n");
                for (const wstring& f : found) Out(L"  %s\n", f.c_str());
                return 2;
            }
            fromWinPath = found[0];
        }
        fromWinPath = FullPath(fromWinPath);
        if (fromWinPath.size() == 2 && fromWinPath[1] == L':') fromWinPath += L'\\';
        wstring store = Join(fromWinPath, L"Windows\\System32\\DriverStore\\FileRepository");
        if (!IsDir(store)) {
            Out(L"ERROR: no Windows driver store found at %s\n", fromWinPath.c_str());
            return 2;
        }
        Source s;
        s.spec = store;
        s.file = store;
        s.kind = SRC_DIR;
        if (!inbox) s.inboxInfDir = Join(fromWinPath, L"Windows\\INF");
        g_sources.push_back(s);
    }

    // ---- options that do nothing in this mode ----
    Out(L"%s\n", PEDRV_BANNER);
    auto noEffect = [](const wchar_t* opt, const wchar_t* why) { Out(L"WARNING: %s has no effect %s\n", opt, why); };
    if (inbox && !fromWin) noEffect(L"/inbox", L"without /fromwin");
    if (exporting) {
        if (forceUnsigned) noEffect(L"/forceunsigned", L"with /export");
        if (!logFile.empty()) noEffect(L"/log", L"with /export");
        if (forceSlim) noEffect(L"/slim", L"with /export (exported copies are always slim)");
    } else if (pe) {
        if (forceUnsigned) noEffect(L"/forceunsigned", L"when loading into WinPE");
        if (!logFile.empty()) noEffect(L"/log", L"when loading into WinPE");
    } else {
        if (forceSlim) noEffect(L"/slim", L"without /pe or /fromwin");
        if (keep32) noEffect(L"/keep32", L"without /pe, /fromwin or /export");
    }
    if (all) {
        if (hwOnly) noEffect(L"/hwonly", L"with /all");
        if (!best) noEffect(L"/allmatches", L"with /all");
        if (fromWin && !list && !exporting)
            Out(L"WARNING: /fromwin /all loads EVERY %sdriver of that Windows into WinPE, not only the ones\n"
                L"         for this machine. Use /fromwin /list /all to see them first.\n",
                inbox ? L"" : L"third-party ");
    }

    if (specs.empty() && !fromWin) {
        wstring def = Join(ExeDir(), L"Drivers");
        if (!IsDir(def)) { Usage(); return 2; }
        specs.push_back(def);
    }
    for (const wstring& s : specs) if (!AddSource(s)) return 2;

    // ---- excluded device classes ----
    std::set<wstring> excluded;
    if (_wcsicmp(exclude.c_str(), L"none") != 0) {
        size_t start = 0;
        while (start <= exclude.size()) {
            size_t end = exclude.find_first_of(L",;", start);
            if (end == wstring::npos) end = exclude.size();
            if (end > start) excluded.insert(Upper(exclude.substr(start, end - start)));
            start = end + 1;
        }
    }

    if (pe && !exporting && IsWinPE() && DisplayDriversBlocked() && !excluded.count(L"DISPLAY")) {
        excluded.insert(L"DISPLAY");
        Out(L"WARNING: this WinPE blocks display drivers (GraphicsDrivers\\ForceBddFallbackOnly=1).\n"
            L"         Display drivers are skipped. Rebuild the PE with that value set to 0 to use them.\n");
    }

    // ---- target image ----
    if (exporting) {
        // no target: the drivers are matched against this machine and written to a folder
    } else if (pe) {
        if (!image.empty()) { Out(L"ERROR: /pe and /image cannot be combined.\n"); return 2; }
        if (!list && !IsWinPE()) {
            Out(L"ERROR: /pe only installs into a running WinPE; this is a full Windows. Use /pe /list to preview.\n");
            return 2;
        }
    } else if (!image.empty()) {
        image = FullPath(image);
        if (image.size() == 2 && image[1] == L':') image += L'\\';
        if (!IsFile(Join(image, L"Windows\\System32\\config\\SYSTEM"))) {
            Out(L"ERROR: no Windows installation found at %s\n", image.c_str());
            return 2;
        }
    } else {
        vector<wstring> found = DetectImages();
        if (found.size() == 1) image = found[0];
        else if (found.size() > 1) {
            Out(L"Several Windows installations found, choose one with /image:\n");
            for (const wstring& f : found) Out(L"  %s\n", f.c_str());
            if (!list) return 2;
        } else if (!list) {
            Out(L"ERROR: no offline Windows installation found. Use /image:<path>\n");
            return 2;
        }
    }

    Platform plat;
    wchar_t sysDir[MAX_PATH] = L"";
    GetSystemWindowsDirectoryW(sysDir, MAX_PATH);
    if (!ReadPlatform(image.empty() ? wstring(sysDir) : Join(image, L"Windows"), plat)) {
        Out(L"ERROR: cannot determine the architecture of the target Windows.\n");
        return 2;
    }
    Out(L"Target : %s  [%s %u.%u.%u]\n",
        exporting ? (L"export to " + exportDir).c_str()
        : pe ? L"running WinPE" : image.empty() ? L"(none - list only, using running system)" : image.c_str(),
        plat.arch.c_str(), plat.major, plat.minor, plat.build);

    // 32-bit files are of use only where 32-bit code runs: an x86 target, or a WinPE with WoW64.
    // An export has no known target; /keep32 keeps them.
    if (plat.arch == L"X86" || (!exporting && IsFile(Join(sysDir, L"SysWOW64\\ntdll.dll")))) keep32 = true;

    // ---- WinPE with DWM, Windows 11 24H2+: dwm.exe loads DDisplay.dll (through DispBroker.dll)
    // as soon as a vendor display driver starts. PE builders tend to leave it out; DWM then
    // crashes in a loop, Windows falls back to the Basic Display Driver and the adapter ends
    // up with code 43.
    if (pe && !list && !exporting && IsWinPE() && !excluded.count(L"DISPLAY")) {
        wchar_t sys32[MAX_PATH] = L"";
        GetSystemDirectoryW(sys32, MAX_PATH);
        wstring target = Join(sys32, L"DDisplay.dll");
        if (IsFile(Join(sys32, L"dwm.exe")) && IsFile(Join(sys32, L"DispBroker.dll")) && !IsFile(target)) {
            Platform src;
            wstring from = fromWin ? Join(fromWinPath, L"Windows\\System32\\DDisplay.dll") : wstring();
            if (!from.empty() && IsFile(from) && ReadPlatform(Join(fromWinPath, L"Windows"), src)
                && src.arch == plat.arch && src.build == plat.build && CopyFileW(from.c_str(), target.c_str(), TRUE)) {
                Out(L"Note   : DDisplay.dll was missing in this WinPE (display drivers need it); copied from %s\n",
                    fromWinPath.c_str());
            } else {
                excluded.insert(L"DISPLAY");
                Out(L"WARNING: this WinPE has no System32\\DDisplay.dll; a display driver would make DWM crash\n"
                    L"         (device code 43). Display drivers are skipped. Add the file to the PE build,\n"
                    L"         or copy it from a Windows of the same build before running pedrv.\n");
            }
        }
    }

    // ---- scratch ----
    // Default: on the disk of the offline Windows (also for /fromwin: the driver packages are
    // staged in the scratch directory, and slim copies can hard-link to the driver store there).
    // /pe: on the drive WinPE booted from, so extracted archives and the staging copy of drvload
    // stay off the RAM disk. Else, or if that drive is read-only (CD, ISO), %TEMP%, which in
    // WinPE is a RAM disk.
    vector<wstring> scratchDirs;
    if (!scratch.empty()) scratchDirs.push_back(scratch);
    else {
        if (!image.empty() && !list) scratchDirs.push_back(Join(image, L"$PeDrv"));
        else if (fromWin && !list && !exporting) scratchDirs.push_back(Join(fromWinPath, L"$PeDrv"));
        else if (pe && !list && !exporting) {
            wstring boot = PEBootDrive();
            if (!boot.empty()) scratchDirs.push_back(Join(boot, L"Temp\\PeDrv"));
        }
        wchar_t tmp[MAX_PATH] = L"";
        GetTempPathW(MAX_PATH, tmp);
        scratchDirs.push_back(Join(tmp, L"PeDrv"));
    }
    wchar_t run[32];
    swprintf(run, 32, L"run%u", GetCurrentProcessId());
    vector<wstring> scratchCreated;     // folders made for the scratch directory, deepest first
    wstring runDir;
    for (size_t i = 0; i < scratchDirs.size(); i++) {
        scratch = FullPath(scratchDirs[i]);
        scratchCreated.clear();
        for (wstring p = scratch; p.size() > 3 && !IsDir(p); p = DirOf(p)) scratchCreated.push_back(p);
        runDir = Join(scratch, run);
        if (MakeDirs(runDir)) break;
        DWORD err = GetLastError();
        for (const wstring& d : scratchCreated) RemoveDirectoryW(LongPath(d).c_str());
        if (i + 1 < scratchDirs.size()) continue;       // e.g. a read-only disk: try %TEMP%
        Out(L"ERROR: cannot create scratch directory %s: %s\n", runDir.c_str(), ErrText(err).c_str());
        return 2;
    }
    if (pe && !list && !exporting) Out(L"Scratch: %s\n", scratch.c_str());
    g_runDir = runDir;
    SetConsoleCtrlHandler(CtrlHandler, TRUE);
    {
        Platform running;
        ReadPlatform(sysDir, running);
        Locate7z(sevenZip, { image, fromWin ? fromWinPath : wstring() }, running.arch);
    }

    auto finish = [&](int code) {
        Cleanup();
        for (const wstring& d : scratchCreated) RemoveDirectoryW(LongPath(d).c_str());   // only if empty
        return code;
    };

    // ---- product type: selects the client or server sections of an INF ----
    // WinPE (also the usual target of an export) is a workstation product.
    if (exporting || pe) plat.productType = 1;
    else if (!image.empty()) plat.productType = OfflineProductType(Join(image, L"Windows"), runDir);
    else plat.productType = RunningProductType();
    if (plat.productType == 2 || plat.productType == 3)
        Out(L"Note   : the target is a Windows Server; server sections of the INFs apply\n");
    else if (!plat.productType && !image.empty())
        Out(L"Note   : cannot read the product type of the target; client and server INF sections both apply\n");

    // ---- scan sources ----
    struct InfFile { int source; wstring path; };
    vector<InfFile> files;
    OemInfs oem;
    for (size_t i = 0; i < g_sources.size() && !g_abort; i++) {
        Source& s = g_sources[i];
        Out(L"Source : %s\n", s.spec.c_str());
        if (!PrepareSource(s, (int)i, all)) { Out(L"  skipped\n"); continue; }
        vector<wstring> found;
        FindInfs(s.root, found);
        unsigned inboxSkipped = 0;
        for (const wstring& f : found) {
            if (!s.inboxInfDir.empty() && IsInboxInf(f, s.inboxInfDir, oem)) { inboxSkipped++; continue; }
            files.push_back({ (int)i, f });
        }
        if (s.inboxInfDir.empty()) Out(L"  %u INF files\n", (unsigned)found.size());
        else Out(L"  %u INF files, %u third-party (inbox drivers skipped, /inbox includes them)\n",
                 (unsigned)found.size(), (unsigned)found.size() - inboxSkipped);
    }

    // ---- parse the INFs once; every pass only matches their IDs again ----
    vector<Inf> parsed;
    for (const InfFile& f : files) {
        if (g_abort) break;
        Inf inf;
        inf.source = f.source;
        if (ParseInf(f.path, plat, inf)) parsed.push_back(std::move(inf));
    }
    if (g_abort) { Out(L"Interrupted.\n"); return finish(3); }

    // In WinPE mode a freshly loaded bus/controller driver can expose new
    // devices, so matching is repeated until nothing new turns up.
    Installer inst;
    bool instOpen = false;
    std::set<wstring> installed;
    unsigned ok = 0, failed = 0, notWorking = 0;
    const int maxPasses = (pe && !all && !list && !exporting) ? 5 : 1;

    // Slim copy of a package folder (see "slim" above). Rules come from every selected INF in it.
    auto slimRules = [&](const wstring& dir, const vector<Inf*>& todo) {
        SlimRules r;
        r.keep32 = keep32;
        for (const Inf* inf : todo)
            if (!_wcsicmp(DirOf(inf->path).c_str(), dir.c_str())) AddSlimRules(*inf, r);
        return r;
    };
    auto fits = [](ULONGLONG need, const wstring& where) {
        ULARGE_INTEGER avail = {};
        return !GetDiskFreeSpaceExW(where.c_str(), &avail, nullptr, nullptr) || avail.QuadPart >= need + (64ull << 20);
    };
    std::map<wstring, wstring> slimDirs;    // package folder (upper case) -> its slim copy, empty if not slimmed
    unsigned slimCount = 0;

    for (int pass = 1; pass <= maxPasses && !g_abort; pass++) {
        if (pass > 1) {
            Out(L"\nPass %d: rescanning for devices exposed by the new drivers...\n", pass);
            RescanDevices();
        }
        if (!all) {
            EnumerateDevices(hwOnly);
            Out(L"Devices: %u present, %u distinct IDs\n", (unsigned)g_devices.size(), (unsigned)g_idMap.size());
        }

        vector<Inf*> infs;
        for (Inf& inf : parsed) {
            inf.selected = false;
            if (all) { inf.devRank.clear(); inf.selected = true; }
            else {
                MatchInf(inf);
                if (inf.devRank.empty()) continue;
            }
            if (excluded.count(Upper(inf.cls))) {
                if (pass == 1) Out(L"Skipped: %s (class %s is excluded)\n", NameOf(inf.path).c_str(), inf.cls.c_str());
                inf.selected = false;
                continue;
            }
            infs.push_back(&inf);
        }
        if (pass == 1)
            Out(L"INF    : %u usable for %s, %u %s\n", (unsigned)parsed.size(), plat.arch.c_str(), (unsigned)infs.size(),
                all ? L"selected" : L"matching this machine");

        // ---- select ----
        if (!all) {
            if (best) {
                for (int d = 0; d < (int)g_devices.size(); d++) {
                    Inf* win = nullptr;
                    int winRank = 0;
                    for (Inf* inf : infs) {
                        auto it = inf->devRank.find(d);
                        if (it == inf->devRank.end()) continue;
                        if (!_wcsicmp(inf->cls.c_str(), L"Extension")) { inf->selected = true; continue; }
                        bool better = !win || it->second < winRank
                            || (it->second == winRank && (inf->date > win->date
                                || (inf->date == win->date && inf->version > win->version)));
                        if (better) { win = inf; winRank = it->second; }
                    }
                    if (win) win->selected = true;
                }
            } else {
                for (Inf* inf : infs) inf->selected = true;
            }
        }

        vector<Inf*> todo;
        for (Inf* inf : infs)
            if (inf->selected && !installed.count(Upper(inf->path))) todo.push_back(inf);
        if (todo.empty()) {
            Out(pass == 1 ? L"\nNothing to install.\n" : L"No further drivers needed.\n");
            break;
        }

        Out(L"\n%u driver package(s) selected:\n", (unsigned)todo.size());
        for (Inf* inf : todo) {
            const Source& s = g_sources[inf->source];
            wstring rel = RelPath(s.root, inf->path);
            Out(L"  [%s] %s  (%s, %s)\n", inf->cls.empty() ? L"?" : inf->cls.c_str(), rel.c_str(),
                inf->provider.empty() ? L"?" : inf->provider.c_str(), inf->verText.empty() ? L"no DriverVer" : inf->verText.c_str());
            for (auto& m : inf->devRank) {
                const Device& d = g_devices[m.first];
                if (d.desc == d.firstId) Out(L"      -> %s\n", d.desc.c_str());
                else Out(L"      -> %s  {%s}\n", d.desc.c_str(), d.firstId.c_str());
            }
        }
        if (list) break;

        // ---- complete partially extracted archives ----
        for (size_t i = 0; i < g_sources.size() && !g_abort; i++) {
            Source& s = g_sources[i];
            if (!s.partial) continue;
            bool needed = false;
            for (Inf* inf : todo) if (inf->source == (int)i) needed = true;
            if (!needed) continue;
            Out(L"\nExtracting selected drivers from %s ...\n", NameOf(s.file).c_str());
            if (!ExtractSelected(s, (int)i, todo)) Out(L"  extraction failed; drivers from this source will likely fail\n");
        }

        // ---- export ----
        if (exporting) {
            Out(L"\nExporting to %s\n", exportDir.c_str());
            std::set<wstring> done;
            for (size_t i = 0; i < todo.size() && !g_abort; i++) {
                Inf* inf = todo[i];
                Out(L"  [%u/%u] %s ... ", (unsigned)i + 1, (unsigned)todo.size(), NameOf(inf->path).c_str());
                wstring dir = DirOf(inf->path);
                if (!done.insert(Upper(dir)).second) { ok++; Out(L"OK (same folder as above)\n"); continue; }
                // Named after the package folder; an INF in the root of a drive after itself.
                wstring name = NameOf(dir);
                if (name.empty() || name.back() == L':') name = NameOf(inf->path).substr(0, NameOf(inf->path).rfind(L'.'));
                wstring dst = Join(exportDir, name);
                for (int n = 2; IsDir(dst) || IsFile(dst); n++) {
                    wchar_t suffix[16];
                    swprintf(suffix, 16, L"_%d", n);
                    dst = Join(exportDir, name + suffix);
                }
                SlimStats st;
                if (SlimCopy(dir, dst, L"", slimRules(dir, todo), false, st)) {
                    ok++;
                    if (st.emptied)
                        Out(L"OK (slimmed %llu -> %llu MB, %u files left empty)\n", st.before >> 20, st.after >> 20, st.emptied);
                    else Out(L"OK (%llu MB)\n", st.before >> 20);
                } else {
                    failed++;
                    Out(L"FAILED %s\n", ErrText(GetLastError()).c_str());
                }
            }
            Out(L"\nDone: %u exported, %u failed.\n", ok, failed);
            break;
        }

        // ---- install ----
        if (g_abort) break;
        Out(L"\nInstalling into %s\n", pe ? L"the running WinPE" : image.c_str());
        if (!instOpen) {
            if (!inst.Open(pe, image, runDir, logFile)) return finish(2);
            instOpen = true;
        }
        vector<Inf*> loaded;
        for (size_t i = 0; i < todo.size() && !g_abort; i++) {
            Inf* inf = todo[i];
            Out(L"  [%u/%u] %s ... ", (unsigned)i + 1, (unsigned)todo.size(), NameOf(inf->path).c_str());

            // WinPE: a package that does not fit the RAM disk (or any with /slim) is loaded from a slim copy.
            wstring infPath = inf->path;
            if (pe) {
                wstring dir = DirOf(inf->path);
                auto known = slimDirs.find(Upper(dir));
                if (known == slimDirs.end()) {
                    wstring slim;
                    ULONGLONG need = TreeSize(dir);
                    if (forceSlim || !fits(need, runDir) || !fits(need, sysDir)) {
                        SlimRules rules = slimRules(dir, todo);
                        SlimStats measure, st;
                        SlimCopy(dir, L"", L"", rules, false, measure);
                        if (measure.emptied) {
                            wchar_t sub[32];
                            swprintf(sub, 32, L"slim%u", ++slimCount);
                            slim = Join(Join(runDir, sub), NameOf(dir));
                            if (SlimCopy(dir, slim, L"", rules, true, st)) {
                                Out(L"slimmed %llu -> %llu MB ... ", st.before >> 20, st.after >> 20);
                            } else {
                                Out(L"slim copy failed (%s), using the full package ... ", ErrText(GetLastError()).c_str());
                                DeleteTree(DirOf(slim));
                                slim.clear();
                            }
                        }
                    }
                    known = slimDirs.insert({ Upper(dir), slim }).first;
                }
                if (!known->second.empty()) infPath = Join(known->second, NameOf(inf->path));
            }

            wstring error;
            if (inst.Add(infPath, forceUnsigned, error)) { ok++; Out(L"OK%s\n", error.c_str()); loaded.push_back(inf); }
            else { failed++; Out(L"FAILED %s\n", error.c_str()); }
            installed.insert(Upper(inf->path));
        }

        // ---- WinPE: a loaded driver is no proof the device works (e.g. code 31) ----
        if (pe && !loaded.empty() && !g_abort) {
            Sleep(1000);
            CMP_WaitNoPendingInstallEvents(15000);
            std::set<int> checked;
            for (Inf* inf : loaded) {
                for (auto& m : inf->devRank) {
                    if (!checked.insert(m.first).second) continue;
                    const Device& d = g_devices[m.first];
                    DEVINST node;
                    ULONG status = 0, problem = 0;
                    if (d.instance.empty()
                        || CM_Locate_DevNodeW(&node, (DEVINSTID_W)d.instance.c_str(), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS
                        || CM_Get_DevNode_Status(&status, &problem, node, 0) != CR_SUCCESS)
                        continue;
                    if (!(status & DN_HAS_PROBLEM)) continue;
                    notWorking++;
                    Out(L"  WARNING: device not working (code %u): %s  [%s]\n", problem, d.desc.c_str(), NameOf(inf->path).c_str());
                }
            }
        }
    }
    inst.Close();

    if (instOpen) {
        Out(L"\nDone: %u installed, %u failed.\n", ok, failed);
        if (notWorking) Out(L"%u device(s) still report a problem after loading their driver.\n", notWorking);
    }
    if (g_abort) { Out(L"Interrupted.\n"); return finish(3); }
    return finish(failed || notWorking ? 1 : 0);
}
