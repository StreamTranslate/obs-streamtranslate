/* Opt-in is local to this OBS installation and never stored in a scene. */
#if defined(ST_DESKTOP_UPDATER) && (defined(_WIN32) || defined(__APPLE__))
#include <errno.h>
#ifdef __APPLE__
#include <spawn.h>
#include <sys/wait.h>
#endif

#define ST_AUTO_CHOICE "auto-updates.enabled"
static char *st_auto_choice;
static char *st_auto_dir;
static bool st_auto_available;

#ifdef _WIN32
static HANDLE st_auto_process;
#else
static pid_t st_auto_pid;
#endif

static bool st_auto_enabled(void)
{
    return st_auto_choice && os_file_exists(st_auto_choice);
}

static char *st_auto_worker_path(const char *module)
{
    const char *slash = strrchr(module, '/');
#ifdef _WIN32
    const char *backslash = strrchr(module, '\\');
    if (backslash && (!slash || backslash > slash)) slash = backslash;
    const char *worker = "obs-streamtranslate-updater.exe";
#else
    const char *worker = "obs-streamtranslate-updater";
#endif
    if (!slash) return NULL;
    size_t prefix = (size_t)(slash - module + 1);
    char *path = bmalloc(prefix + strlen(worker) + 1);
    memcpy(path, module, prefix);
    strcpy(path + prefix, worker);
    return path;
}

#ifdef _WIN32
static bool st_auto_spawn(const char *worker, const char *module)
{
    if (st_auto_process) {
        if (WaitForSingleObject(st_auto_process, 0) == WAIT_TIMEOUT) return true;
        CloseHandle(st_auto_process);
        st_auto_process = NULL;
    }
    /* Run outside the plugin folder so Windows can rename the old DLL's folder. */
    struct dstr copy = {0}, command = {0};
    dstr_printf(&copy, "%s/desktop-updater-%s.exe", st_auto_dir, ST_RELEASE_VERSION);
    wchar_t *source = NULL, *executable = NULL, *wide_command = NULL;
    os_utf8_to_wcs_ptr(worker, 0, &source);
    os_utf8_to_wcs_ptr(copy.array, 0, &executable);
    bool started = false;
    if (!source || !executable) goto cleanup;
    if (!CopyFileW(source, executable, FALSE)) goto cleanup;
    dstr_printf(&command, "\"%s\" \"%s\" \"%s\" %s",
                copy.array, module, st_auto_choice, ST_RELEASE_VERSION);
    os_utf8_to_wcs_ptr(command.array, 0, &wide_command);
    if (wide_command) {
        STARTUPINFOW startup = { .cb = sizeof(startup) };
        PROCESS_INFORMATION process = {0};
        started = CreateProcessW(executable, wide_command, NULL, NULL, FALSE,
                                 CREATE_NO_WINDOW, NULL, NULL, &startup, &process);
        if (started) {
            CloseHandle(process.hThread);
            st_auto_process = process.hProcess;
        }
    }
cleanup:
    bfree(source);
    bfree(executable);
    bfree(wide_command);
    dstr_free(&copy);
    dstr_free(&command);
    return started;
}
#else
static bool st_auto_spawn(const char *worker, const char *module)
{
    if (st_auto_pid > 0 && waitpid(st_auto_pid, NULL, WNOHANG) == 0) return true;
    char *argv[] = {(char *)worker, (char *)module, st_auto_choice, ST_RELEASE_VERSION, NULL};
    /* The worker does not use OBS's environment, credentials or configuration. */
    char *environment[] = {"PATH=/usr/bin:/bin", NULL};
    return posix_spawn(&st_auto_pid, worker, NULL, NULL, argv, environment) == 0;
}
#endif

static bool st_auto_launch(void)
{
    if (!st_auto_available || !st_auto_enabled()) return false;
    const char *module = obs_get_module_binary_path(obs_current_module());
    char *worker = st_auto_worker_path(module);
    bool started = worker && st_auto_spawn(worker, module);
    bfree(worker);
    if (!started) blog(LOG_WARNING, "[streamtranslate] desktop update helper could not start");
    return started;
}

static void st_auto_start(void)
{
    st_auto_dir = obs_module_config_path(NULL);
    st_auto_choice = obs_module_config_path(ST_AUTO_CHOICE);
    const char *module = obs_get_module_binary_path(obs_current_module());
    char *worker = module ? st_auto_worker_path(module) : NULL;
    st_auto_available = st_auto_dir && st_auto_choice && worker && os_file_exists(worker);
    bfree(worker);
    st_auto_launch();
}

static void st_auto_shutdown(void)
{
#ifdef _WIN32
    if (st_auto_process) CloseHandle(st_auto_process);
#else
    if (st_auto_pid > 0) waitpid(st_auto_pid, NULL, WNOHANG);
#endif
    bfree(st_auto_dir);
    bfree(st_auto_choice);
}

static bool st_auto_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
    (void)props; (void)data;
    if (st_auto_enabled()) {
        if (os_unlink(st_auto_choice) != 0) {
            blog(LOG_WARNING, "[streamtranslate] could not disable desktop updates");
            return false;
        }
    } else {
        os_mkdirs(st_auto_dir);
        FILE *choice = os_fopen(st_auto_choice, "wb");
        if (!choice) {
            blog(LOG_WARNING, "[streamtranslate] could not save desktop update preference");
            return false;
        }
        fclose(choice);
        if (!st_auto_launch()) {
            os_unlink(st_auto_choice);
            return false;
        }
    }
    obs_property_set_description(property, st_auto_enabled() ?
        "Turn off automatic updates" : "Turn on automatic updates");
    return true;
}

static void st_auto_properties(obs_properties_t *props)
{
    if (!st_auto_available) return;
    obs_property_t *button = obs_properties_add_button2(props, "desktop_auto_updates",
        st_auto_enabled() ? "Turn off automatic updates" : "Turn on automatic updates",
        st_auto_clicked, NULL);
    obs_property_set_long_description(button,
        "Download StreamTranslate plugin updates and install them after OBS closes.");
}
#endif
