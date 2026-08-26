#include "core/playback-state-store.hpp"
#include "obs/now-playing-source.hpp"
#include "windows/windows-media-playback-provider.hpp"

#include <obs-module.h>

#include <memory>
#include <string>
#include <utility>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-now-playing", "en-US")

namespace {

std::unique_ptr<now_playing::PlaybackStateStore> playback_state;
std::unique_ptr<now_playing::WindowsMediaPlaybackProvider> playback_provider;

} // namespace

MODULE_EXPORT const char* obs_module_description()
{
    return "Customizable Now Playing source using the local Windows media session";
}

MODULE_EXPORT const char* obs_module_name()
{
    return "OBS Now Playing";
}

MODULE_EXPORT const char* obs_module_author()
{
    return "SleepyKev";
}

bool obs_module_load()
{
#ifndef _WIN32
    blog(LOG_ERROR, "[obs-now-playing] Windows GSMTC backend is only available on Windows");
    return false;
#else
    playback_state = std::make_unique<now_playing::PlaybackStateStore>();
    now_playing::set_now_playing_playback_store(playback_state.get());
    obs_register_source(&now_playing_source_info);

    playback_provider = std::make_unique<now_playing::WindowsMediaPlaybackProvider>();
    playback_provider->start(
        [](const now_playing::TrackState& state) {
            if (playback_state) {
                playback_state->update(state);
            }
        },
        [](const std::string& error) {
            blog(LOG_WARNING, "[obs-now-playing] Windows media session: %s", error.c_str());
        });

    blog(LOG_INFO,
         "[obs-now-playing] Loaded version %s (Windows media session backend; no Spotify OAuth required)",
         NOWPLAYING_PLUGIN_VERSION);
    return true;
#endif
}

void obs_module_unload()
{
    if (playback_provider) {
        playback_provider->stop();
    }
    playback_provider.reset();
    now_playing::set_now_playing_playback_store(nullptr);
    playback_state.reset();
    blog(LOG_INFO, "[obs-now-playing] Unloaded");
}
