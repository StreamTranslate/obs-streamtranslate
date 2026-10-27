/* Keep optional update-network work outside OBS's address space. Even a stuck
 * system DNS resolver is cancellable by terminating this credential-free child.
 * No shell, user URL, shell expansion, curlrc or install command is involved. */
#ifndef ST_UPDATE_PROCESS_H
#define ST_UPDATE_PROCESS_H
#include <wchar.h>

#ifndef _WIN32
#include <spawn.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
extern char **environ;
#endif

static bool st_updates_stopping(void)
{
    pthread_mutex_lock(&st_updates.lock);
    bool stop = st_updates.stop;
    pthread_mutex_unlock(&st_updates.lock);
    return stop;
}

static const char *st_update_api_url(void)
{
#ifdef ST_UPDATE_TEST_MODE
    extern const char *st_update_test_url;
    if (st_update_test_url) return st_update_test_url;
#endif
    return "https://api.github.com/repos/StreamTranslate/obs-streamtranslate/releases/latest";
}

static const char *st_update_allowed_protocol(void)
{
#ifdef ST_UPDATE_TEST_MODE
    extern const char *st_update_test_url;
    if (st_update_test_url) return "http";
#endif
    return "https";
}

static bool st_https_capture(const char *request_url, const char *allowed_protocol,
                             const char *user_agent, bool (*stopping)(void *), void *opaque,
                             char *body, size_t capacity, size_t *used)
{
    *used = 0;
    uint64_t start = os_gettime_ns();
    bool ok = false;
#ifdef _WIN32
    wchar_t executable[MAX_PATH], command[2048], url[1024], protocol[16], agent[128];
    UINT executable_length = GetSystemDirectoryW(executable, MAX_PATH);
    if (!executable_length || executable_length + 10 >= MAX_PATH) return false;
    wcscat(executable, L"\\curl.exe");
#ifdef ST_UPDATE_TEST_MODE
    extern const char *st_update_test_executable;
    if (st_update_test_executable &&
        !MultiByteToWideChar(CP_UTF8, 0, st_update_test_executable, -1, executable, MAX_PATH)) return false;
#endif
    if (!MultiByteToWideChar(CP_UTF8, 0, request_url, -1, url, 1024) ||
        !MultiByteToWideChar(CP_UTF8, 0, allowed_protocol, -1, protocol, 16) ||
        !MultiByteToWideChar(CP_UTF8, 0, user_agent, -1, agent, 128)) return false;
    _snwprintf(command, 2048,
        L"\"%ls\" --disable --silent --fail --proto =%ls --connect-timeout 3 --max-time 6 "
        L"--max-filesize 131072 --user-agent %ls "
        L"--write-out \"\\n%%{http_code}\" --url \"%ls\"", executable, protocol, agent, url);
    SECURITY_ATTRIBUTES attributes = {sizeof(attributes), NULL, TRUE};
    HANDLE read_pipe = NULL, write_pipe = NULL;
    if (!CreatePipe(&read_pipe, &write_pipe, &attributes, 0)) return false;
    if (!SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(read_pipe); CloseHandle(write_pipe); return false;
    }
    STARTUPINFOEXW startup = {0}; startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdOutput = startup.StartupInfo.hStdError = write_pipe;
    SIZE_T attributes_size = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &attributes_size);
    startup.lpAttributeList = malloc(attributes_size);
    if (!startup.lpAttributeList ||
        !InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attributes_size)) {
        free(startup.lpAttributeList); CloseHandle(read_pipe); CloseHandle(write_pipe); return false;
    }
    if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   &write_pipe, sizeof(write_pipe), NULL, NULL)) {
        DeleteProcThreadAttributeList(startup.lpAttributeList); free(startup.lpAttributeList);
        CloseHandle(read_pipe); CloseHandle(write_pipe); return false;
    }
    PROCESS_INFORMATION process = {0};
    BOOL created = CreateProcessW(executable, command, NULL, NULL, TRUE,
                                  CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
                                  NULL, NULL, &startup.StartupInfo, &process);
    DeleteProcThreadAttributeList(startup.lpAttributeList); free(startup.lpAttributeList);
    CloseHandle(write_pipe);
    if (!created) { CloseHandle(read_pipe); return false; }
    bool exited = false, overflow = false;
    while (!stopping(opaque) && os_gettime_ns() - start < ST_UPDATE_TIMEOUT_NS) {
        DWORD available = 0, count = 0;
        if (PeekNamedPipe(read_pipe, NULL, 0, NULL, &available, NULL) && available) {
            if (*used == capacity) { overflow = true; break; }
            DWORD wanted = (DWORD)(capacity - *used);
            if (wanted > available) wanted = available;
            if (!ReadFile(read_pipe, body + *used, wanted, &count, NULL)) break;
            *used += count;
            continue;
        }
        if (exited) break;
        if (WaitForSingleObject(process.hProcess, 0) == WAIT_OBJECT_0) {
            DWORD code = 1; GetExitCodeProcess(process.hProcess, &code);
            /* The child may write between PeekNamedPipe and this check.
             * Drain again after observing exit, before interpreting stdout. */
            exited = true; ok = code == 0; continue;
        }
        os_sleep_ms(10);
    }
    if (!exited) {
        TerminateProcess(process.hProcess, 1);
        (void)WaitForSingleObject(process.hProcess, 1000);
    }
    if (overflow) ok = false;
    CloseHandle(read_pipe); CloseHandle(process.hThread); CloseHandle(process.hProcess);
#else
    const char *executable = "/usr/bin/curl";
#ifdef ST_UPDATE_TEST_MODE
    extern const char *st_update_test_executable;
    if (st_update_test_executable) executable = st_update_test_executable;
#endif
    char *const args[] = {(char *)executable, "--disable", "--silent", "--fail",
        "--proto", (char *)(strcmp(allowed_protocol, "http") == 0 ? "=http" : "=https"),
        "--connect-timeout", "3", "--max-time", "6", "--max-filesize", "131072",
        "--user-agent", (char *)user_agent, "--write-out", "\n%{http_code}",
        "--url", (char *)request_url, NULL};
    int pipes[2];
    if (pipe(pipes) != 0) return false;
    int flags = fcntl(pipes[0], F_GETFL);
    if (flags < 0 || fcntl(pipes[0], F_SETFL, flags | O_NONBLOCK) < 0) {
        close(pipes[0]); close(pipes[1]); return false;
    }
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) {
        close(pipes[0]); close(pipes[1]); return false;
    }
    int error = posix_spawn_file_actions_adddup2(&actions, pipes[1], STDOUT_FILENO);
    error |= posix_spawn_file_actions_addclose(&actions, pipes[0]);
    error |= posix_spawn_file_actions_addclose(&actions, pipes[1]);
    error |= posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    error |= posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
#if defined(__GLIBC__) && defined(__GLIBC_PREREQ)
#if __GLIBC_PREREQ(2, 34)
    error |= posix_spawn_file_actions_addclosefrom_np(&actions, 3);
#endif
#endif
    posix_spawnattr_t attributes;
    int attributes_error = posix_spawnattr_init(&attributes);
#ifdef __APPLE__
    if (!attributes_error)
        attributes_error = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_CLOEXEC_DEFAULT);
#endif
    pid_t pid = -1;
    if (!error && !attributes_error)
        error = posix_spawn(&pid, executable, &actions, &attributes, args, environ);
    else error = 1;
    if (!attributes_error) posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
    close(pipes[1]);
    if (error) { close(pipes[0]); return false; }
    int status = 0;
    bool exited = false, eof = false, overflow = false;
    while (!stopping(opaque) && os_gettime_ns() - start < ST_UPDATE_TIMEOUT_NS) {
        char buffer[4096];
        ssize_t count = read(pipes[0], buffer, sizeof(buffer));
        if (count > 0) {
            if ((size_t)count > capacity - *used) { overflow = true; break; }
            memcpy(body + *used, buffer, (size_t)count); *used += (size_t)count;
        } else if (count == 0) eof = true;
        else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) break;
        if (!exited) exited = waitpid(pid, &status, WNOHANG) == pid;
        if (exited && eof) { ok = WIFEXITED(status) && WEXITSTATUS(status) == 0; break; }
        if (count <= 0) os_sleep_ms(10);
    }
    if (!exited) {
        (void)kill(pid, SIGKILL);
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    }
    if (overflow) ok = false;
    close(pipes[0]);
#endif
    body[*used] = 0;
    return ok;
}
static bool st_update_capture_stopping(void *unused)
{
    (void)unused;
    return st_updates_stopping();
}

static bool st_update_capture(char *body, size_t capacity, size_t *used)
{
    return st_https_capture(st_update_api_url(), st_update_allowed_protocol(),
        "StreamTranslate-OBS-update-check", st_update_capture_stopping, NULL,
        body, capacity, used);
}
#endif
