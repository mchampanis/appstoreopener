#include <windows.h>
#include <shellapi.h>
#include <wchar.h>
#include <stdlib.h>

#define MAX_RESULTS       16
#define MAX_SEARCH_DEPTH  16
#define PATHBUF_LEN       4096                      /* wide chars per path buffer */
#define PS_OUT_WCHARS     (MAX_RESULTS * MAX_PATH)  /* wide chars for PS output */

typedef struct {
    const WCHAR *exeTerm;
    int          matchCount;
    WCHAR       *paths[MAX_RESULTS]; /* heap-alloc'd PATHBUF_LEN WCHARs each */
} SearchCtx;

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

    /* generate lower-case search term once per call */
    WCHAR termLower[MAX_PATH];
    wcsncpy(termLower, ctx->exeTerm, MAX_PATH - 1);
    termLower[MAX_PATH - 1] = L'\0';
    CharLowerW(termLower);
    size_t termLen = wcslen(termLower);

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

            if (nameLen >= termLen &&
                wcsncmp(lower, termLower, termLen) == 0 &&
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
 * run psCmd via PowerShell, capture UTF-8 output, and decode it into outBuf.
 * outWChars is the capacity of outBuf including the NUL terminator.
 */
static BOOL RunPowerShell(const WCHAR *psCmd, WCHAR *outBuf, int outWChars) {
    SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
    HANDLE hRead, hWrite;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) return FALSE;
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    /* force UTF-8 stdout so MultiByteToWideChar can decode it reliably */
    WCHAR cmdLine[4096];
    _snwprintf(cmdLine, 4096,
        L"powershell.exe -NoProfile -NonInteractive -Command "
        L"\"[Console]::OutputEncoding=[System.Text.Encoding]::UTF8; %s\"",
        psCmd
    );
    cmdLine[4095] = L'\0';

    STARTUPINFOW si = {0};
    si.cb          = sizeof(si);
    si.dwFlags     = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput   = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput  = hWrite;
    si.hStdError   = GetStdHandle(STD_ERROR_HANDLE);

    PROCESS_INFORMATION pi = {0};
    BOOL ok = CreateProcessW(
        NULL, cmdLine, NULL, NULL, TRUE,
        CREATE_NO_WINDOW, NULL, NULL, &si, &pi
    );
    CloseHandle(hWrite);

    if (!ok) { CloseHandle(hRead); return FALSE; }

    /* UTF-8 uses at most 4 bytes per Unicode code point */
    int rawCap = outWChars * 4;
    char *raw = malloc(rawCap);
    if (!raw) {
        CloseHandle(hRead);
        if (WaitForSingleObject(pi.hProcess, 30000) == WAIT_TIMEOUT)
            TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return FALSE;
    }

    DWORD total = 0, bytesRead;
    while ((int)total < rawCap - 1) {
        if (!ReadFile(hRead, raw + total, rawCap - 1 - total, &bytesRead, NULL) || bytesRead == 0) break;
        total += bytesRead;
    }

    if (WaitForSingleObject(pi.hProcess, 30000) == WAIT_TIMEOUT)
        TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(hRead);

    if (total == 0) { free(raw); return FALSE; }

    int written = MultiByteToWideChar(CP_UTF8, 0, raw, (int)total, outBuf, outWChars - 1);
    free(raw);
    if (written == 0) return FALSE;
    outBuf[written] = L'\0';
    return TRUE;
}

/* split newline-separated InstallLocation paths and search each */
static void SearchPackagePaths(const WCHAR *psOutput, SearchCtx *ctx) {
    const WCHAR *p = psOutput;
    while (*p && ctx->matchCount < MAX_RESULTS) {
        const WCHAR *lineStart = p;
        while (*p && *p != L'\n') p++;

        int lineLen = (int)(p - lineStart);
        if (lineLen > 0 && lineLen < PATHBUF_LEN) {
            WCHAR line[PATHBUF_LEN];
            wcsncpy(line, lineStart, lineLen);
            line[lineLen] = L'\0';

            /* trim trailing whitespace / CR */
            WCHAR *end = line + lineLen - 1;
            while (end >= line && (*end == L'\r' || *end == L' ' || *end == L'\t'))
                *end-- = L'\0';

            if (line[0] != L'\0')
                SearchDir(line, ctx, 0);
        }

        if (*p == L'\n') p++;
    }
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
            if (wcscmp(argv[i], L"--dry-run") == 0) { dryRun = TRUE; break; }
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
    int ni = 0;
    for (int i = 0; pkgTerm[i] && ni < MAX_PATH - 1; i++) {
        WCHAR c = pkgTerm[i];
        if (c >= L'a' && c <= L'z')      pkgTermNorm[ni++] = c;
        else if (c >= L'A' && c <= L'Z') pkgTermNorm[ni++] = c + (L'a' - L'A');
        else if (c >= L'0' && c <= L'9') pkgTermNorm[ni++] = c;
    }
    pkgTermNorm[ni] = L'\0';

    if (pkgTermNorm[0] == L'\0') {
        MessageBoxW(NULL,
            L"The package term in the filename must contain alphanumeric characters.\n\n"
            L"Example: appstoreopener-abc-def1.exe",
            L"appstoreopener", MB_OK | MB_ICONERROR
        );
        return 1;
    }

    /*
     * query PowerShell: strip non-alphanumeric chars from each package name
     * before comparing, so "Raindrop.io" matches the search term "raindropio".
     * PowerShell's -like operator is case-insensitive.
     */
    WCHAR psCmd[1024];
    _snwprintf(psCmd, 1024,
        L"(Get-AppxPackage | Where-Object { "
        L"($_.Name -replace '[^a-zA-Z0-9]','') -like '*%s*' "
        L"}).InstallLocation",
        pkgTermNorm
    );
    psCmd[1023] = L'\0';

    WCHAR *psOutput = malloc(PS_OUT_WCHARS * sizeof(WCHAR));
    if (!psOutput) {
        MessageBoxW(NULL, L"Out of memory.", L"appstoreopener", MB_OK | MB_ICONERROR);
        return 1;
    }

    if (!RunPowerShell(psCmd, psOutput, PS_OUT_WCHARS)) {
        WCHAR msg[512];
        _snwprintf(msg, 512,
            L"No installed package found matching '%s' (normalized: '%s').\n\n"
            L"Was the app installed from the Microsoft Store (C:\\Program Files\\WindowsApps\\...)?",
            pkgTerm, pkgTermNorm
        );
        msg[511] = L'\0';
        MessageBoxW(NULL, msg, L"appstoreopener", MB_OK | MB_ICONWARNING);
        free(psOutput);
        return 1;
    }

    /* collect all matching executables, then sort alphabetically */
    SearchCtx ctx = {0};
    ctx.exeTerm = exeTerm;
    SearchPackagePaths(psOutput, &ctx);
    free(psOutput);

    if (ctx.matchCount > 1)
        qsort(ctx.paths, ctx.matchCount, sizeof(WCHAR *), ComparePaths);

    /* dry-run mode: list all matches sorted and exit */
    if (dryRun) {
        WCHAR header[512];
        _snwprintf(header, 512,
            L"Package: *%s* (normalized: *%s*)\nExecutable search: %s*.exe\n\n",
            pkgTerm, pkgTermNorm, exeTerm
        );
        header[511] = L'\0';

        int listWChars = ctx.matchCount * (PATHBUF_LEN + 8) + 1;
        WCHAR *list = calloc(listWChars, sizeof(WCHAR));
        if (list) {
            for (int i = 0; i < ctx.matchCount; i++) {
                WCHAR line[PATHBUF_LEN + 8];
                _snwprintf(line, PATHBUF_LEN + 8, L"%d. %s\n", i + 1, ctx.paths[i]);
                line[PATHBUF_LEN + 7] = L'\0';
                wcsncat(list, line, listWChars - wcslen(list) - 1);
            }
        }

        int msgWChars = 512 + listWChars;
        WCHAR *msg = malloc(msgWChars * sizeof(WCHAR));
        if (msg) {
            _snwprintf(msg, msgWChars, L"%s%s", header,
                ctx.matchCount == 0 ? L"(no matching executables found)" : (list ? list : L""));
            msg[msgWChars - 1] = L'\0';
            MessageBoxW(NULL, msg, L"appstoreopener --dry-run", MB_OK | MB_ICONINFORMATION);
            free(msg);
        }
        free(list);

    } else if (ctx.matchCount == 0) {
        WCHAR msg[256];
        _snwprintf(msg, 256,
            L"No executable matching '%s*.exe' found in the package directory.",
            exeTerm
        );
        msg[255] = L'\0';
        MessageBoxW(NULL, msg, L"appstoreopener", MB_OK | MB_ICONWARNING);
        ret = 1;

    } else {
        /* launch the first (alphabetically earliest) match */
        SHELLEXECUTEINFOW sei = {0};
        sei.cbSize = sizeof(sei);
        //sei.fMask  = SEE_MASK_NOCLOSEPROCESS;
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

    for (int i = 0; i < ctx.matchCount; i++)
        free(ctx.paths[i]);

    return ret;
} //WinMain
