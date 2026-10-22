/* Included by plugin.c: independent, bounded, credential-free HTTPS worker.
 * No audio socket, plugin key, caption, source name or settings enter here. */
#ifndef ST_UPDATE_CHECK_H
#define ST_UPDATE_CHECK_H
#include "update-policy.h"

#define ST_RELEASES_URL "https://github.com/StreamTranslate/obs-streamtranslate/releases/latest"
#define ST_UPDATE_BODY_MAX (128 * 1024)
#define ST_UPDATE_COOLDOWN_NS (900ULL * 1000000000ULL)
#define ST_UPDATE_TIMEOUT_NS (8ULL * 1000000000ULL)

enum st_update_result { ST_UPDATE_UNCHECKED, ST_UPDATE_CHECKING,
                        ST_UPDATE_CURRENT, ST_UPDATE_AVAILABLE, ST_UPDATE_FAILED };
static struct {
    pthread_mutex_t lock;
    pthread_cond_t ready;
    pthread_t thread;
    bool started, stop, pending;
    uint64_t requested_ns;
    enum st_update_result result;
    char latest[32];
} st_updates = { .lock = PTHREAD_MUTEX_INITIALIZER, .ready = PTHREAD_COND_INITIALIZER };

struct st_update_fetch {
    char body[ST_UPDATE_BODY_MAX + 1];
    size_t used;
    bool done, complete, failed;
    unsigned status;
    struct lws_context *context;
    lws_sorted_usec_list_t pulse;
};

static bool st_updates_stopping(void)
{
    pthread_mutex_lock(&st_updates.lock);
    bool stop = st_updates.stop;
    pthread_mutex_unlock(&st_updates.lock);
    return stop;
}

static void st_update_pulse(lws_sorted_usec_list_t *sul)
{
    struct st_update_fetch *fetch = lws_container_of(sul, struct st_update_fetch, pulse);
    if (!fetch->done && !st_updates_stopping())
        lws_sul_schedule(fetch->context, 0, &fetch->pulse, st_update_pulse, 100000);
}

static int st_update_http(struct lws *wsi, enum lws_callback_reasons reason,
                          void *user, void *in, size_t len)
{
    (void)user;
    struct st_update_fetch *fetch = lws_context_user(lws_get_context(wsi));
    if (!fetch) return 0;
    switch (reason) {
    case LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER: {
        unsigned char **p = in, *end = *p + len;
        const char agent[] = "StreamTranslate-OBS-update-check";
        return lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_USER_AGENT,
                    (const unsigned char *)agent, sizeof(agent) - 1, p, end);
    }
    case LWS_CALLBACK_ESTABLISHED_CLIENT_HTTP:
        fetch->status = lws_http_client_http_response(wsi);
        if (fetch->status != 200) {
            fetch->failed = fetch->done = true;
            return -1;
        }
        break;
    case LWS_CALLBACK_CLIENT_HTTP_REDIRECT:
        /* Never follow redirects to another host or a non-TLS endpoint. */
        fetch->failed = fetch->done = true;
        return -1;
    case LWS_CALLBACK_RECEIVE_CLIENT_HTTP: {
        char buffer[LWS_PRE + 4096], *p = buffer + LWS_PRE;
        int size = sizeof(buffer) - LWS_PRE;
        if (lws_http_client_read(wsi, &p, &size) < 0) {
            fetch->failed = fetch->done = true;
            return -1;
        }
        break;
    }
    case LWS_CALLBACK_RECEIVE_CLIENT_HTTP_READ:
        if (len > ST_UPDATE_BODY_MAX - fetch->used || memchr(in, '\0', len)) {
            fetch->failed = fetch->done = true;
            return -1;
        }
        memcpy(fetch->body + fetch->used, in, len);
        fetch->used += len;
        fetch->body[fetch->used] = 0;
        break;
    case LWS_CALLBACK_COMPLETED_CLIENT_HTTP:
        fetch->complete = fetch->done = true;
        break;
    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        fetch->failed = fetch->done = true;
        break;
    case LWS_CALLBACK_CLOSED_CLIENT_HTTP:
        fetch->done = true;
        break;
    default: break;
    }
    return 0;
}

static const struct lws_protocols st_update_protocols[] = {
    { "st-update-http", st_update_http, 0, 4096, 0, NULL, 0 },
    LWS_PROTOCOL_LIST_TERM,
};

static enum st_update_result st_update_parse(const char *body, char latest[32])
{
    obs_data_t *release = obs_data_create_from_json(body);
    if (!release) return ST_UPDATE_FAILED;
    unsigned parts[3];
    const char *tag = obs_data_get_string(release, "tag_name");
    /* OBS getters default absent fields to false; require actual boolean fields. */
    obs_data_item_t *draft = obs_data_item_byname(release, "draft");
    obs_data_item_t *prerelease = obs_data_item_byname(release, "prerelease");
    bool valid = draft && prerelease &&
        obs_data_item_gettype(draft) == OBS_DATA_BOOLEAN &&
        obs_data_item_gettype(prerelease) == OBS_DATA_BOOLEAN &&
        !obs_data_get_bool(release, "draft") && !obs_data_get_bool(release, "prerelease") &&
        st_parse_release_version(tag, parts);
    if (draft) obs_data_item_release(&draft);
    if (prerelease) obs_data_item_release(&prerelease);
    enum st_update_result result = ST_UPDATE_FAILED;
    if (valid) {
        snprintf(latest, 32, "%u.%u.%u", parts[0], parts[1], parts[2]);
        result = st_release_is_newer(latest, ST_RELEASE_VERSION) ?
            ST_UPDATE_AVAILABLE : ST_UPDATE_CURRENT;
    }
    obs_data_release(release);
    return result;
}

static enum st_update_result st_update_fetch_release(char latest[32])
{
    struct st_update_fetch *fetch = calloc(1, sizeof(*fetch));
    if (!fetch) return ST_UPDATE_FAILED;
    struct lws_context_creation_info info = {0};
    info.port = CONTEXT_PORT_NO_LISTEN;
    info.protocols = st_update_protocols;
    info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
    info.client_ssl_ca_filepath = st_ca_bundle_path();
    info.user = fetch;
    info.connect_timeout_secs = 5;
    fetch->context = lws_create_context(&info);
    if (!fetch->context) { free(fetch); return ST_UPDATE_FAILED; }
    struct lws_client_connect_info ci = {0};
    ci.context = fetch->context;
    ci.address = ci.host = "api.github.com";
    ci.port = 443;
    ci.path = "/repos/StreamTranslate/obs-streamtranslate/releases/latest";
    ci.method = "GET";
    ci.local_protocol_name = "st-update-http";
    ci.ssl_connection = LCCSCF_USE_SSL;
    ci.alpn = "http/1.1";
    uint64_t start = os_gettime_ns();
    lws_sul_schedule(fetch->context, 0, &fetch->pulse, st_update_pulse, 100000);
    if (!lws_client_connect_via_info(&ci)) fetch->failed = fetch->done = true;
    while (!fetch->done && !st_updates_stopping() &&
           os_gettime_ns() - start < ST_UPDATE_TIMEOUT_NS)
        if (lws_service(fetch->context, 100) < 0) break;
    enum st_update_result result = ST_UPDATE_FAILED;
    if (fetch->complete && !fetch->failed && fetch->status == 200)
        result = st_update_parse(fetch->body, latest);
    lws_sul_cancel(&fetch->pulse);
    lws_context_destroy(fetch->context);
    free(fetch);
    return result;
}

static void st_update_refresh_filter(obs_source_t *parent, obs_source_t *filter, void *param)
{
    (void)parent; (void)param;
    if (strcmp(obs_source_get_id(filter), "streamtranslate_audio_filter") != 0) return;
    obs_source_t *ref = obs_source_get_ref(filter);
    if (ref) obs_queue_task(OBS_TASK_UI, st_refresh_status_ui, ref, false);
}

static bool st_update_refresh_source(void *param, obs_source_t *source)
{
    obs_source_enum_filters(source, st_update_refresh_filter, param);
    return true;
}

static void *st_update_worker(void *unused)
{
    (void)unused;
    pthread_mutex_lock(&st_updates.lock);
    while (!st_updates.stop) {
        while (!st_updates.pending && !st_updates.stop)
            pthread_cond_wait(&st_updates.ready, &st_updates.lock);
        if (st_updates.stop) break;
        st_updates.pending = false;
        pthread_mutex_unlock(&st_updates.lock);
        char latest[32] = "";
        enum st_update_result result = st_update_fetch_release(latest);
        pthread_mutex_lock(&st_updates.lock);
        st_updates.result = result;
        snprintf(st_updates.latest, sizeof(st_updates.latest), "%s", latest);
        bool notify = !st_updates.stop;
        pthread_mutex_unlock(&st_updates.lock);
        if (notify) obs_enum_sources(st_update_refresh_source, NULL);
        pthread_mutex_lock(&st_updates.lock);
    }
    pthread_mutex_unlock(&st_updates.lock);
    return NULL;
}

static bool st_check_updates_clicked(obs_properties_t *props, obs_property_t *prop, void *data)
{
    (void)props; (void)prop; (void)data;
    pthread_mutex_lock(&st_updates.lock);
    uint64_t now = os_gettime_ns();
    if (st_updates.started && !st_updates.stop &&
        st_updates.result != ST_UPDATE_CHECKING &&
        (!st_updates.requested_ns || now - st_updates.requested_ns >= ST_UPDATE_COOLDOWN_NS)) {
        st_updates.requested_ns = now;
        st_updates.result = ST_UPDATE_CHECKING;
        st_updates.pending = true;
        pthread_cond_signal(&st_updates.ready);
    }
    pthread_mutex_unlock(&st_updates.lock);
    return true;
}

static void st_update_properties(obs_properties_t *props, struct st_filter *f)
{
    char text[320], latest[32];
    pthread_mutex_lock(&st_updates.lock);
    enum st_update_result result = st_updates.result;
    snprintf(latest, sizeof(latest), "%s", st_updates.latest);
    bool can_check = st_updates.started && result != ST_UPDATE_CHECKING &&
        (!st_updates.requested_ns || os_gettime_ns() - st_updates.requested_ns >= ST_UPDATE_COOLDOWN_NS);
    pthread_mutex_unlock(&st_updates.lock);
    const char *status = result == ST_UPDATE_CHECKING ? "Checking for updates..." :
        result == ST_UPDATE_FAILED ? "Could not check for updates. Captions are unaffected; use the downloads link." :
        result == ST_UPDATE_CURRENT ? "No newer stable release found." : "Not checked yet.";
    if (result == ST_UPDATE_AVAILABLE)
        snprintf(text, sizeof(text), "Installed: %s. Update available: %s. Install when you are not streaming, then restart OBS.", ST_RELEASE_VERSION, latest);
    else snprintf(text, sizeof(text), "Installed: %s. %s", ST_RELEASE_VERSION, status);
    obs_properties_add_text(props, "plugin_update_text", "Plugin version / updates", OBS_TEXT_INFO);
    if (f) {
        obs_data_t *settings = obs_source_get_settings(f->context);
        obs_data_set_string(settings, "plugin_update_text", text);
        obs_data_release(settings);
    }
    obs_property_t *check = obs_properties_add_button2(props, "check_updates", "Check for updates", st_check_updates_clicked, NULL);
    obs_property_set_enabled(check, can_check);
    obs_property_set_long_description(check, "Checks public GitHub release metadata only. Results are cached for 15 minutes. No plugin key or audio is sent.");
    obs_property_t *download = obs_properties_add_button2(props, "plugin_downloads", "Open plugin downloads / release notes", NULL, NULL);
    obs_property_button_set_type(download, OBS_BUTTON_URL);
    obs_property_button_set_url(download, (char *)ST_RELEASES_URL);
}

static void st_updates_start(void)
{
    /* Resolve this existing cached helper before either worker can use it. */
    (void)st_ca_bundle_path();
    pthread_mutex_lock(&st_updates.lock);
    st_updates.stop = false;
    if (!st_updates.started) {
        st_updates.requested_ns = 0;
        st_updates.pending = false;
        st_updates.result = ST_UPDATE_UNCHECKED;
        st_updates.latest[0] = 0;
        st_updates.started = pthread_create(&st_updates.thread, NULL, st_update_worker, NULL) == 0;
        if (!st_updates.started) st_updates.result = ST_UPDATE_FAILED;
    }
    pthread_mutex_unlock(&st_updates.lock);
    st_check_updates_clicked(NULL, NULL, NULL);
}

static void st_updates_stop(void)
{
    pthread_mutex_lock(&st_updates.lock);
    bool join = st_updates.started;
    st_updates.stop = true;
    pthread_cond_signal(&st_updates.ready);
    pthread_mutex_unlock(&st_updates.lock);
    if (join) pthread_join(st_updates.thread, NULL);
    pthread_mutex_lock(&st_updates.lock);
    st_updates.started = false;
    pthread_mutex_unlock(&st_updates.lock);
}
#endif
