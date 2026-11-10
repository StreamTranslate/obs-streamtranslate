/*
 * obs-streamtranslate — streams an OBS audio source to StreamTranslate for
 * real-time translated captions, from inside any OBS instance (including
 * cloud-hosted ones like IRLToolkit where no local browser exists).
 *
 * Design: an audio FILTER you attach to the audio source you stream with.
 * It passes audio through untouched, and in parallel converts it to mono
 * s16le at the OBS pipeline sample rate and ships it over a TLS websocket
 * to wss://<server>/audio?pluginKey=...&rate=<hz>. The plugin key is the
 * only credential — generate it from your StreamTranslate account.
 *
 * Linux-compilable (IRLToolkit third-party plugin requirement).
 * Deps: libobs, libwebsockets.
 */

#include <obs-module.h>
#include <media-io/audio-resampler.h>
#include <media-io/audio-io.h>
#include <util/platform.h>
#include <util/threading.h>
#include <libwebsockets.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <inttypes.h>
#include <math.h>
#include <stdatomic.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

OBS_DECLARE_MODULE()

#include "retry-policy.h"
#include "ca-path.h"
#define ST_PLUGIN_VERSION "two-speaker-native-cc-v1"
#define ST_RELEASE_VERSION "0.1.15"
#include "native-cc.h"
#ifdef _WIN32
#define ST_PLATFORM "windows"
#elif defined(__APPLE__)
#define ST_PLATFORM "macos"
#else
#define ST_PLATFORM "linux"
#endif
#define ST_QUEUE_MAX 256 /* count cap, also bounded by age below */
#define ST_QUEUE_MAX_AGE_NS 3000000000ULL /* at most 3s of reconnect pre-roll */

struct st_chunk {
	struct st_chunk *next;
	size_t len;
    uint64_t captured_ns;
	unsigned char *buf; /* includes LWS_PRE headroom; payload at buf+LWS_PRE */
};

struct st_filter {
	obs_source_t *context;

	/* config */
	char server[256];
	char plugin_key[128];
	/* Two-speaker (stereo split): keep L/R separate instead of folding to mono, so
	   a Rode/DJI receiver in SPLIT mode gives one caption track per transmitter. */
	bool stereo_split;
	/* Which speaker this filter carries when it sits on a single-mic source:
	   0 = Mic 1 (default), 1 = Mic 2. Sent as &speaker=1 so the server runs a
	   second caption track for the same room. */
	int speaker;
	atomic_bool native_twitch_cc;
	atomic_bool two_speakers;
	struct st_cc_state cc_state;
	struct st_cc_fragments cc_fragments;
	char cc_acks[8][256];
	unsigned cc_ack_read, cc_ack_count;
	bool cc_ack_last;

	/* live status shown in the filter UI */
	char status[320];
	unsigned last_close_code;
	bool show_diagnostics;
    /* Independent HTTPS health probe: never replaces the audio connection. */
    pthread_t test_thread;
    pthread_cond_t test_ready;
    bool test_started, test_stop, test_pending, test_busy;
    char test_server[256], test_result[320];
    bool status_dirty;
	bool muted_now;
	int  audio_chunks_captured;
	int  audio_chunks_sent;
	char filter_id[48];
    char capture_id[48]; /* per intentional capture; protected by qlock */
	uint64_t callbacks, input_frames, last_callback_ns, last_send_ns;
	uint64_t written_bytes, write_errors, resample_errors, queue_drops, reconnects;
	double left_peak, right_peak;
	uint64_t diagnostic_sequence, last_health_ns, last_source_metadata_ns;
	char health_ring[16][2048];
	unsigned health_read, health_count;
	bool conflict_paused;
	bool terminal_paused, waiting_audio, waiting_primary;
	uint64_t retry_after_audio_ns, last_usable_audio_ns;
	bool user_paused; /* protected by qlock; not saved to settings */
    unsigned stop_phase; /* 0=none, 1=send intent, 2=graceful close; qlock */
    uint64_t stop_deadline_ns;


	/* audio conversion */
	audio_resampler_t *resampler;
	uint32_t sample_rate;
	bool sending_stereo; /* what the resampler is actually producing */
	/* Set from the UI thread, acted on by the audio thread. The resampler must only
	   ever be destroyed/created on the thread that calls audio_resampler_resample(),
	   otherwise toggling the checkbox mid-stream frees it under the audio callback. */
	volatile bool resampler_dirty;

	/* websocket service thread */
	pthread_t thread;
	atomic_bool stop;
	volatile bool connected;
    atomic_bool audio_started;
	struct lws_context *lws_ctx;
	struct lws *wsi;
    lws_sorted_usec_list_t service_pulse;

	/* outbound queue (audio thread -> ws thread) */
	pthread_mutex_t qlock;
	struct st_chunk *qhead, *qtail;
	int qcount;
};

#include "native-cc-output.h"

/* UI work must run after the current Qt event, never on the audio/WS thread.
 * A weak source reference also makes queued work safe after filter removal. */
struct st_status_refresh {
    obs_weak_source_t *source;
    char text[320];
};
static void st_refresh_status_ui(void *data)
{
    struct st_status_refresh *refresh = data;
    obs_source_t *source = obs_weak_source_get_source(refresh->source);
    if (source) {
        obs_data_t *settings = obs_source_get_settings(source);
        obs_data_set_string(settings, "status_text", refresh->text);
        obs_data_release(settings);
        obs_source_update_properties(source);
        obs_source_release(source);
    }
    obs_weak_source_release(refresh->source);
    bfree(refresh);
}
static void st_set_status(struct st_filter *f, const char *fmt, ...)
{
    char text[sizeof(f->status)];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&f->qlock);
    bool changed = strcmp(f->status, text) != 0;
    snprintf(f->status, sizeof(f->status), "%s", text);
    pthread_mutex_unlock(&f->qlock);
    if (!changed) return;
    blog(LOG_INFO, "[streamtranslate] %s", text);
    pthread_mutex_lock(&f->qlock);
    f->status_dirty = true;
    pthread_mutex_unlock(&f->qlock);
}

/* Called ONLY by the network worker. OBS uses Qt::AutoConnection for UI tasks:
 * queueing from a button handler would execute inline and destroy that button's
 * WidgetInfo while OBS is still inside ButtonClicked. */
static void st_publish_status(struct st_filter *f)
{
    pthread_mutex_lock(&f->qlock);
    if (!f->status_dirty || !f->context) { pthread_mutex_unlock(&f->qlock); return; }
    f->status_dirty = false;
    struct st_status_refresh *refresh = bzalloc(sizeof(*refresh));
    snprintf(refresh->text, sizeof(refresh->text), "%s", f->status);
    pthread_mutex_unlock(&f->qlock);
    refresh->source = obs_source_get_weak_source(f->context);
    obs_queue_task(OBS_TASK_UI, st_refresh_status_ui, refresh, false);
}

/* Numeric metadata only; this bounded ring survives network interruptions.
 * OBS's own log also keeps the snapshots when the transport cannot upload. */
static void st_health_snapshot(struct st_filter *f)
{
    uint64_t now=os_gettime_ns();
    if (f->last_health_ns && now-f->last_health_ns<15000000000ULL) return;
    f->last_health_ns=now;
    obs_source_t *parent=obs_filter_get_parent(f->context);
    bool muted=parent && obs_source_muted(parent);
    bool enabled=obs_source_enabled(f->context);
    bool active=parent && obs_source_active(parent);
    if(f->health_count==16){f->health_read=(f->health_read+1)%16;f->health_count--;}
    unsigned slot=(f->health_read+f->health_count)%16;
    pthread_mutex_lock(&f->qlock);
    snprintf(f->health_ring[slot],sizeof(f->health_ring[slot]),
      "{\"type\":\"audio_health\",\"plugin_version\":\"%s\",\"filter_id\":\"%s\",\"obs_version\":\"%s\",\"platform\":\"%s\",\"uptime_ms\":%" PRIu64 ",\"sequence\":%" PRIu64
      ",\"callbacks\":%" PRIu64 ",\"frames\":%" PRIu64 ",\"captured\":%d,\"sent\":%d,\"written_bytes\":%" PRIu64
      ",\"write_errors\":%" PRIu64 ",\"resample_errors\":%" PRIu64 ",\"queue_drops\":%" PRIu64 ",\"queue_depth\":%d"
      ",\"last_callback_age_ms\":%" PRIu64 ",\"last_send_age_ms\":%" PRIu64 ",\"sample_rate\":%u,\"reconnects\":%" PRIu64
      ",\"left_peak\":%.6f,\"right_peak\":%.6f,\"muted\":%s,\"enabled\":%s,\"source_active\":%s,\"connected\":%s}",
      ST_PLUGIN_VERSION,f->filter_id,obs_get_version_string(),ST_PLATFORM,now/1000000,++f->diagnostic_sequence,f->callbacks,f->input_frames,f->audio_chunks_captured,f->audio_chunks_sent,f->written_bytes,
      f->write_errors,f->resample_errors,f->queue_drops,f->qcount,
      f->last_callback_ns?(now-f->last_callback_ns)/1000000:0,f->last_send_ns?(now-f->last_send_ns)/1000000:0,f->sample_rate,f->reconnects,
      f->left_peak,f->right_peak,muted?"true":"false",enabled?"true":"false",active?"true":"false",f->connected?"true":"false");
    pthread_mutex_unlock(&f->qlock);
    blog(LOG_INFO,"[streamtranslate-health] %s",f->health_ring[slot]);
    f->health_count++;
}

static bool st_user_paused(struct st_filter *f)
{
    pthread_mutex_lock(&f->qlock);
    bool paused = f->user_paused;
    pthread_mutex_unlock(&f->qlock);
    return paused;
}

/* ---------------- queue ---------------- */

static void st_queue_clear(struct st_filter *f)
{
	pthread_mutex_lock(&f->qlock);
	struct st_chunk *c = f->qhead;
	while (c) {
		struct st_chunk *n = c->next;
		free(c->buf);
		free(c);
		c = n;
	}
	f->qhead = f->qtail = NULL;
	f->qcount = 0;
	pthread_mutex_unlock(&f->qlock);
}

static void st_queue_push(struct st_filter *f, const unsigned char *data, size_t len)
{
	struct st_chunk *c = malloc(sizeof(*c));
	if (!c)
		return;
	c->buf = malloc(LWS_PRE + len);
	if (!c->buf) {
		free(c);
		return;
	}
	memcpy(c->buf + LWS_PRE, data, len);
	c->len = len;
    c->captured_ns = os_gettime_ns();
	c->next = NULL;

	pthread_mutex_lock(&f->qlock);
    if (f->user_paused || f->terminal_paused || f->conflict_paused) {
        pthread_mutex_unlock(&f->qlock);
        free(c->buf); free(c); return;
    }
	while (f->qhead && (f->qcount >= ST_QUEUE_MAX ||
        c->captured_ns - f->qhead->captured_ns > ST_QUEUE_MAX_AGE_NS)) {
		struct st_chunk *old = f->qhead;
		f->qhead = old->next;
		if (!f->qhead)
			f->qtail = NULL;
		f->qcount--;
		f->queue_drops++;
		free(old->buf);
		free(old);
	}
	if (f->qtail)
		f->qtail->next = c;
	else
		f->qhead = c;
	f->qtail = c;
	f->qcount++;
	pthread_mutex_unlock(&f->qlock);
}

static struct st_chunk *st_queue_pop(struct st_filter *f)
{
	pthread_mutex_lock(&f->qlock);
    const uint64_t now = os_gettime_ns();
    while (f->qhead && now - f->qhead->captured_ns > ST_QUEUE_MAX_AGE_NS) {
        struct st_chunk *old = f->qhead;
        f->qhead = old->next; f->qcount--; f->queue_drops++;
        if (!f->qhead) f->qtail = NULL;
        free(old->buf); free(old);
    }
	struct st_chunk *c = f->qhead;
	if (c) {
		f->qhead = c->next;
		if (!f->qhead)
			f->qtail = NULL;
		f->qcount--;
	}
	pthread_mutex_unlock(&f->qlock);
	return c;
}

/* ---------------- websocket ---------------- */

static const char *st_setup_close_message(const char *reason)
{
    if (!strncmp(reason,"Receiver already supplies",25)) return "Mic 1 uses a stereo receiver. Use that one filter, or switch Mic 1 to Two separate OBS sources first.";
    if (strstr(reason,"native CC")) return "Two speakers requires native Twitch CC off. Check streamtranslate.live, then press Reconnect.";
    if (strstr(reason,"settings") || strstr(reason,"admission changed")) return "Settings changed while connecting. Finish setup at streamtranslate.live, then press Reconnect.";
    if (!strcmp(reason,"Streamer is not live")) return "Waiting for Mic 1. Start its filter with the same Plugin Key; Mic 2 reconnects automatically.";
    return "Audio setup changed. Check both filters and Two speakers at streamtranslate.live, then press Reconnect.";
}

static int st_ws_callback(struct lws *wsi, enum lws_callback_reasons reason,
			  void *user, void *in, size_t len)
{
	struct st_filter *f = lws_context_user(lws_get_context(wsi));
	(void)user;
	(void)in;
	(void)len;
	if (!f)
		return 0;

	switch (reason) {
	case LWS_CALLBACK_CLIENT_ESTABLISHED:
		if (!st_user_paused(f)) st_set_status(f, "Connected — waiting for audio");
        f->audio_started = false;
        f->waiting_primary = false;
		f->connected = true;
        memset(&f->cc_state,0,sizeof(f->cc_state));
        memset(&f->cc_fragments,0,sizeof(f->cc_fragments));
        f->cc_ack_count=f->cc_ack_read=0;f->cc_ack_last=false;
		f->last_health_ns=0; f->last_source_metadata_ns=0;
		st_health_snapshot(f);
		lws_callback_on_writable(wsi);
		break;

	case LWS_CALLBACK_CLIENT_RECEIVE: {
        const char *message=st_cc_fragment(&f->cc_fragments,in,len,lws_is_first_fragment(wsi),lws_is_final_fragment(wsi)&&!lws_remaining_packet_payload(wsi),lws_frame_is_binary(wsi));
        if(message)st_cc_receive(f,message);
        break;
    }
	case LWS_CALLBACK_CLIENT_WRITEABLE: {
        // Stop has priority over health/PCM. All socket operations stay on this
        // service thread; the UI only marks intent and cancels the service wait.
        pthread_mutex_lock(&f->qlock);
        unsigned phase = f->stop_phase;
        bool paused = f->user_paused;
        if (phase == 1 && paused) {
            static const char intent[] = "{\"type\":\"capture_stop\",\"v\":1,\"reason\":\"stop_button\"}";
            unsigned char payload[LWS_PRE + sizeof(intent)];
            memcpy(payload + LWS_PRE, intent, sizeof(intent) - 1);
            int wrote = lws_write(wsi, payload + LWS_PRE, sizeof(intent) - 1, LWS_WRITE_TEXT);
            f->stop_phase = 2;
            pthread_mutex_unlock(&f->qlock);
            if (wrote != (int)(sizeof(intent) - 1)) return -1;
            lws_callback_on_writable(wsi);
            return 0;
        }
        if (phase == 2 && paused) {
            f->stop_phase = 0;
            pthread_mutex_unlock(&f->qlock);
            unsigned char reason[] = "capture_stopped";
            lws_close_reason(wsi, LWS_CLOSE_STATUS_NORMAL, reason, sizeof(reason) - 1);
            return -1;
        }
        pthread_mutex_unlock(&f->qlock);
        if (paused) { st_queue_clear(f); return 0; }
        /* Alternate ACK/audio writes when both queues have work: neither can
         * starve the other, and the existing capture-stop check stays first. */
        pthread_mutex_lock(&f->qlock);bool cc_audio_pending=f->qcount>0;pthread_mutex_unlock(&f->qlock);
        if(f->cc_ack_count && (!f->cc_ack_last || !cc_audio_pending)) {
            unsigned char payload[LWS_PRE+256];const char *ack=f->cc_acks[f->cc_ack_read];size_t size=strlen(ack);
            memcpy(payload+LWS_PRE,ack,size);
            int wrote=lws_write(wsi,payload+LWS_PRE,size,LWS_WRITE_TEXT);
            if(wrote!=(int)size)return -1;
            f->cc_ack_read=(f->cc_ack_read+1)%8;f->cc_ack_count--;f->cc_ack_last=true;
            lws_callback_on_writable(wsi);return 0;
        }
        /* Publish the parent source, not the filter name. Control derives its
         * meters from the very same PCM frames; metadata alone is not audio. */
        uint64_t metadata_now=os_gettime_ns();
        if(!f->last_source_metadata_ns || metadata_now-f->last_source_metadata_ns>=1000000000ULL){
            f->last_source_metadata_ns=metadata_now;
            obs_source_t *parent=obs_filter_get_parent(f->context);
            obs_data_t *metadata=obs_data_create();
            obs_data_set_string(metadata,"type","obs_source");obs_data_set_int(metadata,"v",1);
            obs_data_set_string(metadata,"name",parent?obs_source_get_name(parent):"OBS audio source");
            obs_data_set_string(metadata,"sourceId",parent?obs_source_get_uuid(parent):"");
            obs_data_set_bool(metadata,"canSelect",false);
            const char *json=obs_data_get_json(metadata);size_t size=strlen(json);
            unsigned char payload[LWS_PRE+2048];
            int wrote=-1;
            if(size<=2048){memcpy(payload+LWS_PRE,json,size);wrote=lws_write(wsi,payload+LWS_PRE,size,LWS_WRITE_TEXT);}
            obs_data_release(metadata);
            if(wrote!=(int)size)return -1;
            lws_callback_on_writable(wsi);return 0;
        }
        if(f->health_count){
            unsigned char payload[LWS_PRE+2048];
            const char *text=f->health_ring[f->health_read];size_t size=strlen(text);
            memcpy(payload+LWS_PRE,text,size);
            int wrote=lws_write(wsi,payload+LWS_PRE,size,LWS_WRITE_TEXT);
            if(wrote!=(int)size)return -1;
            f->health_read=(f->health_read+1)%16;f->health_count--;
            lws_callback_on_writable(wsi);return 0;
        }

		obs_source_t *parent = obs_filter_get_parent(f->context);
        if (!obs_source_enabled(f->context) || (parent && obs_source_muted(parent))) {
            st_queue_clear(f);
            return 0;
        }
        struct st_chunk *c = st_queue_pop(f);
		if (c) {
            // Serialize the final write check with Stop. A chunk popped just
            // before Stop cannot escape after the button handler returns.
            f->cc_ack_last=false;
            pthread_mutex_lock(&f->qlock);
            if (f->user_paused) {
                pthread_mutex_unlock(&f->qlock);
                free(c->buf); free(c);
                lws_callback_on_writable(wsi);
                return 0;
            }
			int wrote = lws_write(wsi, c->buf + LWS_PRE, c->len, LWS_WRITE_BINARY);
            if(wrote==(int)c->len){f->audio_chunks_sent++;f->written_bytes+=wrote;f->last_send_ns=os_gettime_ns();}
            else f->write_errors++;
            pthread_mutex_unlock(&f->qlock);
            bool write_failed=wrote!=(int)c->len;
			free(c->buf);
			free(c);
            if(write_failed)return -1;
            if(!f->audio_started){f->audio_started=true;st_set_status(f,"Connected — sending audio · %s",f->sending_stereo?"Mic 1 left / Mic 2 right":f->speaker==1?"Mic 2":"Mic 1");}
			lws_callback_on_writable(wsi);
		}
		break;
	}

	case LWS_CALLBACK_EVENT_WAIT_CANCELLED:
		if (f->wsi && f->connected)
			lws_callback_on_writable(f->wsi);
		break;

	case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        pthread_mutex_lock(&f->qlock);
        f->stop_phase = 0; f->stop_deadline_ns = 0;
        pthread_mutex_unlock(&f->qlock);
		if (!st_user_paused(f)) st_set_status(f, "Connection FAILED: %s (server: %s) - retrying",
			      in ? (const char *)in : "unknown error", f->server);
		f->connected = false;
		f->wsi = NULL;
		break;

    case LWS_CALLBACK_WS_PEER_INITIATED_CLOSE:
        if(len>=2){const unsigned char *p=in;unsigned code=(p[0]<<8)|p[1]; f->last_close_code=code;
            blog(LOG_INFO,"[streamtranslate] Server close code: %u",code);
            char reason[160]={0};size_t reason_len=len-2<sizeof(reason)-1?len-2:sizeof(reason)-1;memcpy(reason,p+2,reason_len);
            bool primary_pending=f->speaker==1 && code==4409 && !strcmp(reason,"Streamer is not live");
            f->waiting_primary=primary_pending;
            if(primary_pending) st_set_status(f,"%s",st_setup_close_message(reason));
            bool changed=code==4409 && (!strcmp(reason,"Stream control changed; reconnect") || !strcmp(reason,"Streamer input changed; reconnect"));
            if(st_close_policy(code)==ST_RETRY_MANUAL && code!=4001 && code!=4009 && !changed && !primary_pending){
                pthread_mutex_lock(&f->qlock);f->terminal_paused=true;pthread_mutex_unlock(&f->qlock);
                st_set_status(f,code==4008 ? "Stopped after idle timeout. Press Start sending audio to start again." :
                    code==4409 ? st_setup_close_message(reason) :
                    code==4403 ? "Plugin key rejected. Update your Plugin Key, then press Reconnect." :
                    "Session stopped by server. Check your account, then press Reconnect.");
            }
            if(st_close_policy(code)==ST_RETRY_AUDIO){
                pthread_mutex_lock(&f->qlock);f->waiting_audio=true;f->retry_after_audio_ns=os_gettime_ns();pthread_mutex_unlock(&f->qlock);
                st_set_status(f,"No audio received. Waiting for the OBS source to send audio before retrying.");
            }
            if(code==4001 || code==4009){pthread_mutex_lock(&f->qlock);f->conflict_paused=true;pthread_mutex_unlock(&f->qlock);st_set_status(f,"Another filter already sends this mic. Adding a second mic? Choose Two speakers > Two separate OBS sources > Mic 2. To replace a mic, stop its old filter and start sending audio here.");}
        }
        break;
	case LWS_CALLBACK_CLIENT_CLOSED:
        pthread_mutex_lock(&f->qlock);
        f->stop_phase = 0; f->stop_deadline_ns = 0;
        pthread_mutex_unlock(&f->qlock);
        st_queue_clear(f);
		if (st_user_paused(f)) st_set_status(f, "Paused - not sending audio. Press Start sending audio to resume.");
        else if(!f->conflict_paused && !f->terminal_paused && !f->waiting_audio && !f->waiting_primary) st_set_status(f, "Disconnected from %s - reconnecting", f->server);
		f->connected = false;
		f->wsi = NULL;
		break;

	default:
		break;
	}
	return 0;
}

static const struct lws_protocols st_protocols[] = {
	{"st-audio", st_ws_callback, 0, 4096, 0, NULL, 0},
	LWS_PROTOCOL_LIST_TERM,
};

static void st_connect(struct st_filter *f)
{
	if (!f->lws_ctx || f->wsi)
		return;
	if (!f->plugin_key[0]) {
		st_set_status(f, "Not configured - paste your Plugin Key above");
		return;
	}

	char path[768], capture_id[48];
    pthread_mutex_lock(&f->qlock);snprintf(capture_id,sizeof(capture_id),"%s",f->capture_id);pthread_mutex_unlock(&f->qlock);
	snprintf(path, sizeof(path), "/audio?pluginKey=%s&rate=%u&tabId=%s&captureId=%s&transportVersion=%s&cfgSpeakers=%d%s%s%s",
         f->plugin_key, f->sample_rate,f->filter_id,capture_id,ST_PLUGIN_VERSION,
         f->two_speakers ? 2 : 1,
         f->sending_stereo ? "&channels=2&cfgInput=receiver" : "&cfgInput=separate",
         f->speaker == 1 ? "&speaker=1" : "",
         f->native_twitch_cc && f->speaker==0 ? "&native_cc=1&native_cc_interim=1" : "");
    pthread_mutex_lock(&f->qlock);f->reconnects++;pthread_mutex_unlock(&f->qlock);

	struct lws_client_connect_info ci;
	memset(&ci, 0, sizeof(ci));
	ci.context = f->lws_ctx;
	ci.address = f->server;
	ci.port = 443;
	ci.path = path;
	ci.host = f->server;
	ci.origin = f->server;
	ci.ssl_connection = LCCSCF_USE_SSL;
	ci.protocol = NULL; /* server doesn't negotiate a subprotocol */
	ci.local_protocol_name = "st-audio";

	st_set_status(f, "Connecting…");
	f->wsi = lws_client_connect_via_info(&ci);
	if (!f->wsi)
		st_set_status(f, "Could not start connection to %s - check the Server field", f->server);
}

/* TLS trust: the bundled OpenSSL has no OS certificate store, so server
 * verification fails silently without a CA file. We ship Mozilla's CA bundle
 * (cacert.pem) inside the plugin and point lws at it. Resolved relative to
 * this module's own binary location. */
static const char *st_ca_bundle_path(void)
{
	static char path[1024];
	static int resolved = 0;
	if (resolved)
		return path[0] ? path : NULL;
	resolved = 1;
	path[0] = 0;
#ifdef _WIN32
	HMODULE hm = NULL;
	if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
				       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			       (LPCSTR)&st_ca_bundle_path, &hm)) {
		char mod[1024];
        DWORD length = GetModuleFileNameA(hm, mod, sizeof(mod));
        if (length > 0 && length < sizeof(mod)) {
			char *slash = strrchr(mod, '\\');
			if (slash) {
				*slash = 0;
				st_checked_path_join(path, sizeof(path), mod, "\\cacert.pem");
			}
		}
	}
#else
    Dl_info dli;
    bool rejected_path = false;
    if (dladdr((void *)&st_ca_bundle_path, &dli) && dli.dli_fname) {
        char mod[1024];
        if (!st_checked_path_join(mod, sizeof(mod), dli.dli_fname, "")) {
            rejected_path = true;
        } else {
            char *slash = strrchr(mod, '/');
            if (slash) {
                *slash = 0;
                /* macOS bundle: .../Contents/MacOS -> .../Contents/Resources/cacert.pem */
                char *macos = strstr(mod, "/Contents/MacOS");
                if (macos) {
                    *macos = 0;
                    rejected_path = !st_checked_path_join(path, sizeof(path), mod,
                                                         "/Contents/Resources/cacert.pem");
                } else {
                    rejected_path = !st_checked_path_join(path, sizeof(path), mod, "/cacert.pem");
                }
            }
        }
    }
    if (rejected_path || (path[0] && access(path, R_OK) != 0)) {
        /* Keep the existing readable canonical Linux store; never use a
         * truncated candidate or relax certificate verification. */
        if (access("/etc/ssl/certs/ca-certificates.crt", R_OK) == 0)
            st_checked_path_join(path, sizeof(path), "/etc/ssl/certs/ca-certificates.crt", "");
        else
            path[0] = 0;
    }
#endif
	if (path[0])
		blog(LOG_INFO, "[streamtranslate] CA bundle: %s", path);
	else
		blog(LOG_WARNING, "[streamtranslate] no CA bundle found — TLS verification may fail");
	return path[0] ? path : NULL;
}

#include "update-check.h"
#include "connection-test.h"

static void st_service_pulse(lws_sorted_usec_list_t *sul)
{
    struct st_filter *f=lws_container_of(sul,struct st_filter,service_pulse);
    if(!f->stop) lws_sul_schedule(f->lws_ctx,0,&f->service_pulse,st_service_pulse,LWS_US_PER_SEC);
}

static void *st_ws_thread(void *arg)
{
	struct st_filter *f = arg;
	uint64_t next_retry = 0;

	struct lws_context_creation_info info;
	memset(&info, 0, sizeof(info));
	info.port = CONTEXT_PORT_NO_LISTEN;
	info.protocols = st_protocols;
	info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
	info.user = f;
	info.client_ssl_ca_filepath = st_ca_bundle_path();

	f->lws_ctx = lws_create_context(&info);
	if (!f->lws_ctx) {
		blog(LOG_ERROR, "[streamtranslate] lws context creation failed");
		return NULL;
	}

    lws_sul_schedule(f->lws_ctx,0,&f->service_pulse,st_service_pulse,LWS_US_PER_SEC);

	while (!f->stop) {
		st_health_snapshot(f);
		pthread_mutex_lock(&f->qlock);
        if(f->waiting_audio && st_audio_can_retry(f->last_usable_audio_ns,f->retry_after_audio_ns,os_gettime_ns())) f->waiting_audio=false;
        bool waiting_audio=f->waiting_audio;
        pthread_mutex_unlock(&f->qlock);
        if (!f->wsi && !f->conflict_paused && !st_user_paused(f) && !f->terminal_paused && !waiting_audio && !f->resampler_dirty) {
			uint64_t now = os_gettime_ns();
			if (now >= next_retry) {
				st_connect(f);
				next_retry = now + 3000000000ULL; /* 3s backoff */
			}
		}
		/* Ask for a writeable slot from THIS thread whenever audio is waiting.
		 * (Relying on lws_cancel_service from the audio thread proved unreliable:
		 * the socket connected but never sent a byte, so the server's watchdog
		 * closed the silent connection after ~30s.) */
        pthread_mutex_lock(&f->qlock);
        int pending = f->qcount;
        unsigned stop_phase = f->stop_phase;
        uint64_t deadline = f->stop_deadline_ns;
        pthread_mutex_unlock(&f->qlock);
        if (f->wsi && deadline && os_gettime_ns() >= deadline) {
            // Includes an unfinished handshake: Stop must remain bounded even
            // when the peer never becomes writeable. No receipt is claimed.
            lws_set_timeout(f->wsi, PENDING_TIMEOUT_CLOSE_SEND, LWS_TO_KILL_ASYNC);
        } else if (f->wsi && f->connected && (pending > 0 || f->health_count > 0 || f->cc_ack_count || stop_phase)) {
            lws_callback_on_writable(f->wsi);
        }
		lws_service(f->lws_ctx, 20);
        // Packet counters remain in audio_health diagnostics. Rebuilding all
        // Qt properties every two seconds closes open source/role dropdowns.
        // Only connection-state changes may rebuild this form.
        st_publish_status(f);


	}

    lws_sul_cancel(&f->service_pulse);
    /* The poll backend ignores lws_service's nominal timeout. Once the pulse
     * is cancelled, another service call can sleep indefinitely after closing
     * the last socket. Destroy on this owner thread; it closes active and
     * unfinished connections while f and its mutex are still alive. */
	lws_context_destroy(f->lws_ctx);
	f->lws_ctx = NULL;
	f->wsi = NULL;
	f->connected = false;
	return NULL;
}

/* ---------------- OBS filter ---------------- */

static const char *st_get_name(void *unused)
{
	(void)unused;
	return "StreamTranslate (live translated captions)";
}

static void st_build_resampler(struct st_filter *f)
{
	if (f->resampler) {
		audio_resampler_destroy(f->resampler);
		f->resampler = NULL;
	}
	const struct audio_output_info *aoi = audio_output_get_info(obs_get_audio());
	if (!aoi)
		return;
	f->sample_rate = aoi->samples_per_sec;

	struct resample_info src = {
		.samples_per_sec = aoi->samples_per_sec,
		.format = AUDIO_FORMAT_FLOAT_PLANAR,
		.speakers = aoi->speakers,
	};
	/* Only keep two channels when the user asked for it AND the source really has
	   them -- a mono source forced to stereo would just duplicate one voice onto
	   both captions. Falls back to mono silently in that case. */
	bool want_stereo = f->stereo_split && get_audio_channels(aoi->speakers) >= 2;
	f->sending_stereo = want_stereo;
	struct resample_info dst = {
		.samples_per_sec = aoi->samples_per_sec,
		.format = AUDIO_FORMAT_16BIT,
		.speakers = want_stereo ? SPEAKERS_STEREO : SPEAKERS_MONO,
	};
	f->resampler = audio_resampler_create(&dst, &src);
    if (!f->resampler) { pthread_mutex_lock(&f->qlock);f->resample_errors++;pthread_mutex_unlock(&f->qlock);blog(LOG_ERROR, "[streamtranslate] resampler creation failed"); }
}

static void st_update(void *data, obs_data_t *settings)
{
	struct st_filter *f = data;
	const char *server = obs_data_get_string(settings, "server");
	const char *key = obs_data_get_string(settings, "plugin_key");

	/* mic_mode: 0 = Mic 1, 1 = Mic 2, 2 = stereo pair (left = Mic 1, right = Mic 2) */
    /* OBS may invoke update before the property's modified callback. Derive
     * the transport mode here too so the selection is never one click behind. */
    if (obs_data_has_user_value(settings, "speaker_count")) {
        bool two = obs_data_get_int(settings, "speaker_count") == 2;
        bool separate = two && obs_data_get_int(settings, "mic_wiring") == 1;
        bool second = separate && obs_data_get_int(settings, "source_role") == 1;
        obs_data_set_int(settings, "mic_mode", !two ? 0 : !separate ? 2 : second ? 1 : 0);
    }
	long long mic_mode = obs_data_get_int(settings, "mic_mode");
	bool stereo = mic_mode == 2;
    bool two = obs_data_get_int(settings, "speaker_count") == 2 || mic_mode != 0;
    if (two) obs_data_set_bool(settings, "native_twitch_cc", false);
    bool native_cc = !two && obs_data_get_bool(settings, "native_twitch_cc");
	int speaker = mic_mode == 1 ? 1 : 0;

	bool changed = strcmp(f->server, server ? server : "") != 0 ||
		       strcmp(f->plugin_key, key ? key : "") != 0 ||
		       stereo != f->stereo_split ||
		       speaker != f->speaker || native_cc != f->native_twitch_cc || two != f->two_speakers;
	f->speaker = speaker;
    f->native_twitch_cc=native_cc;
    f->two_speakers=two;

	snprintf(f->server, sizeof(f->server), "%s", server ? server : "");
	snprintf(f->plugin_key, sizeof(f->plugin_key), "%s", key ? key : "");

	if (stereo != f->stereo_split) {
		/* Channel layout is baked into the resampler and announced in the connect
		   URL. Do NOT rebuild here -- this runs on the UI thread. Flag it and let
		   the audio thread swap the resampler between frames; the reconnect below
		   then re-announces the new channel count. */
		f->stereo_split = stereo;
		f->resampler_dirty = true;
	}

	if (changed) {
        f->terminal_paused=false;
        f->last_close_code=0;f->waiting_primary=false;f->conflict_paused=false;
        pthread_mutex_lock(&f->qlock);f->waiting_audio=false;pthread_mutex_unlock(&f->qlock);
    }
    if (changed) st_queue_clear(f);
    if (changed && f->wsi) {
		/* reconnect with new credentials: close current, thread will redial */
		lws_set_timeout(f->wsi, PENDING_TIMEOUT_CLOSE_SEND, LWS_TO_KILL_ASYNC);
		lws_cancel_service(f->lws_ctx);
	}
}

static void *st_create(obs_data_t *settings, obs_source_t *context)
{
	struct st_filter *f = bzalloc(sizeof(struct st_filter));
	f->context = context;
    atomic_init(&f->native_twitch_cc,false);
    atomic_init(&f->two_speakers,false);
    atomic_init(&f->audio_started,false);
    atomic_init(&f->stop,false);
    snprintf(f->filter_id,sizeof(f->filter_id),"filter-%" PRIx64,os_gettime_ns());
	pthread_mutex_init(&f->qlock, NULL);
    pthread_cond_init(&f->test_ready, NULL);
    char *capture_uuid=os_generate_uuid();
    snprintf(f->capture_id,sizeof(f->capture_id),"%s",capture_uuid);bfree(capture_uuid);
	st_update(f, settings);
	st_build_resampler(f);
    f->resampler_dirty = false;
	f->stop = false;
	pthread_create(&f->thread, NULL, st_ws_thread, f);
	return f;
}

static void st_destroy(void *data)
{
	struct st_filter *f = data;
	f->stop = true;
	if (f->lws_ctx)
		lws_cancel_service(f->lws_ctx);
	st_connection_test_stop(f);
	pthread_join(f->thread, NULL);
	st_queue_clear(f);
	pthread_cond_destroy(&f->test_ready);
	pthread_mutex_destroy(&f->qlock);
	if (f->resampler)
		audio_resampler_destroy(f->resampler);
	bfree(f);
}

static struct obs_audio_data *st_filter_audio(void *data, struct obs_audio_data *audio)
{
	struct st_filter *f = data;
    pthread_mutex_lock(&f->qlock);
    f->callbacks++;f->last_callback_ns=os_gettime_ns();
    if(audio)f->input_frames+=audio->frames;
    f->left_peak=0;f->right_peak=0;
    if(audio){for(size_t ch=0;ch<2;ch++){if(!audio->data[ch])continue;double peak=0;const float *samples=(const float*)audio->data[ch];for(uint32_t i=0;i<audio->frames;i++){double v=fabs(samples[i]);if(isfinite(v)&&v>peak)peak=v;}if(ch==0)f->left_peak=peak;else f->right_peak=peak;}}
    pthread_mutex_unlock(&f->qlock);
    if (!audio || !audio->frames || st_user_paused(f)) return audio;

	/* OBS applies a source's mute AFTER its filter chain, so a muted mic still
	 * reaches us. Without this check a streamer who mutes for a private moment
	 * would still be transcribed and captioned on stream. Also honour the filter's
	 * own enabled toggle (the eye icon) as a pause control. */
	obs_source_t *parent = obs_filter_get_parent(f->context);
	bool blocked = (parent && obs_source_muted(parent)) || !obs_source_enabled(f->context);
	if (blocked) {
		if (!f->muted_now) {
			f->muted_now = true;
            st_queue_clear(f);
			st_set_status(f, "Muted in OBS — audio is not being sent");
            f->audio_started=false;
		}
		return audio;
	}
	if (f->muted_now)
		f->muted_now = false;
	if (f->resampler_dirty) {
		/* Owned by this thread -- safe to destroy and recreate here. */
		st_build_resampler(f);
		f->resampler_dirty = false;
	}
	if (!f->resampler) {
		st_build_resampler(f); /* audio subsystem may not have been ready at create time */
		if (!f->resampler)
			return audio;
	}


	uint8_t *out[MAX_AV_PLANES] = {0};
	uint32_t out_frames = 0;
	uint64_t ts_offset = 0;

	if (audio_resampler_resample(f->resampler, out, &out_frames, &ts_offset,
				     (const uint8_t *const *)audio->data,
				     audio->frames) &&
	    out_frames > 0 && out[0]) {
		/* A callback alone may be silence. Use the same PCM RMS floor as the
         * server, after conversion, to wake a no-audio admission pause. */
        const int16_t *pcm=(const int16_t *)out[0];double energy=0;
        for(uint32_t i=0;i<out_frames;i++) energy+=(double)pcm[i]*pcm[i];
        if(sqrt(energy/out_frames)>=180){
            pthread_mutex_lock(&f->qlock);f->last_usable_audio_ns=os_gettime_ns();pthread_mutex_unlock(&f->qlock);
        }
        // Preserve the opening syllables while a same-capture transport wakes
        // or handshakes. Terminal/conflict/explicit Stop never buffers audio.
        // Do not fill this bounded queue with idle silence before voice resumes.
        pthread_mutex_lock(&f->qlock);
        uint64_t now = os_gettime_ns();
        bool capture_allowed = !f->user_paused && !f->terminal_paused && !f->conflict_paused;
        bool fresh_voice = f->last_usable_audio_ns > f->retry_after_audio_ns
            && now - f->last_usable_audio_ns <= ST_QUEUE_MAX_AGE_NS;
        pthread_mutex_unlock(&f->qlock);
        if (!capture_allowed || (!f->connected && !fresh_voice)) return audio;
        st_queue_push(f, out[0], (size_t)out_frames * 2 /* s16 mono */);
		pthread_mutex_lock(&f->qlock);f->audio_chunks_captured++;pthread_mutex_unlock(&f->qlock);
		if (f->lws_ctx)
			lws_cancel_service(f->lws_ctx); /* wake ws thread to flush */
    } else { pthread_mutex_lock(&f->qlock);f->resample_errors++;pthread_mutex_unlock(&f->qlock); }
	return audio;
}

static bool st_stop_clicked(obs_properties_t *props, obs_property_t *prop, void *data)
{
	struct st_filter *f = data;
	(void)props;
	(void)prop;
	if (!f)
		return false;
	pthread_mutex_lock(&f->qlock);
    bool was_paused = f->user_paused;
    f->user_paused = true;
    if (!was_paused) {
        f->stop_phase = 1;
        f->stop_deadline_ns = os_gettime_ns() + 2000000000ULL;
    }
    pthread_mutex_unlock(&f->qlock);
    st_queue_clear(f);
    st_set_status(f, "Paused - not sending audio. Press Start sending audio to resume.");
	if (f->lws_ctx)
		lws_cancel_service(f->lws_ctx);
	return false;
}

static bool st_reconnect_clicked(obs_properties_t *props, obs_property_t *prop, void *data)
{
	struct st_filter *f = data;
	(void)props;
	(void)prop;
	if (!f)
		return false;
    pthread_mutex_lock(&f->qlock);
    if(f->user_paused || f->conflict_paused || f->terminal_paused){
        char *capture_uuid=os_generate_uuid();
        snprintf(f->capture_id,sizeof(f->capture_id),"%s",capture_uuid);bfree(capture_uuid);
    }
    pthread_mutex_unlock(&f->qlock);
	pthread_mutex_lock(&f->qlock);f->conflict_paused=false;
    f->terminal_paused=false;pthread_mutex_unlock(&f->qlock);
    f->last_close_code=0;f->waiting_primary=false;
    pthread_mutex_lock(&f->qlock);f->waiting_audio=false;pthread_mutex_unlock(&f->qlock);
    pthread_mutex_lock(&f->qlock);
    f->user_paused=false; f->stop_phase=0; f->stop_deadline_ns=0;
    pthread_mutex_unlock(&f->qlock);
	st_set_status(f, "Reconnecting ...");
	if (f->wsi) {
		lws_set_timeout(f->wsi, PENDING_TIMEOUT_CLOSE_SEND, LWS_TO_KILL_ASYNC);
	}
	if (f->lws_ctx)
		lws_cancel_service(f->lws_ctx);
	return false;
}

#ifdef __APPLE__
bool st_copy_plugin_key_to_clipboard(const char *key);

static bool st_copy_plugin_key_clicked(obs_properties_t *props, obs_property_t *prop, void *data)
{
    (void)props;
    struct st_filter *filter = data;
    if (!filter || !filter->context)
        return false;

    /* OBS's filter properties editor writes text edits into this settings
     * object immediately, before its deferred undo/update callback. */
    obs_data_t *settings = obs_source_get_settings(filter->context);
    const char *key = obs_data_get_string(settings, "plugin_key");
    const char *label = "Enter a plugin key first";
    if (key && *key)
        label = st_copy_plugin_key_to_clipboard(key) ? "Plugin key copied" : "Could not copy plugin key";
    obs_data_release(settings);

    obs_property_set_description(prop, label);
    return true;
}
#endif

/* mic_mode remains the transport setting for saved-scene compatibility.
 * The panel separates speaker count, wiring and source assignment. */
static bool st_setup_properties(obs_properties_t *props, obs_property_t *property,
                                obs_data_t *settings)
{
    (void)property;
    bool two = obs_data_get_int(settings, "speaker_count") == 2;
    bool separate = two && obs_data_get_int(settings, "mic_wiring") == 1;
    bool second = separate && obs_data_get_int(settings, "source_role") == 1;
    obs_data_set_int(settings, "mic_mode", !two ? 0 : !separate ? 2 : second ? 1 : 0);
    obs_property_set_visible(obs_properties_get(props, "mic_wiring"), two);
    obs_property_set_visible(obs_properties_get(props, "source_role"), separate);
    if (two) obs_data_set_bool(settings, "native_twitch_cc", false);
    obs_property_set_enabled(obs_properties_get(props, "native_twitch_cc"), !two);
    obs_property_set_description(obs_properties_get(props, "native_help"), two ?
        "Native Twitch CC requires 1 speaker. Use a browser overlay for 2 speakers." :
        "Optional Twitch CC button. Set language in Control. Change before streaming.");
    obs_property_set_description(obs_properties_get(props, "audio_help"), !two ?
        "Languages and caption output: choose them at streamtranslate.live." : separate ?
        "Add this filter to each OBS source. Assign Mic 1 to one and Mic 2 to the other." :
        "Receiver in stereo: left = Mic 1, right = Mic 2. Enable Two speakers at streamtranslate.live.");
    return true;
}

/* Status refresh can rebuild properties during a selection. Never overwrite
 * explicit new-panel choices with the previous transport's mic_mode. */
static void st_migrate_setup(obs_data_t *settings)
{
    if (obs_data_has_user_value(settings, "speaker_count")) return;
    long long mode = obs_data_get_int(settings, "mic_mode");
    obs_data_set_int(settings, "speaker_count", mode == 0 ? 1 : 2);
    obs_data_set_int(settings, "mic_wiring", mode == 1 ? 1 : 0);
    obs_data_set_int(settings, "source_role", mode == 1 ? 1 : 0);
}

static bool st_audio_toggle_clicked(obs_properties_t *props, obs_property_t *prop, void *data)
{
    struct st_filter *f = data;
    if (!f) return false;
    pthread_mutex_lock(&f->qlock);
    bool paused = f->user_paused || f->terminal_paused || f->conflict_paused;
    pthread_mutex_unlock(&f->qlock);
    return paused ? st_reconnect_clicked(props, prop, data) : st_stop_clicked(props, prop, data);
}

static bool st_diagnostics_clicked(obs_properties_t *props, obs_property_t *prop, void *data)
{
    (void)props; (void)prop;
    struct st_filter *f = data;
    if (!f) return false;
    f->show_diagnostics = !f->show_diagnostics;
    return true;
}

static void st_diagnostic_text(struct st_filter *f, char *out, size_t size)
{
    uint64_t now = os_gettime_ns();
    pthread_mutex_lock(&f->qlock);
    snprintf(out,size,
      "Plugin %s | OBS %s\nConnection: %s\nAudio chunks: captured %d, sent %d\n"
      "Last delivery: %s | Queue: %d\nWrite errors: %" PRIu64 " | Conversion errors: %" PRIu64 "\n"
      "Last server close code: %u\n\n"
      "If account access is blocked, copy the current plugin key from Control and check your plan.\n"
      "If no audio is delivered, check that the source and filter are enabled and the source is not muted.\n"
      "Start sending audio resumes this filter. Pause sending audio pauses this filter only.\n"
      "Test connection checks server reachability only; it never starts or interrupts audio.\n"
      "For support, use Help > Log Files > View Current Log in OBS. Do not share your plugin key.",
      ST_RELEASE_VERSION,obs_get_version_string(),f->connected?"Connected":f->terminal_paused?"Blocked by server":f->user_paused?"Paused":f->waiting_audio?"Waiting for audio":f->conflict_paused?"Another source active":"Connecting / disconnected",
      f->audio_chunks_captured,f->audio_chunks_sent,
      f->last_send_ns && now-f->last_send_ns<2000000000ULL?"within 2 seconds":"no recent delivery",
      f->qcount,f->write_errors,f->resample_errors,f->last_close_code);
    pthread_mutex_unlock(&f->qlock);
}

static obs_properties_t *st_get_properties(void *data)
{
    struct st_filter *f = data;
    obs_properties_t *props = obs_properties_create();
    st_update_properties(props, f);
    obs_properties_add_text(props, "status_text", "", OBS_TEXT_INFO);
    obs_properties_add_text(props, "plugin_key", "Plugin key", OBS_TEXT_PASSWORD);
#ifdef __APPLE__
    obs_properties_add_button2(props, "copy_plugin_key", "⧉ Copy Plugin Key", st_copy_plugin_key_clicked, f);
#endif
    obs_property_t *count = obs_properties_add_list(props, "speaker_count", "Speakers",
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(count, "1 speaker", 1);
    obs_property_list_add_int(count, "2 speakers", 2);
    obs_property_t *wiring = obs_properties_add_list(props, "mic_wiring", "Microphones",
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(wiring, "One receiver (stereo)", 0);
    obs_property_list_add_int(wiring, "Two separate OBS sources", 1);
    obs_property_t *role = obs_properties_add_list(props, "source_role", "This source",
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(role, "Mic 1", 0);
    obs_property_list_add_int(role, "Mic 2", 1);
    obs_property_set_modified_callback(count, st_setup_properties);
    obs_property_set_modified_callback(wiring, st_setup_properties);
    obs_property_set_modified_callback(role, st_setup_properties);
    obs_properties_add_text(props, "audio_help",
        "Languages and caption output: choose them at streamtranslate.live.", OBS_TEXT_INFO);

    obs_properties_t *advanced = obs_properties_create();
    obs_properties_add_text(advanced, "server", "Server", OBS_TEXT_DEFAULT);
    obs_properties_add_bool(advanced, "native_twitch_cc", "Enable native Twitch CC");
    obs_properties_add_text(advanced, "native_help",
        "Optional Twitch CC button. Set language in Control. Change before streaming.", OBS_TEXT_INFO);
    obs_properties_add_group(props, "more_settings", "More settings", OBS_GROUP_NORMAL, advanced);

    obs_data_t *settings = f ? obs_source_get_settings(f->context) : obs_data_create();
    st_migrate_setup(settings);
    if (f) {
        pthread_mutex_lock(&f->qlock);
        obs_data_set_string(settings, "status_text", f->status[0] ? f->status : "Starting up…");
        pthread_mutex_unlock(&f->qlock);
    }
    st_setup_properties(props, NULL, settings);
    obs_data_release(settings);
    obs_properties_add_button2(props, "show_diagnostics",
        f && f->show_diagnostics ? "Hide diagnostics / help" : "Show diagnostics / help",
        st_diagnostics_clicked, f);
    obs_property_t *diagnostics = obs_properties_add_text(props, "diagnostics_text", "Diagnostics", OBS_TEXT_INFO);
    obs_property_set_visible(diagnostics, f && f->show_diagnostics);
    if (f) {
        char diagnostic[1600]; st_diagnostic_text(f, diagnostic, sizeof(diagnostic));
        obs_data_t *settings = obs_source_get_settings(f->context);
        obs_data_set_string(settings,"diagnostics_text",diagnostic);
        obs_data_release(settings);
    }

    bool paused = false;
    char test_result[320] = "Not tested. Checks server reachability without changing audio.";
    bool testing = false;
    if (f) {
        pthread_mutex_lock(&f->qlock);
        paused = f->user_paused || f->terminal_paused || f->conflict_paused;
        testing = f->test_busy;
        if (f->test_result[0]) snprintf(test_result, sizeof(test_result), "%s", f->test_result);
        pthread_mutex_unlock(&f->qlock);
    }
    obs_properties_add_button2(props, "stop_stream",
        paused ? "Start sending audio" : "Pause sending audio", st_audio_toggle_clicked, f);
    obs_property_t *test = obs_properties_add_button2(props, "test_connection", "Test connection",
        st_test_connection_clicked, f);
    obs_property_set_enabled(test, f && !testing);
    obs_property_set_long_description(test, "Read-only HTTPS health check. Does not resume audio, reconnect the audio socket, or validate your plugin key.");
    obs_properties_add_text(props, "connection_test_text", "Connection test", OBS_TEXT_INFO);
    if (f) {
        obs_data_t *settings = obs_source_get_settings(f->context);
        obs_data_set_string(settings, "connection_test_text", test_result);
        obs_data_release(settings);
    }
    return props;
}

static void st_get_defaults(obs_data_t *settings)
{
    obs_data_set_default_string(settings, "server", "streamtranslate.live");
    obs_data_set_default_string(settings, "plugin_key", "");
    obs_data_set_default_int(settings, "mic_mode", 0);
    obs_data_set_default_bool(settings, "native_twitch_cc", false);
    obs_data_set_default_bool(settings, "advanced", false);
}

static struct obs_source_info st_filter_info = {
	.id = "streamtranslate_audio_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_AUDIO,
	.get_name = st_get_name,
	.create = st_create,
	.destroy = st_destroy,
	.update = st_update,
	.filter_audio = st_filter_audio,
	.get_properties = st_get_properties,
	.get_defaults = st_get_defaults,
};

bool obs_module_load(void)
{
	st_updates_start();
	obs_register_source(&st_filter_info);
	blog(LOG_INFO, "[streamtranslate] plugin loaded");
	return true;
}

void obs_module_unload(void)
{
    st_updates_stop();
}
