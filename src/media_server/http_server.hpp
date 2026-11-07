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
#include <QHostAddress>
#include <QList>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QString>
#include <functional>
#include <utility>

class QTcpServer;
class QTcpSocket;

namespace media_server {

using HttpHeaders = QList<std::pair<QByteArray, QByteArray>>;

struct HttpRequest {
  QByteArray method;
  QByteArray target;  // path and query, as sent by the client
  QByteArray version;
  QMap<QByteArray, QByteArray> headers;  // names are lowercase
  QByteArray body;
  QHostAddress peerAddress;
  QHostAddress localAddress;
  quint16 localPort = 0;

  QByteArray header(const QByteArray& name) const;
  QString path() const;  // decoded, without the query
};

class HttpServer;

// A client connection. Requests are handled one at a time; a handler either replies right away
// with `respond`, or starts a response with `beginResponse`, writes the body with `write` (e.g.
// while streaming a file), and calls `endResponse` when done.
class HttpConnection final : public QObject {
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(HttpConnection)

public:
  HttpConnection(QTcpSocket* socket, HttpServer* server);
  ~HttpConnection() override;

  void respond(int status, const QByteArray& contentType, const QByteArray& body,
               const HttpHeaders& headers = {}, bool headOnly = false);
  void beginResponse(int status, const HttpHeaders& headers, bool closeWhenDone = false);
  void write(const QByteArray& data);
  void endResponse();
  void close();

  qint64 bytesToWrite() const;
  QHostAddress peerAddress() const;
  bool isOpen() const;

signals:
  void bytesWritten();
  void closed();

private:
  void onReadyRead();
  bool parseRequest();

  QPointer<QTcpSocket> socket_;
  HttpServer* server_ = nullptr;
  QByteArray buffer_;
  bool busy_ = false;
  bool keepAlive_ = true;
  bool closeAfterResponse_ = false;
};

class HttpServer final : public QObject {
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(HttpServer)

public:
  using handler_t = std::function<void(HttpConnection* connection, const HttpRequest& request)>;
  using filter_t = std::function<bool(const QHostAddress& address)>;

  explicit HttpServer(QObject* parent);

  bool listen(quint16 port);
  void close();
  bool isListening() const;
  quint16 port() const;
  QString errorString() const;

  void setHandler(handler_t handler);
  void setClientFilter(filter_t filter);

private:
  friend class HttpConnection;

  void onNewConnection();
  void handle(HttpConnection* connection, const HttpRequest& request);

  QTcpServer* server_ = nullptr;
  handler_t handler_;
  filter_t filter_;
};

QByteArray statusText(int status);
QByteArray httpDate();

}  // namespace media_server
