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
#include <QHash>
#include <QHostAddress>
#include <QObject>
#include <QString>
#include <chrono>
#include <optional>
#include <vector>

#include "media_server/library.hpp"

class QNetworkAccessManager;
class QTimer;

namespace media_server {

class HttpConnection;
class HttpServer;
class MediaInfoCache;
class Ssdp;
struct HttpRequest;

// A file that a client is streaming. All requests of a client for the same file (e.g. range
// requests while seeking) count as one stream.
struct Stream {
  QString path;
  QHostAddress client;
  std::chrono::steady_clock::time_point started;
  std::chrono::steady_clock::time_point lastActivity;  // last time data was sent
  int connections = 0;
  qint64 bytesSent = 0;
  bool transcoded = false;
  bool paused = false;  // still connected, but the client stopped reading
};

// A UPnP AV / DLNA media server (DMS) that shares the library folders. Files are streamed as they
// are by default; transcoding with FFmpeg (in software, or with VA-API) is optional. As the server
// knows what is being streamed, it also serves as a source for media detection.
class Server final : public QObject {
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(Server)

public:
  explicit Server(QObject* parent);
  ~Server() override;

  void init();
  void restart();  // applies settings
  void stop();

  bool isRunning() const;
  QString statusText() const;

  // The file that is being watched through the server, if any
  std::optional<Stream> currentStream() const;

  // Called by streaming responses
  void streamStarted(const QString& key, const QString& path, const QHostAddress& client,
                     bool transcoded);
  void streamProgress(const QString& key, qint64 bytes);
  void streamFinished(const QString& key);

signals:
  void statusChanged();

private:
  void handle(HttpConnection* connection, const HttpRequest& request);
  void handleDescription(HttpConnection* connection, const HttpRequest& request);
  void handleControl(HttpConnection* connection, const HttpRequest& request);
  void handleEvent(HttpConnection* connection, const HttpRequest& request);
  void handleIcon(HttpConnection* connection, const HttpRequest& request);
  void handleMedia(HttpConnection* connection, const HttpRequest& request);
  void handleTranscode(HttpConnection* connection, const HttpRequest& request);

  QByteArray browse(const QMap<QString, QString>& arguments, const HttpRequest& request,
                    int& errorCode);
  QString didl(const std::vector<Object>& objects, const HttpRequest& request);
  QString baseUrl(const HttpRequest& request) const;
  void sendInitialEvent(const QByteArray& callback, const QByteArray& sid, bool contentDirectory);
  void cleanUpStreams();
  bool isAllowedClient(const QHostAddress& address) const;

  HttpServer* http_ = nullptr;
  Ssdp* ssdp_ = nullptr;
  MediaInfoCache* mediaInfo_ = nullptr;
  QNetworkAccessManager* network_ = nullptr;
  QTimer* cleanupTimer_ = nullptr;
  Library library_;
  QHash<QString, Stream> streams_;
  QString uuid_;
  QString error_;
  QString warning_;
  quint32 systemUpdateId_ = 1;
};

inline Server* server() {
  static auto server = new Server(qApp);
  return server;
}

}  // namespace media_server
