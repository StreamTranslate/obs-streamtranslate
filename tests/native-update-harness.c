#include <stdbool.h>
#include <assert.h>
#define ST_UPDATE_TEST_MODE
const char *st_update_test_url;
const char *st_update_test_executable;
#include "../plugin.c"

static void set_fixture(const char *path)
{
    static char url[256];
    snprintf(url, sizeof(url), "http://127.0.0.1:8797%s", path);
    st_update_test_url = url;
}

static const char *name(void *unused) { (void)unused; return "Update UI test"; }
static void *make_source(obs_data_t *s, obs_source_t *source) { (void)s; return source; }
static void destroy_source(void *unused) { (void)unused; }

int main(int argc, char **argv)
{
    assert(obs_startup("en-US", NULL, NULL));
    assert(strcmp(st_update_api_url(), "https://api.github.com/repos/StreamTranslate/obs-streamtranslate/releases/latest") == 0);
    assert(strcmp(st_update_allowed_protocol(), "https") == 0);
    (void)argv;
    char latest[32] = "";
    if (argc > 1) {
        assert(strcmp(st_update_api_url(), "https://api.github.com/repos/StreamTranslate/obs-streamtranslate/releases/latest") == 0);
        assert(strcmp(st_update_allowed_protocol(), "https") == 0);
        enum st_update_result result = st_update_fetch_release(latest);
        printf("LIVE update metadata: result=%d latest=%s\n", result, latest);
        assert(result == ST_UPDATE_CURRENT || result == ST_UPDATE_AVAILABLE);
        obs_shutdown();
        return 0;
    }
    const char *cases[] = {"/new", "/equal", "/older", "/draft", "/prerelease", "/missing",
        "/wrong-type", "/markup", "/blocked", "/redirect", "/oversize", "/malformed", "/nul"};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        set_fixture(cases[i]); latest[0] = 0;
        enum st_update_result result = st_update_fetch_release(latest);
        assert(result == (i == 0 ? ST_UPDATE_AVAILABLE : i < 3 ? ST_UPDATE_CURRENT : ST_UPDATE_FAILED));
        if (i >= 3) assert(!latest[0]);
        printf("PASS HTTP fixture %s\n", cases[i]);
    }
    set_fixture("/slow");
    uint64_t start = os_gettime_ns();
    assert(st_update_fetch_release(latest) == ST_UPDATE_FAILED);
    assert(os_gettime_ns() - start < 10000000000ULL);
    puts("PASS slow update service bounded by timeout");

    struct obs_source_info info = {.id="update_ui_test", .type=OBS_SOURCE_TYPE_INPUT,
        .output_flags=OBS_SOURCE_AUDIO, .get_name=name, .create=make_source, .destroy=destroy_source};
    obs_register_source(&info);
    obs_source_t *source = obs_source_create_private(info.id, "Updates", NULL);
    assert(source);
    struct st_filter f = {0}; f.context = source; f.connected = true;
    pthread_mutex_init(&f.qlock, NULL);
    strcpy(f.plugin_key, "DO-NOT-SEND-THIS");
    st_updates.result = ST_UPDATE_AVAILABLE;
    st_updates.started = true;
    st_updates.requested_ns = os_gettime_ns();
    strcpy(st_updates.latest, "0.1.13");
    obs_properties_t *props = st_get_properties(&f);
    assert(obs_properties_get(props, "check_updates"));
    assert(obs_property_enabled(obs_properties_get(props, "check_updates")));
    st_check_updates_clicked(NULL, NULL, NULL);
    assert(!st_updates.pending);
    obs_property_t *link = obs_properties_get(props, "plugin_downloads");
    assert(obs_property_button_type(link) == OBS_BUTTON_URL);
    assert(strcmp(obs_property_button_url(link), ST_RELEASES_URL) == 0);
    obs_data_t *settings = obs_source_get_settings(source);
    const char *text = obs_data_get_string(settings, "plugin_update_text");
    assert(strstr(text, ST_RELEASE_VERSION) && strstr(text, "Update available: 0.1.13"));
    assert(!strstr(text, f.plugin_key));
    assert(f.connected && !f.stop && !f.user_paused && !f.reconnects);
    obs_properties_destroy(props); obs_data_release(settings);
    obs_source_release(source);
    pthread_mutex_destroy(&f.qlock);
    st_updates.started = false;
    puts("PASS installed version, update notice, fixed HTTPS download link and unchanged audio state");

    set_fixture("/new");
    st_updates_start();
    uint64_t wait_start = os_gettime_ns();
    enum st_update_result completed;
    do {
        os_sleep_ms(20);
        pthread_mutex_lock(&st_updates.lock);
        completed = st_updates.result;
        pthread_mutex_unlock(&st_updates.lock);
    } while (completed == ST_UPDATE_CHECKING && os_gettime_ns() - wait_start < ST_UPDATE_TIMEOUT_NS);
    assert(completed == ST_UPDATE_AVAILABLE);
    uint64_t previous_request = st_updates.requested_ns;
    st_check_updates_clicked(NULL, NULL, NULL);
    assert(st_updates.requested_ns == previous_request && !st_updates.pending);
    st_updates_stop();
    puts("PASS completed check stays cached across subsequent requests");

    /* Real worker cancellation and process-wide cooldown, without any audio. */
    set_fixture("/slow");
    /* A stuck resolver/helper that ignores curl's own timeout must still be
     * killed by the parent. This is a local fixture executable only. */
    st_update_test_executable = getenv("ST_STUCK_HELPER");
    assert(st_update_test_executable);
    start = os_gettime_ns();
    assert(st_update_fetch_release(latest) == ST_UPDATE_FAILED);
    assert(os_gettime_ns() - start < 10000000000ULL);
    puts("PASS stuck DNS/helper process killed by parent deadline");
    st_updates_start();
    uint64_t requested = st_updates.requested_ns;
    st_check_updates_clicked(NULL, NULL, NULL);
    assert(st_updates.requested_ns == requested);
    os_sleep_ms(200);
    start = os_gettime_ns(); st_updates_stop();
    assert(os_gettime_ns() - start < 2000000000ULL);
    assert(!st_updates.started);
    puts("PASS shared request deduplication and prompt worker shutdown");
    obs_shutdown();
}
