/**
 * Taiga
 * Copyright (C) 2010-2026, Eren Okka
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "media.hpp"

#include <QFileInfo>
#include <QUrl>
#include <algorithm>
#include <optional>
#include <set>
#include <string>

#include "base/string.hpp"
#include "media/anime.hpp"
#include "media/anime_db.hpp"
#include "media_server/server.hpp"
#include "taiga/settings.hpp"
#include "track/episode.hpp"
#include "track/kodi.hpp"
#include "track/media_player.hpp"
#include "track/recognition.hpp"
#include "track/recognition_validate.hpp"
#include "track/scanner.hpp"

namespace track::media {

namespace {

// Same as the video container extensions that Anitomy recognizes, so that files Kodi reports are
// also accepted by `recognition::isVideoFile`.
const std::set<std::string>& videoExtensions() {
  static const std::set<std::string> extensions{
      "3gp", "avi", "divx", "flv", "m2ts", "m4v", "mkv", "mov",
      "mp4", "mpg", "ogm",  "rm",  "rmvb", "ts",  "webm", "wmv",
  };
  return extensions;
}

[[maybe_unused]] bool hasVideoExtension(const QString& path) {
  const auto extension = path.section(u'.', -1).toLower().toStdString();
  return path.contains(u'.') && videoExtensions().contains(extension);
}

[[maybe_unused]] anisthesia::Media mediaFromKodi(const kodi::Playback& playback) {
  anisthesia::Media media;
  media.state = playback.paused ? anisthesia::MediaState::Paused : anisthesia::MediaState::Playing;
  media.position = playback.position;
  media.duration = playback.duration;

  const auto add = [&media](anisthesia::MediaInfoType type, const QString& value) {
    if (!value.isEmpty()) media.information.push_back({type, value.toStdString()});
  };

  if (playback.file.startsWith(u'/')) {
    add(anisthesia::MediaInfoType::File, playback.file);
  } else if (const QUrl url{playback.file};
             url.isValid() && hasVideoExtension(url.path(QUrl::FullyDecoded))) {
    // Network shares (e.g. `smb://host/share/Show/Episode.mkv`) keep the folder structure, which
    // helps recognition just like local paths do.
    add(anisthesia::MediaInfoType::File, url.path(QUrl::FullyDecoded));
  }

  // Items in Kodi's video library have proper metadata.
  if (!playback.showTitle.isEmpty() && playback.episode > 0) {
    auto title = playback.showTitle;
    if (playback.season > 1) title += u" S%1"_s.arg(playback.season);
    title += u" - %1"_s.arg(playback.episode, 2, 10, QChar{u'0'});
    add(anisthesia::MediaInfoType::Title, title);
  } else {
    add(anisthesia::MediaInfoType::Title, !playback.label.isEmpty() ? playback.label : playback.title);
  }

  return media;
}

struct MediaFields {
  std::string file;
  std::string title;
  std::string url;
};

#ifdef Q_OS_WINDOWS
anisthesia::Media flattenMedia(const anisthesia::win::Result& result) {
  anisthesia::Media media;

  for (const auto& item : result.media) {
    media.information.append_range(item.information);
  }

  return media;
}
#endif

MediaFields extractMediaFields(const anisthesia::Media& media) {
  MediaFields fields;

  for (const auto& information : media.information) {
    switch (information.type) {
      case anisthesia::MediaInfoType::File:
        if (fields.file.empty()) fields.file = information.value;
        break;
      case anisthesia::MediaInfoType::Url:
        fields.url = information.value;
        break;
      case anisthesia::MediaInfoType::Title:
        if (fields.title.empty()) fields.title = information.value;
        break;
      case anisthesia::MediaInfoType::Unknown:
        // Always prefer window title over page title reported via UI Automation.
        fields.title = information.value;
        break;
      case anisthesia::MediaInfoType::Tab:
        break;
    }
  }

  return fields;
}

#ifdef Q_OS_LINUX
// The following functions are ports of Taiga 1.x (see `track/media.cpp`), which decide what a
// media player is playing from the information that Anisthesia found.

bool isAudioFileExtension(const QString& extension) {
  static const QStringList extensions{
      u"aac"_s, u"aiff"_s, u"flac"_s, u"m4a"_s, u"mka"_s, u"mp3"_s, u"ogg"_s, u"wav"_s, u"wma"_s,
  };
  return extensions.contains(extension, Qt::CaseInsensitive);
}

std::optional<anisthesia::MediaInfo> getMediaFromDefaultPlayer(
    const std::vector<anisthesia::Media>& media) {
  bool invalidFile = false;
  const bool checkLibraryFolders =
      taiga::settings.updateLibraryOnly() && !taiga::settings.libraryFolders().empty();

  for (const auto& item : media) {
    for (const auto& information : item.information) {
      switch (information.type) {
        case anisthesia::MediaInfoType::File: {
          const QFileInfo fileInfo{QString::fromStdString(information.value)};
          const auto fileExtension = fileInfo.suffix();

          // Taiga 1.x checks the extension against Anitomy's keywords, and so does
          // `recognition::isVideoFile`.
          std::optional<Episode> episode;
          const auto isValidFileExtension = [&]() {
            if (fileExtension.isEmpty() || fileExtension.size() > 4) return false;
            if (!episode) episode = track::recognition::parseFileInfo(fileInfo);
            return track::recognition::isVideoFile(*episode);
          };

          // Invalid path
          if (checkLibraryFolders && !isInsideLibraryFolders(fileInfo.filePath())) {
            if (isValidFileExtension() || isAudioFileExtension(fileExtension)) {
              invalidFile = true;
            }
            continue;
          }

          // Video file
          if (isValidFileExtension()) {
            // Invalid type
            if (!track::recognition::isValidEpisodeType(*episode)) {
              invalidFile = true;
              continue;
            }
            // Valid video file
            return information;
          }

          // Audio file
          if (isAudioFileExtension(fileExtension)) {
            invalidFile = true;
            continue;
          }

          // Unknown extension
          break;
        }

        default:
        case anisthesia::MediaInfoType::Title:
        case anisthesia::MediaInfoType::Unknown: {
          if (!invalidFile) {
            return information;
          }
          break;
        }
      }
    }
  }

  return std::nullopt;
}

std::optional<anisthesia::MediaInfo> getMediaFromResult(const anisthesia::lin::Result& result) {
  switch (result.player.type) {
    case anisthesia::PlayerType::Default:
      return getMediaFromDefaultPlayer(result.media);
    case anisthesia::PlayerType::WebBrowser:
      // Requires the window title and UI Automation strategies, which are not available here
      return std::nullopt;
  }
  return std::nullopt;
}
#endif

std::optional<Episode> resolveEpisode(const MediaFields& fields) {
  if (!fields.file.empty()) {
    const QFileInfo fileInfo{QString::fromStdString(fields.file)};
    auto episode = track::recognition::parseFileInfo(fileInfo);

    if (!track::recognition::isVideoFile(episode)) return std::nullopt;

    return episode;
  }

  auto value = fields.title;

  if (value.empty()) return std::nullopt;

  return track::recognition::parse(value);
}

}  // namespace

Detection::Detection(QObject* parent) : QObject(parent) {
  pollTimer_ = new QTimer(this);
  connect(pollTimer_, &QTimer::timeout, this, &Detection::poll);

#ifndef Q_OS_WINDOWS
  // React to play/pause/stop in Kodi right away, rather than on the next tick.
  connect(kodi::client(), &kodi::Client::playbackChanged, this, [this]() {
    if (pollTimer_->isActive() && taiga::settings.kodiDetection()) poll();
  });
#endif
}

const std::optional<Episode> Detection::getCurrentEpisode() const {
  return currentEpisode_;
}

const std::optional<std::string> Detection::getCurrentFile() const {
  if (!currentMedia_) return std::nullopt;

  auto file = extractMediaFields(*currentMedia_).file;
  if (file.empty()) return std::nullopt;

  return file;
}

const std::optional<Detection::media_t> Detection::getCurrentMedia() const {
  return currentMedia_;
}

const std::optional<Detection::player_t> Detection::getCurrentPlayer() const {
  return currentPlayer_;
}

bool Detection::init() {
  if (!parsePlayersData(players_)) {
    return false;
  }

  setPollingEnabled(isEnabled());

  return true;
}

bool Detection::isEnabled() const {
  return taiga::settings.detectionEnabled();
}

void Detection::setEnabled(bool enabled) {
  taiga::settings.setDetectionEnabled(enabled);
  setPollingEnabled(enabled);
}

void Detection::setPollingEnabled(bool enabled) {
  if (enabled) {
    const auto interval = taiga::settings.mediaDetectionInterval();
    pollTimer_->start(interval);
  } else {
    pollTimer_->stop();
#ifndef Q_OS_WINDOWS
    playerPaused_ = false;
#endif
    reset();
  }
}

void Detection::poll() {
#ifdef Q_OS_WINDOWS
  const auto players = getEnabledPlayers(players_);

  static const auto media_proc = [](const anisthesia::MediaInfo&) {
    return true;  // Accept all media
  };

  std::vector<anisthesia::win::Result> results;
  if (!anisthesia::win::GetResults(players, media_proc, results)) {
    reset();
    return;
  }

  const auto resultIt = std::ranges::find_if(results, [this](const anisthesia::win::Result& r) {
    return r.window.handle == currentWindowHandle_;
  });
  const auto& result = resultIt != results.end() ? *resultIt : results.front();

  currentPlayer_ = result.player;
  currentMedia_ = flattenMedia(result);
  currentWindowHandle_ = result.window.handle;

  auto episode = resolveEpisode(extractMediaFields(*currentMedia_));
  if (!episode) {
    reset();
    return;
  }

  const auto animeId = track::recognition::identify(*episode);
  episode->setAnimeId(animeId);

  if (hasEpisodeChanged(*episode)) {
    currentEpisode_ = episode;
    emit currentEpisodeChanged(episode);
  }
#else
  const auto players = getEnabledPlayers(players_);

  // Taiga's own media server knows exactly what each client is streaming (and whether it is
  // paused), so it takes precedence. Kodi's JSON-RPC interface is optional. Otherwise we detect
  // media players the same way as on Windows, which also covers Kodi streaming to other devices
  // via its UPnP/DLNA server.
  auto result = pollMediaServer();
  if (!result) result = pollKodi(players);
#ifdef Q_OS_LINUX
  if (!result) result = pollPlayers(players);
#endif

  if (!result) {
    playerPaused_ = false;
    reset();
    return;
  }

  auto& [player, media] = *result;
  playerPaused_ = media.state == anisthesia::MediaState::Paused;
  currentPlayer_ = std::move(player);
  currentMedia_ = std::move(media);

  auto episode = resolveEpisode(extractMediaFields(*currentMedia_));
  if (!episode) {
    reset();
    return;
  }

  const auto animeId = track::recognition::identify(*episode);
  episode->setAnimeId(animeId);

  if (hasEpisodeChanged(*episode)) {
    currentEpisode_ = episode;
    emit currentEpisodeChanged(episode);
  }
#endif
}

#ifndef Q_OS_WINDOWS
std::optional<Detection::result_t> Detection::pollKodi(const std::vector<player_t>& players) {
  auto* client = kodi::client();
  if (!client->isEnabled() || !taiga::settings.kodiDetection()) return std::nullopt;

  // Respects the user's choice in Settings > Recognition > Media players.
  const auto player = std::ranges::find(players, std::string{"Kodi"}, &player_t::name);
  if (player == players.end()) return std::nullopt;

  client->poll();  // asynchronous; the result is used on the next tick

  const auto& playback = client->playback();
  if (!playback) return std::nullopt;

  return result_t{*player, mediaFromKodi(*playback)};
}

std::optional<Detection::result_t> Detection::pollMediaServer() {
  if (!taiga::settings.mediaServerDetection()) return std::nullopt;

  const auto* server = media_server::server();
  if (!server->isRunning()) return std::nullopt;

  const auto stream = server->currentStream();
  if (!stream) return std::nullopt;

  player_t player;
  player.name = u"Taiga media server (%1)"_s.arg(stream->client.toString()).toStdString();

  media_t media;
  media.state = stream->paused ? anisthesia::MediaState::Paused : anisthesia::MediaState::Playing;
  media.information.push_back({anisthesia::MediaInfoType::File, stream->path.toStdString()});

  return result_t{player, media};
}

#ifdef Q_OS_LINUX
std::optional<Detection::result_t> Detection::pollPlayers(const std::vector<player_t>& players) {
  // Port of `MediaPlayers::CheckRunningPlayers` in Taiga 1.x
  static const auto media_proc = [](const anisthesia::MediaInfo&) {
    return true;  // Accept all media
  };

  std::vector<anisthesia::lin::Result> results;

  if (anisthesia::lin::GetResults(players, media_proc, results)) {
    // Stick with the previously detected process if possible
    if (currentProcessId_ && isMediaIdentified()) {
      const auto it = std::ranges::find(results, currentProcessId_,
                                        [](const anisthesia::lin::Result& result) {
                                          return static_cast<int>(result.process.id);
                                        });
      if (it != results.end()) std::rotate(results.begin(), it, it + 1);  // Move to front
    }

    for (const auto& result : results) {
      if (auto information = getMediaFromResult(result)) {
        currentProcessId_ = static_cast<int>(result.process.id);

        media_t media;
        media.information.push_back(std::move(*information));
        return result_t{result.player, std::move(media)};
      }
    }
  }

  currentProcessId_ = 0;
  return std::nullopt;
}
#endif
#endif

bool Detection::isMediaIdentified() const {
  return currentEpisode_.has_value() && currentEpisode_->animeId() != anime::kUnknownId;
}

bool Detection::isPlayerFocused() const {
#ifdef Q_OS_WINDOWS
  return currentWindowHandle_ && currentWindowHandle_ == static_cast<void*>(GetForegroundWindow());
#else
  // There is no reliable notion of focus here (e.g. Wayland, or media streamed to another device),
  // but we can tell whether Kodi is paused, which is what this setting is really about.
  return !playerPaused_;
#endif
}

void Detection::setCurrentEpisodeAnimeId(int animeId) {
  if (!currentEpisode_) return;

  currentEpisode_->setAnimeId(animeId);
  emit currentEpisodeChanged(currentEpisode_);
}

void Detection::reset() {
  currentPlayer_.reset();
  currentMedia_.reset();
  currentWindowHandle_ = nullptr;
#ifdef Q_OS_LINUX
  currentProcessId_ = 0;
#endif

  if (currentEpisode_) {
    currentEpisode_.reset();
    emit currentEpisodeChanged(std::nullopt);
  }
}

bool Detection::hasEpisodeChanged(const Episode& episode) const {
  if (!currentEpisode_) return true;
  if (currentEpisode_->animeId() != episode.animeId()) return true;

  return currentEpisode_->elements(anitomy::ElementKind::Episode) !=
         episode.elements(anitomy::ElementKind::Episode);
}

}  // namespace track::media
