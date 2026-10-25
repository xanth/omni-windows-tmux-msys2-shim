/* A native tmux.exe that runs MSYS2's tmux with the argv a native caller gave it.
 *
 * The MSYS2 runtime parses a native caller's command line itself: it globs and
 * brace-expands bare words ("#{pane_pid}" arrives as "#pane_pid"), treats single
 * quotes as quotes and reads "@file" words as files. This shim quotes each word
 * so that parser restores it exactly, and runs MSYS2's tmux.exe on the same
 * standard handles. It also gives tmux the forms it cannot read from Windows:
 *   - "-c X:\dir" and a load-buffer, save-buffer or source-file "X:\path" become
 *     "/x/..." (tmux reads a path without a leading "/" as relative);
 *   - "-f nul" becomes "-f /dev/null";
 *   - a native pane program runs through "tmux.exe --native-exec", which starts
 *     it with its argv unconverted and without the MSYS2 variables tmux adds;
 *   - a control client (-C) on pipes runs in a pty from script(1), because tmux
 *     on Cygwin reaches a client's input and output only through its tty.
 * The tmux binary is %MSYS2_TMUX%, else <root>\usr\bin\tmux.exe for %MSYS2_ROOT%,
 * %USERPROFILE%\msys64 and C:\msys64.
 */
#include <windows.h>
#include <shellapi.h>
#include <stdlib.h>
#include <wchar.h>
#include <wctype.h>

#define NATIVE_EXEC L"--native-exec"
#define CONV_EXCL_MARK L"TMUX_SHIM_SET_ARG_CONV_EXCL"
#define PATH_MARK L"TMUX_SHIM_ADDED_PATH"

static int is_drive_path(const wchar_t *s)
{
    return iswalpha(s[0]) && s[1] == L':' && (s[2] == 0 || s[2] == L'\\' || s[2] == L'/');
}

static int is_absolute_windows_path(const wchar_t *s)
{
    return is_drive_path(s) || (s[0] == L'\\' && s[1] == L'\\');
}

static int is_file(const wchar_t *path)
{
    DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

/* "C:\a\b" -> "/c/a/b", "\\host\share" -> "//host/share". */
static wchar_t *posix_path(const wchar_t *s)
{
    wchar_t *out = calloc(wcslen(s) + 2, sizeof(wchar_t)), *p = out;
    if (is_drive_path(s)) {
        *p++ = L'/';
        *p++ = (wchar_t)towlower(s[0]);
        s += 2;
    }
    for (; *s; s++)
        *p++ = *s == L'\\' ? L'/' : *s;
    return out;
}

static void append(wchar_t **buffer, size_t *length, size_t *capacity, const wchar_t *text)
{
    size_t n = wcslen(text);
    if (*length + n + 1 > *capacity) {
        *capacity = (*length + n + 1) * 2;
        *buffer = realloc(*buffer, *capacity * sizeof(wchar_t));
    }
    wcscpy(*buffer + *length, text);
    *length += n;
}

/* For /bin/sh, and for the MSYS2 argv parser with MSYS=noglob: 'it'"'"'s'. */
static wchar_t *sh_quote(const wchar_t *s)
{
    size_t length = 0, capacity = 0;
    wchar_t *out = NULL, one[2] = {0};
    append(&out, &length, &capacity, L"'");
    for (; *s; s++) {
        one[0] = *s;
        append(&out, &length, &capacity, *s == L'\'' ? L"'\"'\"'" : one);
    }
    append(&out, &length, &capacity, L"'");
    return out;
}

/* With globbing on (the default) the parser keeps a double-quoted word literal
 * and reads \" and \\ as escapes; with MSYS=noglob only quotes count. */
static wchar_t *word_quote(const wchar_t *s)
{
    wchar_t value[32767];
    if (GetEnvironmentVariableW(L"MSYS", value, 32767) && wcsstr(value, L"noglob"))
        return sh_quote(s);
    wchar_t *out = calloc(wcslen(s) * 2 + 3, sizeof(wchar_t)), *p = out;
    *p++ = L'"';
    for (; *s; s++) {
        if (*s == L'\\' || *s == L'"')
            *p++ = L'\\';
        *p++ = *s;
    }
    *p++ = L'"';
    return out;
}

/* The MSVCRT quoting that CommandLineToArgvW undoes (Python's list2cmdline). */
static void append_windows_word(wchar_t **line, size_t *length, size_t *capacity, const wchar_t *s)
{
    if (*s && !wcspbrk(s, L" \t\"")) {
        append(line, length, capacity, s);
        return;
    }
    append(line, length, capacity, L"\"");
    for (;; s++) {
        size_t slashes = 0;
        while (*s == L'\\') {
            s++;
            slashes++;
        }
        size_t repeat = *s == 0 ? slashes * 2 : *s == L'"' ? slashes * 2 + 1 : slashes;
        for (size_t k = 0; k < repeat; k++)
            append(line, length, capacity, L"\\");
        if (*s == 0)
            break;
        wchar_t one[2] = {*s, 0};
        append(line, length, capacity, one);
    }
    append(line, length, capacity, L"\"");
}

static wchar_t *msys_usr_bin;

/* A program found in MSYS2's usr\bin is an MSYS2 program, which tmux runs itself. */
static int is_msys_program(const wchar_t *name)
{
    wchar_t found[MAX_PATH];
    return name[0] == L'/' || (SearchPathW(NULL, name, L".exe", MAX_PATH, found, NULL) &&
                               _wcsnicmp(found, msys_usr_bin, wcslen(msys_usr_bin)) == 0);
}

enum rewrite { START_DIR = 1, PANE_COMMAND = 2, PATH_ARGUMENT = 4 };

/* Per tmux command: the flags that take a value, and what to rewrite. */
static const wchar_t *command_flags(const wchar_t *command, int *rewrites)
{
    static const struct {
        const wchar_t *name, *flags;
        int rewrites;
    } table[] = {
        {L"new-session", L"cefFnstxy", START_DIR | PANE_COMMAND},
        {L"new", L"cefFnstxy", START_DIR | PANE_COMMAND},
        {L"new-window", L"ceFnt", START_DIR | PANE_COMMAND},
        {L"neww", L"ceFnt", START_DIR | PANE_COMMAND},
        {L"split-window", L"BceFlmpRsStTxXyY", START_DIR | PANE_COMMAND},
        {L"splitw", L"BceFlmpRsStTxXyY", START_DIR | PANE_COMMAND},
        {L"respawn-pane", L"cet", START_DIR | PANE_COMMAND},
        {L"respawnp", L"cet", START_DIR | PANE_COMMAND},
        {L"respawn-window", L"cet", START_DIR | PANE_COMMAND},
        {L"respawnw", L"cet", START_DIR | PANE_COMMAND},
        {L"attach-session", L"cft", START_DIR},
        {L"attach", L"cft", START_DIR},
        {L"load-buffer", L"bt", PATH_ARGUMENT},
        {L"loadb", L"bt", PATH_ARGUMENT},
        {L"save-buffer", L"b", PATH_ARGUMENT},
        {L"saveb", L"b", PATH_ARGUMENT},
        {L"source-file", L"t", PATH_ARGUMENT},
        {L"source", L"t", PATH_ARGUMENT},
    };
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++)
        if (wcscmp(command, table[i].name) == 0) {
            *rewrites = table[i].rewrites;
            return table[i].flags;
        }
    return NULL;
}

/* Copy one tmux command, args[0..count), to out, rewritten. Returns whether it
 * starts a server with no client: start-server, or new-session -d. */
static int rewrite_command(wchar_t **args, int count, wchar_t **out, int *used, wchar_t *self)
{
    int rewrites = 0, detached = 0;
    const wchar_t *flags = count ? command_flags(args[0], &rewrites) : NULL;
    int i = 0;
    if (count && (wcscmp(args[0], L"start-server") == 0 || wcscmp(args[0], L"start") == 0))
        detached = 1;
    if (flags != NULL) {
        int new_session = wcscmp(args[0], L"new-session") == 0 || wcscmp(args[0], L"new") == 0;
        out[(*used)++] = args[i++];
        for (; i < count && args[i][0] == L'-' && args[i][1]; i++) {
            if (wcscmp(args[i], L"--") == 0) {
                out[(*used)++] = args[i++];
                /* A native program (one argument only when it is a file path, as a
                 * lone argument is otherwise a shell command) runs through this shim. */
                if ((rewrites & PANE_COMMAND) && i < count && !is_msys_program(args[i]) &&
                    (count - i > 1 || (is_absolute_windows_path(args[i]) && is_file(args[i])))) {
                    out[(*used)++] = self;
                    out[(*used)++] = NATIVE_EXEC;
                }
                break;
            }
            wchar_t last = args[i][wcslen(args[i]) - 1];
            out[(*used)++] = args[i];
            detached |= new_session && wcschr(args[i], L'd') != NULL;
            if (wcschr(flags, last) && i + 1 < count) {
                i++;
                int start_dir = last == L'c' && (rewrites & START_DIR);
                out[(*used)++] =
                    start_dir && is_absolute_windows_path(args[i]) ? posix_path(args[i]) : args[i];
            }
        }
        if ((rewrites & PATH_ARGUMENT) && i < count && is_absolute_windows_path(args[i]))
            out[(*used)++] = posix_path(args[i++]);
    }
    for (; i < count; i++)
        out[(*used)++] = args[i];
    return detached;
}

static int find_tmux(wchar_t *out, DWORD size)
{
    wchar_t root[MAX_PATH];
    if (GetEnvironmentVariableW(L"MSYS2_TMUX", out, size) && is_file(out))
        return 1;
    const wchar_t *roots[] = {L"%MSYS2_ROOT%", L"%USERPROFILE%\\msys64", L"C:\\msys64"};
    for (int i = 0; i < 3; i++) {
        if (!ExpandEnvironmentStringsW(roots[i], root, MAX_PATH) || wcschr(root, L'%'))
            continue;
        _snwprintf(out, size, L"%ls\\usr\\bin\\tmux.exe", root);
        if (is_file(out))
            return 1;
    }
    return 0;
}

/* <tmux.exe dir>\<name>, e.g. "...\usr\bin\script.exe". */
static wchar_t *sibling(const wchar_t *tmux, const wchar_t *name)
{
    wchar_t *out = calloc(wcslen(tmux) + wcslen(name) + 2, sizeof(wchar_t));
    wcscpy(out, tmux);
    wcscpy(wcsrchr(out, L'\\') + 1, name);
    return out;
}

/* A command line for the MSYS2 argv parser: "<program>" <quoted word>... */
static wchar_t *command_line(const wchar_t *program, wchar_t **words, int count)
{
    size_t length = 0, capacity = 0;
    wchar_t *line = NULL;
    append(&line, &length, &capacity, L"\"");
    append(&line, &length, &capacity, program);
    append(&line, &length, &capacity, L"\"");
    for (int k = 0; k < count; k++) {
        append(&line, &length, &capacity, L" ");
        append(&line, &length, &capacity, word_quote(words[k]));
    }
    return line;
}

/* script -qfec '<stty> raw -echo && exec <tmux> <args>' /dev/null */
static wchar_t *in_pty(const wchar_t *tmux, wchar_t **args, int count)
{
    size_t length = 0, capacity = 0;
    wchar_t *inner = NULL;
    append(&inner, &length, &capacity, sh_quote(sibling(tmux, L"stty.exe")));
    append(&inner, &length, &capacity, L" raw -echo && exec ");
    append(&inner, &length, &capacity, sh_quote(tmux));
    for (int k = 0; k < count; k++) {
        append(&inner, &length, &capacity, L" ");
        append(&inner, &length, &capacity, sh_quote(args[k]));
    }
    wchar_t *words[] = {L"-qfec", inner, L"/dev/null"};
    return command_line(sibling(tmux, L"script.exe"), words, 3);
}

/* A job that ends its processes when this process ends. */
static HANDLE kill_on_close_job(void)
{
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
    ZeroMemory(&limits, sizeof limits);
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    HANDLE job = CreateJobObjectW(NULL, NULL);
    if (job)
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits);
    return job;
}

static int run(wchar_t *program, wchar_t *line, HANDLE job, int std_handles, DWORD flags)
{
    SetConsoleCtrlHandler(NULL, TRUE);
    STARTUPINFOW startup;
    ZeroMemory(&startup, sizeof startup);
    startup.cb = sizeof startup;
    if (std_handles) {
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    }
    PROCESS_INFORMATION process;
    if (!CreateProcessW(program, line, NULL, NULL, TRUE, flags | (job ? CREATE_SUSPENDED : 0), NULL, NULL,
                        &startup, &process)) {
        fwprintf(stderr, L"tmux shim: cannot start %ls (error %lu)\n", program ? program : line,
                 GetLastError());
        return 127;
    }
    if (job) {
        AssignProcessToJobObject(job, process.hProcess);
        ResumeThread(process.hThread);
    }
    CloseHandle(process.hThread);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    return (int)code;
}

/* tmux.exe --native-exec <program> [args...]: run a pane program as Windows would. */
static int native_exec(wchar_t **argv, int argc, const wchar_t *tmux)
{
    wchar_t value[32767], root[MAX_PATH];
    if (GetEnvironmentVariableW(CONV_EXCL_MARK, value, 32767)) {
        SetEnvironmentVariableW(L"MSYS2_ARG_CONV_EXCL", NULL);
        SetEnvironmentVariableW(CONV_EXCL_MARK, NULL);
    }
    /* tmux sets SHELL to its own default-shell, an MSYS2 program. */
    wcscpy(root, tmux);
    *wcsstr(root, L"\\usr\\bin\\") = 0;
    if (GetEnvironmentVariableW(L"SHELL", value, 32767) && _wcsnicmp(value, root, wcslen(root)) == 0)
        SetEnvironmentVariableW(L"SHELL", NULL);
    wchar_t *tail;
    if (GetEnvironmentVariableW(PATH_MARK, NULL, 0) && GetEnvironmentVariableW(L"PATH", value, 32767) &&
        (tail = wcsrchr(value, L';')) && _wcsnicmp(tail + 1, root, wcslen(root)) == 0) {
        *tail = 0;
        SetEnvironmentVariableW(L"PATH", value);
    }
    SetEnvironmentVariableW(PATH_MARK, NULL);
    size_t length = 0, capacity = 0;
    wchar_t *line = NULL;
    for (int k = 0; k < argc; k++) {
        if (k)
            append(&line, &length, &capacity, L" ");
        append_windows_word(&line, &length, &capacity, argv[k]);
    }
    return run(NULL, line, kill_on_close_job(), 1, 0);
}

int wmain(void)
{
    int argc;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    wchar_t tmux[32767], self[MAX_PATH];
    if (argv == NULL || !find_tmux(tmux, 32767)) {
        fwprintf(stderr, L"tmux shim: MSYS2 tmux.exe not found (set MSYS2_TMUX or MSYS2_ROOT)\n");
        return 127;
    }
    if (argc > 2 && wcscmp(argv[1], NATIVE_EXEC) == 0)
        return native_exec(argv + 2, argc - 2, tmux);
    GetModuleFileNameW(NULL, self, MAX_PATH);
    /* Shell-command panes find MSYS2's tools, as tmux's own /bin/sh does elsewhere;
     * native_exec drops the directory again. */
    wchar_t path[32767];
    msys_usr_bin = sibling(tmux, L"");
    msys_usr_bin[wcslen(msys_usr_bin) - 1] = 0;
    DWORD path_length = GetEnvironmentVariableW(L"PATH", path, 32000);
    if (path_length < 32000 - wcslen(msys_usr_bin) - 1 &&
        !wcsstr(_wcslwr(wcsdup(path)), _wcslwr(wcsdup(msys_usr_bin)))) {
        wcscat(wcscat(path, L";"), msys_usr_bin);
        SetEnvironmentVariableW(L"PATH", path);
        SetEnvironmentVariableW(PATH_MARK, L"1");
    }
    /* Global flags; c, f, L, S and T take a value. */
    wchar_t **out = calloc(argc * 2 + 1, sizeof(wchar_t *));
    int i = 1, used = 0, control = 0, detached = 0;
    for (; i < argc && argv[i][0] == L'-' && argv[i][1]; i++) {
        out[used++] = argv[i];
        if (wcscmp(argv[i], L"--") == 0) {
            i++;
            break;
        }
        wchar_t last = argv[i][wcslen(argv[i]) - 1];
        if (!wcschr(L"cfLST", last)) {
            control |= wcschr(argv[i], L'C') != NULL;
            continue;
        }
        if (++i < argc)
            out[used++] = last == L'f' && _wcsicmp(argv[i], L"nul") == 0 ? L"/dev/null" : argv[i];
    }
    for (int start = i; start < argc;) {
        int end = start;
        while (end < argc && wcscmp(argv[end], L";") != 0)
            end++;
        detached |= rewrite_command(argv + start, end - start, out, &used, self);
        if (end < argc)
            out[used++] = argv[end];
        start = end + 1;
    }
    /* Pane programs get their argv unconverted; native_exec drops the variable again. */
    if (!GetEnvironmentVariableW(L"MSYS2_ARG_CONV_EXCL", NULL, 0)) {
        SetEnvironmentVariableW(L"MSYS2_ARG_CONV_EXCL", L"*");
        SetEnvironmentVariableW(CONV_EXCL_MARK, L"1");
    }
    if (control && GetFileType(GetStdHandle(STD_INPUT_HANDLE)) != FILE_TYPE_CHAR) {
        SetEnvironmentVariableW(L"SHELL", L"/usr/bin/sh");
        /* script and the client end with this process, as a killed tmux client would. */
        return run(sibling(tmux, L"script.exe"), in_pty(tmux, out, used), kill_on_close_job(), 1, 0);
    }
    /* A server started here must not keep the caller's console (such as a scheduled
     * task's) alive, so it gets a console of its own. */
    return run(tmux, command_line(tmux, out, used), NULL, 1, detached ? CREATE_NO_WINDOW : 0);
}
