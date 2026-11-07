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

#include <QByteArray>
#include <QCoreApplication>
#include <QObject>
#include <QString>
#include <functional>

class QNetworkAccessManager;
class QNetworkReply;

namespace track::torrent {

struct AddRequest {
  QString urls;             // magnet link or URL of a .torrent file
  QByteArray torrentData;   // contents of a .torrent file (takes precedence over `urls`)
  QString torrentFileName;  // e.g. "[Group] Title - 01.torrent"
  QString savePath;         // empty for the client's default
  QString category;
  QString tags;
};

// qBittorrent's Web API (v2), which is available in both the GUI version (when "Web User Interface"
// is enabled) and `qbittorrent-nox`. Taiga 1.x used to launch uTorrent and others with command
// line arguments; talking to the Web API also works with headless clients and remote machines.
class QBittorrent final : public QObject {
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(QBittorrent)

public:
  struct Endpoint {
    QString url;  // e.g. "http://127.0.0.1:8080"
    QString username;
    QString password;

    static Endpoint fromSettings();
  };

  using callback_t = std::function<void(bool success, const QString& message)>;

  explicit QBittorrent(QObject* parent);

  void add(const AddRequest& request, callback_t callback);
  void testConnection(const Endpoint& endpoint, callback_t callback);

private:
  using reply_callback_t = std::function<void(QNetworkReply* reply)>;

  void login(const Endpoint& endpoint, callback_t callback);
  void sendAdd(const Endpoint& endpoint, const AddRequest& request, reply_callback_t callback);
  void getVersion(const Endpoint& endpoint, reply_callback_t callback);

  QNetworkAccessManager* manager_ = nullptr;
};

inline QBittorrent* qbittorrent() {
  static auto client = new QBittorrent(qApp);
  return client;
}

}  // namespace track::torrent
