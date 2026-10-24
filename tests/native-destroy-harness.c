/* Real poll-backend teardown: no host capture, UI, playback, account or provider. */
#include <libwebsockets.h>
#include <assert.h>
#include <unistd.h>
static struct lws *test_connect(const struct lws_client_connect_info *);
#define lws_client_connect_via_info test_connect
#include "../plugin.c"
#undef lws_client_connect_via_info
#ifndef ST_LOCAL_TEST_PORT
#define ST_LOCAL_TEST_PORT 8796
#endif
static struct lws *test_connect(const struct lws_client_connect_info *source)
{
    assert(!strcmp(source->address, ST_DEFAULT_SERVER));
    assert(source->port == 443 && source->ssl_connection == LCCSCF_USE_SSL);
    struct lws_client_connect_info ci = *source;
    ci.port = ST_LOCAL_TEST_PORT;
    ci.ssl_connection = 0; /* TLS adapter retains verification for native TLS runs. */
    ci.address = ci.host = "127.0.0.1";
    return lws_client_connect_via_info(&ci);
}
static const char *name(void *x) { (void)x; return "Synthetic teardown audio"; }
static void *make_source(obs_data_t *settings, obs_source_t *context)
{ (void)settings; return context; }
static void free_source(void *data) { (void)data; }
struct ui_job { obs_task_t task; void *param; struct ui_job *next; };
static pthread_mutex_t ui_lock = PTHREAD_MUTEX_INITIALIZER;
static struct ui_job *ui_jobs;
static unsigned ui_queued, ui_completed;
static void test_queue_ui(obs_task_t task, void *param, bool wait)
{
    assert(!wait);
    struct ui_job *job = calloc(1, sizeof(*job)); assert(job);
    job->task = task; job->param = param;
    pthread_mutex_lock(&ui_lock);
    struct ui_job **end = &ui_jobs;
    while (*end) end = &(*end)->next;
    *end = job; ui_queued++;
    pthread_mutex_unlock(&ui_lock);
}
static void test_pump_ui(void)
{
    pthread_mutex_lock(&ui_lock);
    struct ui_job *jobs = ui_jobs; ui_jobs = NULL;
    pthread_mutex_unlock(&ui_lock);
    while (jobs) {
        struct ui_job *next = jobs->next;
        jobs->task(jobs->param); free(jobs); jobs = next; ui_completed++;
    }
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    const char *mode = argv[1];
    assert(!strcmp(mode, "active") || !strcmp(mode, "pending") || !strcmp(mode, "disconnected"));
    assert(obs_startup("en-US", NULL, NULL)); obs_set_ui_task_handler(test_queue_ui);
    struct obs_audio_info ai = {.samples_per_sec=48000, .speakers=SPEAKERS_STEREO};
    assert(obs_reset_audio(&ai));
    struct obs_source_info src = {.id="qa_destroy_audio", .type=OBS_SOURCE_TYPE_INPUT,
        .output_flags=OBS_SOURCE_AUDIO, .get_name=name, .create=make_source, .destroy=free_source};
    obs_register_source(&src);
    obs_source_t *source = obs_source_create_private("qa_destroy_audio", "QA teardown", NULL);
    obs_set_output_source(0, source);
    obs_data_t *settings = obs_data_create(); st_get_defaults(settings);
    obs_data_set_string(settings, "plugin_key", "local-test-only");
#if ST_TWO_SPEAKER_ENABLED
    obs_data_set_int(settings, "mic_mode", 2);
#endif
    struct st_filter *f = st_create(settings, source);
    float left[480], right[480]; double phase=0; uint64_t start=os_gettime_ns();
    bool terminal_observed=false; int terminal_sent=0; uint64_t terminal_at=0;
    while (os_gettime_ns()-start < 3000000000ULL) {
        for (int i=0; i<480; i++) {
            left[i]=0.3f*sin(phase); right[i]=0.2f*cos(phase);
            phase += 2*3.141592653589793*440/48000;
        }
        struct obs_audio_data a={.data={(uint8_t*)left,(uint8_t*)right},
            .frames=480,.timestamp=os_gettime_ns()};
        st_filter_audio(f,&a); test_pump_ui();
        if (!strcmp(mode,"disconnected") && f->terminal_paused && !f->wsi) {
            if (!terminal_observed) {
                terminal_observed=true; terminal_sent=f->audio_chunks_sent;
                terminal_at=os_gettime_ns();
            }
            assert(f->audio_chunks_sent==terminal_sent);
        }
        os_sleep_ms(10);
    }
    if (!strcmp(mode,"active")) assert(f->connected && f->audio_chunks_sent>30);
    else if (!strcmp(mode,"pending")) assert(f->wsi && !f->connected && f->audio_chunks_sent==0);
    else {
        assert(!f->wsi && !f->connected && f->terminal_paused && terminal_observed);
        assert(os_gettime_ns()-terminal_at>1000000000ULL);
        fprintf(stderr,"terminal_sent_count=%d; no_later_pcm=true\n",terminal_sent);
    }
    /* Do not let a lifecycle regression leave a detached native process. */
    alarm(4); start=os_gettime_ns(); st_destroy(f);
    uint64_t elapsed=os_gettime_ns()-start; alarm(0);
    fprintf(stderr, "destroy_elapsed_ns=%" PRIu64 "\n", elapsed);
    assert(elapsed < 1000000000ULL);
    /* Closed callbacks queue a source ref. Drain it after f is freed. */
    test_pump_ui(); assert(ui_queued>0 && ui_completed==ui_queued && !ui_jobs);
    obs_set_output_source(0,NULL); obs_source_release(source); obs_data_release(settings);
    obs_shutdown(); puts("PASS native bounded teardown and queued UI cleanup"); return 0;
}
