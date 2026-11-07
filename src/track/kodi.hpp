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

#pragma once

#include <QCoreApplication>
#include <QJsonValue>
#include <QObject>
#include <QString>
#include <chrono>
#include <functional>
#include <optional>

class QJsonDocument;
class QNetworkAccessManager;
class QNetworkReply;
class QNetworkRequest;

namespace track::kodi {

// What Kodi's video player is currently playing, as reported by its JSON-RPC API.
struct Playback {
  QString file;       // local path, or an URL (e.g. `smb://`, `upnp://`, `plugin://`)
  QString label;      // display label of the item
  QString title;      // title of the item (episode title for library items)
  QString showTitle;  // TV show title, for items in the video library
  int season = -1;
  int episode = -1;
  bool paused = false;
  std::chrono::milliseconds position{0};
  std::chrono::milliseconds duration{0};

  bool operator==(const Playback&) const = default;
};

struct Endpoint {
  QString host;
  int port = 0;
  QString username;
  QString password;

  static Endpoint fromSettings();
};

// Talks to Kodi over JSON-RPC (HTTP). Requires "Allow remote control via HTTP" in Kodi's
// Settings > Services > Control.
class Client final : public QObject {
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(Client)

public:
  using callback_t = std::function<void(bool success, const QString& message)>;

  explicit Client(QObject* parent);

  bool isEnabled() const;
  bool isReachable() const;
  const std::optional<Playback>& playback() const;

  // Asks Kodi what is playing. Results arrive asynchronously, see `playbackChanged`.
  void poll();

  void openFile(const QString& path, callback_t callback = {});
  void testConnection(const Endpoint& endpoint, callback_t callback);

signals:
  void playbackChanged(const std::optional<Playback>& playback);

private:
  using reply_callback_t = std::function<void(const QJsonValue& result, const QString& error)>;

  QNetworkRequest createRequest(const Endpoint& endpoint) const;
  void send(const Endpoint& endpoint, const QJsonValue& body, reply_callback_t callback);
  void handlePollResult(const QJsonValue& result, const QString& error);
  void setPlayback(std::optional<Playback> playback);

  QNetworkAccessManager* manager_ = nullptr;
  std::optional<Playback> playback_;
  bool pollInFlight_ = false;
  bool reachable_ = false;
  int videoPlayerId_ = 1;
};

inline Client* client() {
  static auto client = new Client(qApp);
  return client;
}

}  // namespace track::kodi
