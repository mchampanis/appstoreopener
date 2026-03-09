#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <wchar.h>
#include <stdlib.h>
#include "version.h"

#define MAX_RESULTS       16
#define MAX_SEARCH_DEPTH  16
#define PATHBUF_LEN       4096  /* wide chars per path buffer */

typedef struct {
    const WCHAR *exeTerm;
    WCHAR        exeTermLower[MAX_PATH]; /* lowercased exeTerm, computed once */
    size_t       exeTermLen;
    int          matchCount;
    WCHAR       *paths[MAX_RESULTS]; /* heap-alloc'd PATHBUF_LEN WCHARs each */
} SearchCtx;

static void SearchCtxInit(SearchCtx *ctx, const WCHAR *exeTerm) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->exeTerm = exeTerm;
    wcsncpy(ctx->exeTermLower, exeTerm, MAX_PATH - 1);
    ctx->exeTermLower[MAX_PATH - 1] = L'\0';
    CharLowerW(ctx->exeTermLower);
    ctx->exeTermLen = wcslen(ctx->exeTermLower);
}

/*  appstoreopener-raindropio-raindrop.exe  -> pkg="raindropio"  exe="raindrop"
    appstoreopener-spotify-spotify.exe      -> pkg="spotify"     exe="spotify"

    Rule: last segment (after final '-') = exe term
          second-to-last segment         = package term
          other '-' delineated segments  = ignored
*/
static BOOL ParseFilename(const WCHAR *filename, WCHAR *pkgOut, WCHAR *exeOut) {
    WCHAR base[MAX_PATH];
    wcsncpy(base, filename, MAX_PATH - 1);
    base[MAX_PATH - 1] = L'\0';

    WCHAR *dot = wcsrchr(base, L'.');
    if (dot && _wcsicmp(dot, L".exe") == 0) *dot = L'\0';

    /* last segment → exe term */
    WCHAR *lastDash = wcsrchr(base, L'-');
    if (!lastDash) return FALSE;
    wcsncpy(exeOut, lastDash + 1, MAX_PATH - 1);
    exeOut[MAX_PATH - 1] = L'\0';
    if (exeOut[0] == L'\0') return FALSE;

    /* second-to-last segment → package term */
    *lastDash = L'\0'; // lastDash is a pointer to base, write \0 here to make it the new stop for searching
    WCHAR *pkgDash = wcsrchr(base, L'-');
    if (!pkgDash) return FALSE;
    wcsncpy(pkgOut, pkgDash + 1, MAX_PATH - 1);
    pkgOut[MAX_PATH - 1] = L'\0';
    if (pkgOut[0] == L'\0') return FALSE;

    return TRUE;
}

/*
 * Build a FindFirstFileW search pattern with the \\?\ extended-length prefix
 * so the search can traverse paths longer than MAX_PATH. UNC paths and
 * already-extended paths are passed through unchanged.
 */
static void MakeExtendedPattern(const WCHAR *dir, WCHAR *out, int outLen) {
    if (wcsncmp(dir, L"\\\\", 2) == 0) {
        /* UNC (\\server\share) or already \\?\ */
        _snwprintf(out, outLen, L"%s\\*", dir);
    } else if (wcslen(dir) >= 2 && dir[1] == L':') {
        /* drive-letter absolute path → \\?\C:\...\* */
        _snwprintf(out, outLen, L"\\\\?\\%s\\*", dir);
    } else {
        /* relative or other – pass through */
        _snwprintf(out, outLen, L"%s\\*", dir);
    }
    out[outLen - 1] = L'\0';
}

static void SearchDir(const WCHAR *dir, SearchCtx *ctx, int depth) {
    if (ctx->matchCount >= MAX_RESULTS) return;
    if (depth > MAX_SEARCH_DEPTH) return;

    WCHAR pattern[PATHBUF_LEN];
    MakeExtendedPattern(dir, pattern, PATHBUF_LEN);

    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(pattern, &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
            continue;

        /* build full path without \\?\ — ShellExecuteExW does not accept it */
        WCHAR fullPath[PATHBUF_LEN];
        _snwprintf(fullPath, PATHBUF_LEN, L"%s\\%s", dir, fd.cFileName);
        fullPath[PATHBUF_LEN - 1] = L'\0';

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            SearchDir(fullPath, ctx, depth + 1);
        } else {
            WCHAR lower[MAX_PATH];
            wcsncpy(lower, fd.cFileName, MAX_PATH - 1);
            lower[MAX_PATH - 1] = L'\0';
            CharLowerW(lower);

            size_t nameLen = wcslen(lower);
            const WCHAR *ext = wcsrchr(lower, L'.');

            if (nameLen >= ctx->exeTermLen &&
                wcsncmp(lower, ctx->exeTermLower, ctx->exeTermLen) == 0 &&
                ext && wcscmp(ext, L".exe") == 0) {

                if (ctx->matchCount < MAX_RESULTS) {
                    WCHAR *stored = malloc(PATHBUF_LEN * sizeof(WCHAR));
                    if (stored) {
                        wcsncpy(stored, fullPath, PATHBUF_LEN - 1);
                        stored[PATHBUF_LEN - 1] = L'\0';
                        ctx->paths[ctx->matchCount++] = stored;
                    }
                }
            }
        }
    } while (ctx->matchCount < MAX_RESULTS && FindNextFileW(hFind, &fd));

    FindClose(hFind);
}

static int ComparePaths(const void *a, const void *b) {
    return _wcsicmp(*(const WCHAR * const *)a, *(const WCHAR * const *)b);
}

/*
 * Normalize a package name segment: keep only ASCII alphanumerics, lowercase.
 * Matches the same normalization applied to the search term in WinMain.
 */
static void NormalizeName(const WCHAR *in, WCHAR *out, int outLen) {
    int ni = 0;
    for (int i = 0; in[i] && ni < outLen - 1; i++) {
        WCHAR c = in[i];
        if      (c >= L'a' && c <= L'z') out[ni++] = c;
        else if (c >= L'A' && c <= L'Z') out[ni++] = c + (L'a' - L'A');
        else if (c >= L'0' && c <= L'9') out[ni++] = c;
    }
    out[ni] = L'\0';
}

/*
 * Enumerate installed packages via the AppX registry and search each matching
 * package's install directory for executables.
 *
 * Packages are registered under:
 *   HKCU\Software\Classes\Local Settings\Software\Microsoft\Windows\CurrentVersion\AppModel\Repository\Packages
 * Each subkey is a package full name; its PackageRootFolder value is the path.
 *
 * Returns ERROR_SUCCESS on success, or a Win32 error code on failure.
 */
static LONG FindAndSearchMatchingPackages(const WCHAR *pkgTermNorm, SearchCtx *ctx) {
    static const WCHAR *REG_KEY =
        L"Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows"
        L"\\CurrentVersion\\AppModel\\Repository\\Packages";

    HKEY hRoot;
    LONG rc = RegOpenKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, KEY_READ, &hRoot);
    if (rc != ERROR_SUCCESS) return rc;

    size_t termLen = wcslen(pkgTermNorm);

    DWORD idx = 0;
    WCHAR pkgFullName[MAX_PATH];
    DWORD nameLen;

    while (ctx->matchCount < MAX_RESULTS) {
        nameLen = MAX_PATH;
        rc = RegEnumKeyExW(hRoot, idx++, pkgFullName, &nameLen,
                           NULL, NULL, NULL, NULL);
        if (rc == ERROR_NO_MORE_ITEMS) { rc = ERROR_SUCCESS; break; }
        if (rc != ERROR_SUCCESS) break;

        /* package full name: "PublisherName.AppName_1.2.3_x64__publisherid"
           the identity Name is everything before the first '_' */
        WCHAR nameOnly[MAX_PATH];
        wcsncpy(nameOnly, pkgFullName, MAX_PATH - 1);
        nameOnly[MAX_PATH - 1] = L'\0';
        WCHAR *us = wcschr(nameOnly, L'_');
        if (us) *us = L'\0';

        /* normalize and check for substring match */
        WCHAR normalized[MAX_PATH];
        NormalizeName(nameOnly, normalized, MAX_PATH);
        int normLen = (int)wcslen(normalized);

        if (termLen > 0) {
            if (normLen < (int)termLen) continue;
            BOOL found = FALSE;
            for (int j = 0; j <= normLen - (int)termLen; j++) {
                if (wcsncmp(normalized + j, pkgTermNorm, termLen) == 0) {
                    found = TRUE;
                    break;
                }
            }
            if (!found) continue;
        }

        /* open the package subkey and read PackageRootFolder */
        HKEY hPkg;
        if (RegOpenKeyExW(hRoot, pkgFullName, 0, KEY_READ, &hPkg) != ERROR_SUCCESS)
            continue;

        WCHAR rootFolder[PATHBUF_LEN];
        DWORD rootLen = sizeof(rootFolder);
        DWORD type;
        LONG rv = RegQueryValueExW(hPkg, L"PackageRootFolder", NULL,
                                   &type, (LPBYTE)rootFolder, &rootLen);
        RegCloseKey(hPkg);

        if (rv == ERROR_SUCCESS && type == REG_SZ)
            SearchDir(rootFolder, ctx, 0);
    }

    RegCloseKey(hRoot);
    return rc;
}

typedef struct { DWORD pid; HWND hwnd; } FindWndData;

static BOOL CALLBACK FindWindowByPid(HWND hwnd, LPARAM lParam) {
    FindWndData *d = (FindWndData *)lParam;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == d->pid && IsWindowVisible(hwnd)) {
        d->hwnd = hwnd;
        return FALSE; /* stop enumeration */
    }
    return TRUE;
}

/*
 * If exePath is already running, bring its window to the foreground.
 * Returns TRUE if a running instance was found and focused.
 */
static BOOL FindAndFocusRunningInstance(const WCHAR *exePath) {
    const WCHAR *exeName = wcsrchr(exePath, L'\\');
    exeName = exeName ? exeName + 1 : exePath;

    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return FALSE;

    PROCESSENTRY32W pe = {0};
    pe.dwSize = sizeof(pe);
    DWORD targetPid = 0;
    if (Process32FirstW(hSnap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, exeName) == 0) {
                targetPid = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(hSnap, &pe));
    }
    CloseHandle(hSnap);

    if (targetPid == 0) return FALSE;

    FindWndData d = {targetPid, NULL};
    EnumWindows(FindWindowByPid, (LPARAM)&d);
    if (!d.hwnd) return FALSE;

    if (IsIconic(d.hwnd)) ShowWindow(d.hwnd, SW_RESTORE);
    SetForegroundWindow(d.hwnd);
    return TRUE;
}

/* entry point */
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hInstance; (void)hPrevInstance; (void)lpCmdLine; (void)nCmdShow;

    int ret = 0;

    /* parse arguments properly — wcsstr on the raw command line would match
       --dry-run inside the exe path itself */
    BOOL dryRun = FALSE;
    int argc;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        for (int i = 1; i < argc; i++) {
            if (wcscmp(argv[i], L"--dry-run") == 0) {
                dryRun = TRUE;
            } else if (wcscmp(argv[i], L"--version") == 0) {
                MessageBoxW(NULL, L"appstoreopener version: " VERSION, L"appstoreopener", MB_OK | MB_ICONINFORMATION);
                LocalFree(argv);
                return 0;
            }
        }
        LocalFree(argv);
    }

    WCHAR modulePath[PATHBUF_LEN];
    if (!GetModuleFileNameW(NULL, modulePath, PATHBUF_LEN)) {
        MessageBoxW(NULL,
            L"Failed to get module filename.",
            L"appstoreopener", MB_OK | MB_ICONERROR
        );
        return 1;
    }
    WCHAR *filename = wcsrchr(modulePath, L'\\');
    filename = filename ? filename + 1 : modulePath;

    WCHAR pkgTerm[MAX_PATH], exeTerm[MAX_PATH];
    if (!ParseFilename(filename, pkgTerm, exeTerm)) {
        MessageBoxW(NULL,
            L"Rename this exe as: appstoreopener-<package>-<executable>.exe\n\n"
            L"Example: appstoreopener-spotify-spotify.exe",
            L"appstoreopener", MB_OK | MB_ICONERROR
        );
        return 1;
    }

    /*
     * normalize the package term: keep only alphanumeric chars and lowercase them.
     * this lets "raindropio", "raindrop.io", "Raindrop-IO", etc. all match the
     * same package regardless of punctuation in the filename or package name.
     */
    WCHAR pkgTermNorm[MAX_PATH];
    NormalizeName(pkgTerm, pkgTermNorm, MAX_PATH);

    if (pkgTermNorm[0] == L'\0' || wcslen(pkgTermNorm) < 4 || wcslen(exeTerm) < 4) {
        MessageBoxW(NULL,
            L"The package and executable terms must each be at least 4 characters.\n\n"
            L"Short terms match too many packages or executables.\n\n"
            L"Example: appstoreopener-spotify-spotify.exe",
            L"appstoreopener", MB_OK | MB_ICONERROR
        );
        return 1;
    }

    /* collect all matching executables via the native AppModel API, then sort */
    SearchCtx ctx;
    SearchCtxInit(&ctx, exeTerm);

    LONG enumRc = FindAndSearchMatchingPackages(pkgTermNorm, &ctx);
    if (enumRc != ERROR_SUCCESS) {
        /* write diagnostics to a temp file and open it so the text is copyable */
        WCHAR tmpDir[MAX_PATH], logPath[MAX_PATH];
        GetTempPathW(MAX_PATH, tmpDir);
        _snwprintf(logPath, MAX_PATH, L"%sappstoreopener_debug.txt", tmpDir);
        logPath[MAX_PATH - 1] = L'\0';

        HANDLE hLog = CreateFileW(logPath, GENERIC_WRITE, 0, NULL,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hLog != INVALID_HANDLE_VALUE) {
            WCHAR wline[512];
            _snwprintf(wline, 512,
                L"Failed to open the AppX package registry (code: %ld)\r\n\r\n"
                L"Expected key:\r\n"
                L"HKCU\\Software\\Classes\\Local Settings\\Software\\Microsoft"
                L"\\Windows\\CurrentVersion\\AppModel\\Repository\\Packages\r\n",
                enumRc);
            wline[511] = L'\0';
            DWORD written;
            WORD bom = 0xFEFF;
            WriteFile(hLog, &bom, sizeof(bom), &written, NULL);
            WriteFile(hLog, wline, (DWORD)(wcslen(wline) * sizeof(WCHAR)), &written, NULL);
            CloseHandle(hLog);

            ShellExecuteW(NULL, L"open", L"notepad.exe", logPath, NULL, SW_SHOW);
        } else {
            /* fallback to message box if we can't write the file */
            WCHAR msg[512];
            _snwprintf(msg, 512,
                L"Failed to open the AppX package registry (code: %ld).",
                enumRc);
            msg[511] = L'\0';
            MessageBoxW(NULL, msg, L"appstoreopener", MB_OK | MB_ICONWARNING);
        }
        return 1;
    }

    if (ctx.matchCount > 1)
        qsort(ctx.paths, ctx.matchCount, sizeof(WCHAR *), ComparePaths);

    /* dry-run mode: list matches sorted; if none, list all .exe files as a hint */
    if (dryRun) {
        WCHAR header[512];
        const WCHAR *verStr = L"version " VERSION L"\n";
        _snwprintf(header, 512,
            L"appstoreopener %s\n"
            L"Package: *%s* (normalized: *%s*)\nExecutable search: %s*.exe\n\n",
            verStr, pkgTerm, pkgTermNorm, exeTerm
        );
        header[511] = L'\0';

        /* matching results, or (if no match) all .exe files in the package */
        SearchCtx fallback;
        const SearchCtx *display = &ctx;
        if (ctx.matchCount == 0) {
            SearchCtxInit(&fallback, L"");
            FindAndSearchMatchingPackages(pkgTermNorm, &fallback);
            if (fallback.matchCount > 1)
                qsort(fallback.paths, fallback.matchCount, sizeof(WCHAR *), ComparePaths);
            display = &fallback;
        }

        int listWChars = display->matchCount * (PATHBUF_LEN + 8) + 1;
        WCHAR *list = calloc(listWChars, sizeof(WCHAR));
        if (list) {
            for (int i = 0; i < display->matchCount; i++) {
                WCHAR line[PATHBUF_LEN + 8];
                _snwprintf(line, PATHBUF_LEN + 8, L"%d. %s\n", i + 1, display->paths[i]);
                line[PATHBUF_LEN + 7] = L'\0';
                wcsncat(list, line, listWChars - wcslen(list) - 1);
            }
        }

        int msgWChars = 512 + listWChars;
        WCHAR *msg = malloc(msgWChars * sizeof(WCHAR));
        if (msg) {
            if (ctx.matchCount > 0) {
                _snwprintf(msg, msgWChars, L"%s%s", header, list ? list : L"");
            } else if (fallback.matchCount > 0) {
                _snwprintf(msg, msgWChars,
                    L"%sNo match for '%s*.exe'. Available executables in package:\n%s",
                    header, exeTerm, list ? list : L"");
            } else {
                _snwprintf(msg, msgWChars,
                    L"%sNo match for '%s*.exe', and no executables found in package at all.",
                    header, exeTerm);
            }
            msg[msgWChars - 1] = L'\0';
            MessageBoxW(NULL, msg, L"appstoreopener --dry-run", MB_OK | MB_ICONINFORMATION);
            free(msg);
        }
        free(list);
        for (int i = 0; i < fallback.matchCount; i++)
            free(fallback.paths[i]);
    }

    if (!dryRun && ctx.matchCount == 0) {
        WCHAR msg[256];
        _snwprintf(msg, 256,
            L"No executable matching '%s*.exe' found in the package directory.",
            exeTerm
        );
        msg[255] = L'\0';
        MessageBoxW(NULL, msg, L"appstoreopener", MB_OK | MB_ICONWARNING);
        ret = 1;

    } else if (!dryRun) {
        /* if already running, focus its window instead of launching a new instance */
        if (!FindAndFocusRunningInstance(ctx.paths[0])) {
            SHELLEXECUTEINFOW sei = {0};
            sei.cbSize = sizeof(sei);
            sei.lpVerb = L"open";
            sei.lpFile = ctx.paths[0];
            sei.nShow  = SW_SHOW;

            if (!ShellExecuteExW(&sei)) {
                WCHAR msg[PATHBUF_LEN + 64];
                _snwprintf(msg, PATHBUF_LEN + 64,
                    L"Failed to launch:\n%s\n\nError: %lu",
                    ctx.paths[0], GetLastError()
                );
                msg[PATHBUF_LEN + 63] = L'\0';
                MessageBoxW(NULL, msg, L"appstoreopener", MB_OK | MB_ICONERROR);
                ret = 1;
            }
        }
    }

    for (int i = 0; i < ctx.matchCount; i++)
        free(ctx.paths[i]);

    return ret;
} //WinMain
