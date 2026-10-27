/* Credential-free, cancellable HTTPS probe, independent of the audio transport.
 * A successful public health check is not account authentication or audio delivery. */
#ifndef ST_CONNECTION_TEST_H
#define ST_CONNECTION_TEST_H
#include <ctype.h>

static bool st_health_url(const char *server, char *url, size_t size)
{
    /* The Server field is a hostname (optionally :port), never a URL or shell.
     * Reject URL syntax, credentials, control characters, truncation and options. */
    size_t n = strlen(server), host_len = 0;
    if (!n || n > 253 || !isalnum((unsigned char)server[0])) return false;
    while (host_len < n && server[host_len] != ':') {
        unsigned char c = (unsigned char)server[host_len++];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '.')) return false;
    }
    if (!host_len || !isalnum((unsigned char)server[host_len - 1])) return false;
    if (host_len < n) {
        unsigned port = 0; size_t i = host_len + 1;
        if (i == n || n - i > 5) return false;
        for (; i < n; i++) {
            if (server[i] < '0' || server[i] > '9') return false;
            port = port * 10 + (unsigned)(server[i] - '0');
        }
        if (!port || port > 65535) return false;
    }
    int count = snprintf(url, size, "https://%s/health", server);
    return count > 0 && (size_t)count < size;
}

static bool st_connection_test_stopping(void *data)
{
    struct st_filter *f = data;
    pthread_mutex_lock(&f->qlock);
    bool stop = f->test_stop;
    pthread_mutex_unlock(&f->qlock);
    return stop;
}

static void *st_connection_test_worker(void *data)
{
    struct st_filter *f = data;
    pthread_mutex_lock(&f->qlock);
    while (!f->test_stop) {
        while (!f->test_pending && !f->test_stop)
            pthread_cond_wait(&f->test_ready, &f->qlock);
        if (f->test_stop) break;
        char server[sizeof(f->test_server)];
        snprintf(server, sizeof(server), "%s", f->test_server);
        f->test_pending = false;
        pthread_mutex_unlock(&f->qlock);
        char url[320], body[4097]; size_t used = 0;
        const char *protocol = "https";
        bool valid = st_health_url(server, url, sizeof(url));
#ifdef ST_CONNECTION_TEST_MODE
        extern const char *st_connection_test_url;
        if (st_connection_test_url) {
            valid = snprintf(url, sizeof(url), "%s", st_connection_test_url) < (int)sizeof(url);
            protocol = "http";
        }
#endif
        bool healthy = false;
        if (valid && st_https_capture(url, protocol, "StreamTranslate-OBS-connection-test",
                st_connection_test_stopping, f, body, sizeof(body) - 1, &used) &&
            used >= 4 && memcmp(body + used - 4, "\n200", 4) == 0 && !memchr(body, 0, used - 4)) {
            body[used - 4] = 0;
            obs_data_t *health = obs_data_create_from_json(body);
            healthy = health && strcmp(obs_data_get_string(health, "status"), "ok") == 0;
            if (health) obs_data_release(health);
        }
        pthread_mutex_lock(&f->qlock);
        f->test_busy = false;
        snprintf(f->test_result, sizeof(f->test_result), "%s",
            !valid ? "Invalid Server: use a hostname, optionally followed by :port." :
            healthy ? "Server reachable (HTTPS health check). Audio unchanged; plugin key not tested." :
            "Could not verify server health. Audio unchanged; check your network and Server setting.");
        bool notify = !f->test_stop;
        pthread_mutex_unlock(&f->qlock);
        if (notify) {
            obs_source_t *ref = obs_source_get_ref(f->context);
            if (ref) obs_queue_task(OBS_TASK_UI, st_refresh_status_ui, ref, false);
        }
        pthread_mutex_lock(&f->qlock);
    }
    pthread_mutex_unlock(&f->qlock);
    return NULL;
}

static bool st_test_connection_clicked(obs_properties_t *props, obs_property_t *prop, void *data)
{
    (void)props; (void)prop;
    struct st_filter *f = data;
    if (!f) return false;
    pthread_mutex_lock(&f->qlock);
    if (!f->test_busy && !f->test_stop) {
        if (!f->test_started)
            f->test_started = pthread_create(&f->test_thread, NULL, st_connection_test_worker, f) == 0;
        if (f->test_started) {
            snprintf(f->test_server, sizeof(f->test_server), "%s", f->server);
            snprintf(f->test_result, sizeof(f->test_result), "Testing server reachability... Audio unchanged.");
            f->test_pending = f->test_busy = true;
            pthread_cond_signal(&f->test_ready);
        } else snprintf(f->test_result, sizeof(f->test_result), "Could not start connection test. Audio unchanged.");
    }
    pthread_mutex_unlock(&f->qlock);
    return true;
}

static void st_connection_test_stop(struct st_filter *f)
{
    pthread_mutex_lock(&f->qlock);
    f->test_stop = true;
    pthread_cond_signal(&f->test_ready);
    bool join = f->test_started;
    pthread_mutex_unlock(&f->qlock);
    if (join) pthread_join(f->test_thread, NULL);
}
#endif
