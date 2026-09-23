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
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

OBS_DECLARE_MODULE()

#define ST_PLUGIN_VERSION "capture-health-v1"
#ifdef _WIN32
#define ST_PLATFORM "windows"
#elif defined(__APPLE__)
#define ST_PLATFORM "macos"
#else
#define ST_PLATFORM "linux"
#endif
#define ST_QUEUE_MAX 256 /* ~30s of 120ms chunks — drop oldest beyond this */

struct st_chunk {
	struct st_chunk *next;
	size_t len;
	unsigned char *buf; /* includes LWS_PRE headroom; payload at buf+LWS_PRE */
};

struct st_filter {
	obs_source_t *context;

	/* config */
	char server[256];
	char plugin_key[128];

	/* live status shown in the filter UI */
	char status[320];
	bool muted_now;
	int  audio_chunks_captured;
	int  audio_chunks_sent;
	char filter_id[48];
	uint64_t callbacks, input_frames, last_callback_ns, last_send_ns;
	uint64_t written_bytes, write_errors, resample_errors, queue_drops, reconnects;
	double left_peak, right_peak;
	uint64_t diagnostic_sequence, last_health_ns;
	char health_ring[16][2048];
	unsigned health_read, health_count;
	bool conflict_paused;


	/* audio conversion */
	audio_resampler_t *resampler;
	uint32_t sample_rate;

	/* websocket service thread */
	pthread_t thread;
	volatile bool stop;
	volatile bool connected;
	struct lws_context *lws_ctx;
	struct lws *wsi;
    lws_sorted_usec_list_t service_pulse;

	/* outbound queue (audio thread -> ws thread) */
	pthread_mutex_t qlock;
	struct st_chunk *qhead, *qtail;
	int qcount;
};

static void st_set_status(struct st_filter *f, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(f->status, sizeof(f->status), fmt, ap);
	va_end(ap);
	blog(LOG_INFO, "[streamtranslate] %s", f->status);
	/* OBS builds the properties panel once; without this the Status line
	 * shows whatever it said when the dialog opened. */
	if (f->context) {
		obs_data_t *sd = obs_source_get_settings(f->context);
		if (sd) {
			obs_data_set_string(sd, "status_text", f->status);
			obs_data_release(sd);
		}
		obs_source_update_properties(f->context);
	}
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
	c->next = NULL;

	pthread_mutex_lock(&f->qlock);
	if (f->qcount >= ST_QUEUE_MAX && f->qhead) {
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
		st_set_status(f, "Connected to %s - streaming audio", f->server);
		f->connected = true;
		f->last_health_ns=0;
		st_health_snapshot(f);
		lws_callback_on_writable(wsi);
		break;

	case LWS_CALLBACK_CLIENT_WRITEABLE: {
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
			int wrote = lws_write(wsi, c->buf + LWS_PRE, c->len, LWS_WRITE_BINARY);
            pthread_mutex_lock(&f->qlock);
            if(wrote==(int)c->len){f->audio_chunks_sent++;f->written_bytes+=wrote;f->last_send_ns=os_gettime_ns();}
            else f->write_errors++;
            pthread_mutex_unlock(&f->qlock);
            bool write_failed=wrote!=(int)c->len;
			free(c->buf);
			free(c);
            if(write_failed)return -1;
			lws_callback_on_writable(wsi);
		}
		break;
	}

	case LWS_CALLBACK_EVENT_WAIT_CANCELLED:
		if (f->wsi && f->connected)
			lws_callback_on_writable(f->wsi);
		break;

	case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
		st_set_status(f, "Connection FAILED: %s (server: %s) - retrying",
			      in ? (const char *)in : "unknown error", f->server);
		f->connected = false;
		f->wsi = NULL;
		break;

    case LWS_CALLBACK_WS_PEER_INITIATED_CLOSE:
        if(len>=2){const unsigned char *p=in;unsigned code=(p[0]<<8)|p[1];
            if(code==4001 || code==4009){f->conflict_paused=true;st_set_status(f,"Another audio source is active. Reconnect only to switch back to this filter.");}
        }
        break;
	case LWS_CALLBACK_CLIENT_CLOSED:
        st_queue_clear(f);
        if(!f->conflict_paused) st_set_status(f, "Disconnected from %s - reconnecting", f->server);
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

	char path[512];
	snprintf(path, sizeof(path), "/audio?pluginKey=%s&rate=%u&tabId=%s&transportVersion=%s",
         f->plugin_key, f->sample_rate,f->filter_id,ST_PLUGIN_VERSION);
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

	st_set_status(f, "Connecting to %s ...", f->server);
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
		if (GetModuleFileNameA(hm, mod, sizeof(mod))) {
			char *slash = strrchr(mod, '\\');
			if (slash) {
				*slash = 0;
				snprintf(path, sizeof(path), "%s\\cacert.pem", mod);
			}
		}
	}
#else
	Dl_info dli;
	if (dladdr((void *)&st_ca_bundle_path, &dli) && dli.dli_fname) {
		char mod[1024];
		snprintf(mod, sizeof(mod), "%s", dli.dli_fname);
		char *slash = strrchr(mod, '/');
		if (slash) {
			*slash = 0;
			/* macOS bundle: .../Contents/MacOS -> .../Contents/Resources/cacert.pem */
			char *macos = strstr(mod, "/Contents/MacOS");
			if (macos) {
				*macos = 0;
				snprintf(path, sizeof(path), "%s/Contents/Resources/cacert.pem", mod);
			} else {
				snprintf(path, sizeof(path), "%s/cacert.pem", mod);
			}
		}
	}
	if (path[0] && access(path, R_OK) != 0) {
		/* fall back to common system stores (Linux/cloud OBS) */
		if (access("/etc/ssl/certs/ca-certificates.crt", R_OK) == 0)
			snprintf(path, sizeof(path), "/etc/ssl/certs/ca-certificates.crt");
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
	uint64_t last_status = 0;
	while (!f->stop) {
		st_health_snapshot(f);
		if (!f->wsi && !f->conflict_paused) {
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
		if (f->wsi && f->connected) {
			pthread_mutex_lock(&f->qlock);
			int pending = f->qcount;
			pthread_mutex_unlock(&f->qlock);
			if (pending > 0 || f->health_count > 0)
				lws_callback_on_writable(f->wsi);
		}
		lws_service(f->lws_ctx, 20);

		/* refresh the status line with live counters once a second */
		uint64_t now2 = os_gettime_ns();
		if (f->connected && !f->muted_now && now2 - last_status > 15000000000ULL) {
			last_status = now2;
			st_set_status(f, "Connected to %s - captured %d, sent %d audio chunks",
				      f->server, f->audio_chunks_captured, f->audio_chunks_sent);
		}
	}

    lws_sul_cancel(&f->service_pulse);
	if (f->wsi) {
		lws_set_timeout(f->wsi, PENDING_TIMEOUT_CLOSE_SEND, LWS_TO_KILL_ASYNC);
		lws_service(f->lws_ctx, 50);
	}
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
	struct resample_info dst = {
		.samples_per_sec = aoi->samples_per_sec,
		.format = AUDIO_FORMAT_16BIT,
		.speakers = SPEAKERS_MONO,
	};
	f->resampler = audio_resampler_create(&dst, &src);
    if (!f->resampler) { pthread_mutex_lock(&f->qlock);f->resample_errors++;pthread_mutex_unlock(&f->qlock);blog(LOG_ERROR, "[streamtranslate] resampler creation failed"); }
}

static void st_update(void *data, obs_data_t *settings)
{
	struct st_filter *f = data;
	const char *server = obs_data_get_string(settings, "server");
	const char *key = obs_data_get_string(settings, "plugin_key");

	bool changed = strcmp(f->server, server ? server : "") != 0 ||
		       strcmp(f->plugin_key, key ? key : "") != 0;

	snprintf(f->server, sizeof(f->server), "%s", server ? server : "");
	snprintf(f->plugin_key, sizeof(f->plugin_key), "%s", key ? key : "");

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
    snprintf(f->filter_id,sizeof(f->filter_id),"filter-%" PRIx64,os_gettime_ns());
	pthread_mutex_init(&f->qlock, NULL);
	st_build_resampler(f);
	st_update(f, settings);
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
	pthread_join(f->thread, NULL);
	st_queue_clear(f);
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
    if (!audio || !audio->frames) return audio;

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
			st_set_status(f, "Muted in OBS - not sending audio (captured %d, sent %d)",
				      f->audio_chunks_captured, f->audio_chunks_sent);
		}
		return audio;
	}
	if (f->muted_now)
		f->muted_now = false;
	if (!f->resampler) {
		st_build_resampler(f); /* audio subsystem may not have been ready at create time */
		if (!f->resampler)
			return audio;
	}
	if (!f->connected)
		return audio; /* always pass audio through untouched */

	uint8_t *out[MAX_AV_PLANES] = {0};
	uint32_t out_frames = 0;
	uint64_t ts_offset = 0;

	if (audio_resampler_resample(f->resampler, out, &out_frames, &ts_offset,
				     (const uint8_t *const *)audio->data,
				     audio->frames) &&
	    out_frames > 0 && out[0]) {
		st_queue_push(f, out[0], (size_t)out_frames * 2 /* s16 mono */);
		pthread_mutex_lock(&f->qlock);f->audio_chunks_captured++;pthread_mutex_unlock(&f->qlock);
		if (f->lws_ctx)
			lws_cancel_service(f->lws_ctx); /* wake ws thread to flush */
    } else { pthread_mutex_lock(&f->qlock);f->resample_errors++;pthread_mutex_unlock(&f->qlock); }
	return audio;
}

static bool st_reconnect_clicked(obs_properties_t *props, obs_property_t *prop, void *data)
{
	struct st_filter *f = data;
	(void)props;
	(void)prop;
	if (!f)
		return false;
	f->conflict_paused=false;
	st_set_status(f, "Reconnecting ...");
	if (f->wsi) {
		lws_set_timeout(f->wsi, PENDING_TIMEOUT_CLOSE_SEND, LWS_TO_KILL_ASYNC);
	}
	if (f->lws_ctx)
		lws_cancel_service(f->lws_ctx);
	return true;
}

static obs_properties_t *st_get_properties(void *data)
{
	struct st_filter *f = data;
	obs_properties_t *props = obs_properties_create();

	obs_properties_add_text(props, "plugin_key",
				"Plugin Key (from your StreamTranslate account)",
				OBS_TEXT_PASSWORD);
	obs_properties_add_text(props, "server", "Server", OBS_TEXT_DEFAULT);

	/* live status — updated by the connection thread */
	obs_property_t *st = obs_properties_add_text(props, "status_text", "Status", OBS_TEXT_INFO);
	if (f) {
		char line[420];
		snprintf(line, sizeof(line), "%s%s", f->status[0] ? f->status : "Starting up ...",
			 f->connected ? "" : "");
		obs_property_set_long_description(st, line);
		obs_data_t *s = obs_source_get_settings(f->context);
		if (s) {
			obs_data_set_string(s, "status_text", line);
			obs_data_release(s);
		}
	}

	obs_properties_add_button2(props, "reconnect", "Reconnect / Test connection",
				   st_reconnect_clicked, f);
	return props;
}

static void st_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "server", "streamtranslate.live");
	obs_data_set_default_string(settings, "plugin_key", "");
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
	obs_register_source(&st_filter_info);
	blog(LOG_INFO, "[streamtranslate] plugin loaded");
	return true;
}
