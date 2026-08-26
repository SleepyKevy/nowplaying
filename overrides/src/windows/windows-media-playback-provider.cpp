#include "windows-media-playback-provider.hpp"

#ifdef _WIN32

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/base.h>

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>
#include <QString>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <limits>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace now_playing {
namespace {

using namespace winrt;
using namespace Windows::Media::Control;
using namespace Windows::Storage::Streams;

constexpr auto poll_interval = std::chrono::milliseconds(750);
constexpr std::uint64_t maximum_thumbnail_bytes = 16ULL * 1024ULL * 1024ULL;

std::string lowercase_ascii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        if (ch >= 'A' && ch <= 'Z') {
            return static_cast<char>(ch - 'A' + 'a');
        }
        return static_cast<char>(ch);
    });
    return value;
}

bool is_spotify_session(const GlobalSystemMediaTransportControlsSession& session)
{
    if (!session) {
        return false;
    }

    const std::string source = lowercase_ascii(winrt::to_string(session.SourceAppUserModelId()));
    return source.find("spotify") != std::string::npos;
}

GlobalSystemMediaTransportControlsSession find_spotify_session(
    const GlobalSystemMediaTransportControlsSessionManager& manager)
{
    const auto current = manager.GetCurrentSession();
    if (current && is_spotify_session(current)) {
        return current;
    }

    for (const auto& session : manager.GetSessions()) {
        if (is_spotify_session(session)) {
            return session;
        }
    }

    return nullptr;
}

PlaybackStatus map_status(GlobalSystemMediaTransportControlsSessionPlaybackStatus status)
{
    switch (status) {
    case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing:
        return PlaybackStatus::playing;
    case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Paused:
        return PlaybackStatus::paused;
    case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Stopped:
    case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Closed:
        return PlaybackStatus::stopped;
    case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Opened:
    case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Changing:
    default:
        return PlaybackStatus::paused;
    }
}

std::int64_t time_span_ms(const Windows::Foundation::TimeSpan& span)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(span).count();
}

QString artwork_cache_directory()
{
    QString root = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (root.isEmpty()) {
        root = QDir::tempPath();
    }

    const QString directory = QDir(root).filePath(QStringLiteral("obs-now-playing/windows-media-artwork"));
    QDir().mkpath(directory);
    return directory;
}

QString extension_for_content_type(const std::string& content_type)
{
    const std::string lowered = lowercase_ascii(content_type);
    if (lowered.find("png") != std::string::npos) {
        return QStringLiteral(".png");
    }
    if (lowered.find("webp") != std::string::npos) {
        return QStringLiteral(".webp");
    }
    if (lowered.find("gif") != std::string::npos) {
        return QStringLiteral(".gif");
    }
    if (lowered.find("bmp") != std::string::npos) {
        return QStringLiteral(".bmp");
    }
    return QStringLiteral(".jpg");
}

QString cache_key_digest(const std::string& identity)
{
    const QByteArray digest = QCryptographicHash::hash(QByteArray::fromStdString(identity),
                                                       QCryptographicHash::Sha256)
                                  .toHex();
    return QString::fromLatin1(digest);
}

std::string save_thumbnail(const IRandomAccessStreamReference& thumbnail, const std::string& identity)
{
    if (!thumbnail) {
        return {};
    }

    const auto stream = thumbnail.OpenReadAsync().get();
    if (!stream) {
        return {};
    }

    const std::uint64_t size64 = stream.Size();
    if (size64 == 0 || size64 > maximum_thumbnail_bytes ||
        size64 > std::numeric_limits<std::uint32_t>::max()) {
        return {};
    }

    const QString path = QDir(artwork_cache_directory())
                             .filePath(cache_key_digest(identity) +
                                       extension_for_content_type(winrt::to_string(stream.ContentType())));

    QFileInfo existing(path);
    if (existing.exists() && existing.isFile() && existing.size() > 0) {
        return path.toUtf8().toStdString();
    }

    const std::uint32_t size = static_cast<std::uint32_t>(size64);
    DataReader reader{stream.GetInputStreamAt(0)};
    const std::uint32_t loaded = reader.LoadAsync(size).get();
    if (loaded == 0) {
        return {};
    }

    std::vector<std::uint8_t> bytes(loaded);
    reader.ReadBytes(bytes);

    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly)) {
        return {};
    }
    if (output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<qint64>(bytes.size())) !=
        static_cast<qint64>(bytes.size())) {
        output.cancelWriting();
        return {};
    }
    if (!output.commit()) {
        return {};
    }

    return path.toUtf8().toStdString();
}

TrackState read_state(const GlobalSystemMediaTransportControlsSession& session)
{
    TrackState state;
    state.observed_at = std::chrono::steady_clock::now();
    state.provider_timestamp_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();

    const auto playback_info = session.GetPlaybackInfo();
    state.status = map_status(playback_info.PlaybackStatus());

    const auto timeline = session.GetTimelineProperties();
    const std::int64_t start_ms = time_span_ms(timeline.StartTime());
    const std::int64_t end_ms = time_span_ms(timeline.EndTime());
    const std::int64_t position_ms = time_span_ms(timeline.Position());
    state.duration_ms = std::max<std::int64_t>(0, end_ms - start_ms);
    state.progress_ms = std::max<std::int64_t>(0, position_ms - start_ms);
    if (state.duration_ms > 0) {
        state.progress_ms = std::min(state.progress_ms, state.duration_ms);
    }

    const auto properties = session.TryGetMediaPropertiesAsync().get();
    state.title = winrt::to_string(properties.Title());
    state.artists = winrt::to_string(properties.Artist());
    state.album = winrt::to_string(properties.AlbumTitle());

    if (state.artists.empty()) {
        state.artists = winrt::to_string(properties.AlbumArtist());
    }
    if (state.artists.empty()) {
        state.artists = winrt::to_string(properties.Subtitle());
    }

    if (state.title.empty()) {
        if (state.status == PlaybackStatus::playing || state.status == PlaybackStatus::paused) {
            state.title = "Spotify";
        }
    }

    if (!state.title.empty()) {
        const std::string identity = winrt::to_string(session.SourceAppUserModelId()) + "\n" +
                                     state.title + "\n" + state.artists + "\n" + state.album;
        const QString digest = cache_key_digest(identity);
        state.track_id = "windows:spotify:" + digest.toStdString();
        state.artwork.cache_key = state.track_id;
        state.artwork.local_path = save_thumbnail(properties.Thumbnail(), identity);
        state.content_type = ContentType::track;
    } else {
        state.content_type = ContentType::unknown;
        if (state.status == PlaybackStatus::playing || state.status == PlaybackStatus::paused) {
            state.status = PlaybackStatus::stopped;
        }
    }

    return state;
}

void interruptible_sleep(std::stop_token stop_token, std::chrono::milliseconds duration)
{
    constexpr auto slice = std::chrono::milliseconds(50);
    auto remaining = duration;
    while (!stop_token.stop_requested() && remaining > std::chrono::milliseconds::zero()) {
        const auto wait = std::min(slice, remaining);
        std::this_thread::sleep_for(wait);
        remaining -= wait;
    }
}

} // namespace

WindowsMediaPlaybackProvider::~WindowsMediaPlaybackProvider()
{
    stop();
}

void WindowsMediaPlaybackProvider::start(StateHandler on_state, ErrorHandler on_error)
{
    stop();
    on_state_ = std::move(on_state);
    on_error_ = std::move(on_error);
    running_.store(true, std::memory_order_release);
    session_found_.store(false, std::memory_order_release);
    worker_ = std::jthread([this](std::stop_token stop_token) { worker_loop(stop_token); });
}

void WindowsMediaPlaybackProvider::stop()
{
    running_.store(false, std::memory_order_release);
    session_found_.store(false, std::memory_order_release);
    if (worker_.joinable()) {
        worker_.request_stop();
        worker_.join();
    }
    on_state_ = {};
    on_error_ = {};
}

bool WindowsMediaPlaybackProvider::is_connected() const noexcept
{
    return running_.load(std::memory_order_acquire) &&
           session_found_.load(std::memory_order_acquire);
}

void WindowsMediaPlaybackProvider::publish(TrackState state)
{
    if (running_.load(std::memory_order_acquire) && on_state_) {
        on_state_(state);
    }
}

void WindowsMediaPlaybackProvider::publish_empty(PlaybackStatus status)
{
    TrackState state;
    state.status = status;
    state.observed_at = std::chrono::steady_clock::now();
    publish(std::move(state));
}

void WindowsMediaPlaybackProvider::report_error(const std::string& message)
{
    if (running_.load(std::memory_order_acquire) && on_error_) {
        on_error_(message);
    }
}

void WindowsMediaPlaybackProvider::worker_loop(std::stop_token stop_token)
{
    std::string last_error;
    bool apartment_initialized = false;

    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        apartment_initialized = true;
        const auto manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();

        while (!stop_token.stop_requested()) {
            try {
                const auto session = find_spotify_session(manager);
                if (!session) {
                    session_found_.store(false, std::memory_order_release);
                    publish_empty(PlaybackStatus::stopped);
                    last_error.clear();
                    interruptible_sleep(stop_token, poll_interval);
                    continue;
                }

                session_found_.store(true, std::memory_order_release);
                publish(read_state(session));
                last_error.clear();
            } catch (const winrt::hresult_error& error) {
                session_found_.store(false, std::memory_order_release);
                const std::string message = "Windows media session error: " + winrt::to_string(error.message());
                if (message != last_error) {
                    report_error(message);
                    last_error = message;
                }
                publish_empty(PlaybackStatus::unavailable);
            } catch (const std::exception& error) {
                session_found_.store(false, std::memory_order_release);
                const std::string message = std::string("Windows media session error: ") + error.what();
                if (message != last_error) {
                    report_error(message);
                    last_error = message;
                }
                publish_empty(PlaybackStatus::unavailable);
            }

            interruptible_sleep(stop_token, poll_interval);
        }
    } catch (const winrt::hresult_error& error) {
        session_found_.store(false, std::memory_order_release);
        report_error("Unable to initialize Windows media sessions: " + winrt::to_string(error.message()));
        publish_empty(PlaybackStatus::unavailable);
    } catch (const std::exception& error) {
        session_found_.store(false, std::memory_order_release);
        report_error(std::string("Unable to initialize Windows media sessions: ") + error.what());
        publish_empty(PlaybackStatus::unavailable);
    }

    if (apartment_initialized) {
        winrt::uninit_apartment();
    }
}

} // namespace now_playing

#else

#include <utility>

namespace now_playing {

WindowsMediaPlaybackProvider::~WindowsMediaPlaybackProvider() = default;
void WindowsMediaPlaybackProvider::start(StateHandler on_state, ErrorHandler on_error)
{
    on_state_ = std::move(on_state);
    on_error_ = std::move(on_error);
    running_.store(false);
    session_found_.store(false);
    report_error("Windows media sessions are only available on Windows");
}
void WindowsMediaPlaybackProvider::stop() {}
bool WindowsMediaPlaybackProvider::is_connected() const noexcept { return false; }
void WindowsMediaPlaybackProvider::worker_loop(std::stop_token) {}
void WindowsMediaPlaybackProvider::publish(TrackState) {}
void WindowsMediaPlaybackProvider::publish_empty(PlaybackStatus) {}
void WindowsMediaPlaybackProvider::report_error(const std::string& message)
{
    if (on_error_) {
        on_error_(message);
    }
}

} // namespace now_playing

#endif
