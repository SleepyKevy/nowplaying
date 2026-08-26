#pragma once

#include "../services/playback-provider.hpp"

#include <atomic>
#include <stop_token>
#include <string>
#include <thread>

namespace now_playing {

class WindowsMediaPlaybackProvider final : public PlaybackProvider {
public:
    WindowsMediaPlaybackProvider() = default;
    ~WindowsMediaPlaybackProvider() override;

    WindowsMediaPlaybackProvider(const WindowsMediaPlaybackProvider&) = delete;
    WindowsMediaPlaybackProvider& operator=(const WindowsMediaPlaybackProvider&) = delete;

    void start(StateHandler on_state, ErrorHandler on_error) override;
    void stop() override;
    [[nodiscard]] bool is_connected() const noexcept override;

private:
    void worker_loop(std::stop_token stop_token);
    void publish(TrackState state);
    void publish_empty(PlaybackStatus status);
    void report_error(const std::string& message);

    StateHandler on_state_;
    ErrorHandler on_error_;
    std::jthread worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> session_found_{false};
};

} // namespace now_playing
