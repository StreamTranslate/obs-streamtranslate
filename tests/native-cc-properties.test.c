/* Actual libobs property/settings APIs, without registering the filter,
 * opening devices, connecting a websocket or starting an OBS output. */
#include "../plugin.c"
#include <assert.h>

int main(void)
{
    obs_data_t *settings = obs_data_create();
    st_get_defaults(settings);
    assert(obs_data_get_int(settings, "mic_mode") == 0);
    assert(!obs_data_get_bool(settings, "native_twitch_cc"));
    assert(!obs_data_get_bool(settings, "advanced"));

    obs_properties_t *props = st_get_properties(NULL);
    obs_property_t *key = obs_properties_get(props, "plugin_key");
    assert(obs_property_text_type(key) == OBS_TEXT_PASSWORD);
#ifdef __APPLE__
    obs_property_t *copy_key = obs_properties_get(props, "copy_plugin_key");
    assert(obs_property_get_type(copy_key) == OBS_PROPERTY_BUTTON);
    assert(obs_property_next(&key) && key == copy_key);
    assert(!obs_property_button_clicked(copy_key, NULL));
#endif
    obs_property_t *mode = obs_properties_get(props, "speaker_count");
    obs_property_t *cc = obs_properties_get(props, "native_twitch_cc");
    assert(obs_property_list_item_count(mode) == 2);
    assert(obs_property_list_item_int(mode, 0) == 1);
    assert(obs_property_list_item_int(mode, 1) == 2);
    assert(obs_property_get_type(cc) == OBS_PROPERTY_BOOL);
    assert(obs_property_group_type(obs_properties_get(props,"more_settings"))==OBS_GROUP_NORMAL);
    assert(!strcmp(obs_property_description(obs_properties_get(props,"more_settings")),"More settings"));
    assert(obs_property_visible(obs_properties_get(props,"server")));
    assert(obs_property_visible(cc));
    assert(!obs_property_visible(obs_properties_get(props, "mic_wiring")));
    assert(!obs_property_visible(obs_properties_get(props, "source_role")));
    obs_data_set_bool(settings, "native_twitch_cc", true);
    obs_data_set_int(settings,"speaker_count",2);
    obs_property_modified(mode,settings);
    assert(obs_data_get_int(settings,"mic_mode")==2);
    assert(obs_property_visible(obs_properties_get(props,"mic_wiring")));
    assert(!obs_property_visible(obs_properties_get(props,"source_role")));
    assert(!obs_property_enabled(cc));
    assert(!obs_data_get_bool(settings,"native_twitch_cc"));
    obs_data_set_int(settings,"mic_wiring",1);
    obs_data_set_int(settings,"source_role",1);
    obs_property_modified(mode,settings);
    assert(obs_data_get_int(settings,"mic_mode")==1);
    assert(obs_property_visible(obs_properties_get(props,"source_role")));
    assert(!obs_property_enabled(cc));
    obs_data_set_int(settings,"source_role",0);
    obs_property_modified(mode,settings);
    assert(obs_data_get_int(settings,"mic_mode")==0);
    assert(!obs_property_enabled(cc));
    obs_data_set_int(settings,"speaker_count",1);
    obs_property_modified(mode,settings);
    assert(obs_property_enabled(cc));
    assert(!obs_data_get_bool(settings,"native_twitch_cc"));

    struct st_filter f = {0};
    atomic_init(&f.native_twitch_cc, false);
    pthread_mutex_init(&f.qlock, NULL);
    st_update(&f, settings);
    assert(f.speaker == 0 && !f.stereo_split && !f.native_twitch_cc);
    /* Model OBS update-before-modified ordering: the very first update must
       negotiate the currently selected role, not the previous mic_mode. */
    obs_data_set_int(settings,"speaker_count",2);
    obs_data_set_int(settings,"mic_wiring",1);
    obs_data_set_bool(settings,"native_twitch_cc",true);
    st_update(&f,settings);
    assert(!f.native_twitch_cc && !obs_data_get_bool(settings,"native_twitch_cc"));
    obs_data_set_int(settings,"source_role",1);
    /* A status/property refresh between the click and update preserves Mic2. */
    st_migrate_setup(settings);
    assert(obs_data_get_int(settings,"source_role")==1);
    st_update(&f,settings);
    assert(f.speaker==1 && !f.stereo_split);
    obs_data_set_int(settings,"mic_wiring",0);
    st_update(&f,settings);
    assert(f.speaker==0 && f.stereo_split);
    obs_data_unset_user_value(settings,"speaker_count");
    obs_data_set_int(settings,"mic_mode",0);
    st_update(&f,settings);
    /* Previously saved true checkbox remains true with the new bool dropdown. */
    obs_data_set_bool(settings, "native_twitch_cc", true);
    st_update(&f, settings);
    assert(f.speaker == 0 && f.native_twitch_cc);
    obs_data_set_int(settings, "mic_mode", 1);

    assert(obs_data_get_bool(settings, "native_twitch_cc"));
    st_update(&f, settings);
    assert(f.speaker == 1 && !f.stereo_split);
    /* Existing receiver guard rejects Mic 2 even with a retained true opt-in. */
    st_cc_receive(&f, "{\"type\":\"native_cc_reset\",\"v\":1,\"epoch\":\"mic2\"}");
    assert(!f.cc_state.epoch[0]);
    obs_data_set_int(settings, "mic_mode", 2);

    st_update(&f, settings);
    assert(f.speaker == 0 && f.stereo_split && !f.native_twitch_cc);
    obs_data_set_bool(settings, "native_twitch_cc", false);
    obs_data_set_int(settings, "mic_mode", 0);
    st_update(&f, settings);
    assert(f.speaker == 0 && !f.stereo_split && !f.native_twitch_cc);
    pthread_mutex_destroy(&f.qlock);
    obs_properties_destroy(props);
    obs_data_release(settings);
    puts("compact properties: receiver and separate-source mapping, legacy settings, Mic2 guard and native CC compatibility passed");
    return 0;
}
