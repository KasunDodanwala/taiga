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

#include "http_server.hpp"

#include <QDateTime>
#include <QLocale>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>

#include "taiga/version.hpp"

namespace media_server {

namespace {

constexpr qsizetype kMaxHeaderSize = 64 * 1024;
constexpr qsizetype kMaxBodySize = 1024 * 1024;

QByteArray serverHeader() {
  return QByteArray{"Linux UPnP/1.0 DLNADOC/1.50 Taiga/"} +
         QByteArray::fromStdString(taiga::version().to_string());
}

}  // namespace

QByteArray statusText(int status) {
  switch (status) {
    case 200:
      return "OK";
    case 206:
      return "Partial Content";
    case 400:
      return "Bad Request";
    case 403:
      return "Forbidden";
    case 404:
      return "Not Found";
    case 405:
      return "Method Not Allowed";
    case 406:
      return "Not Acceptable";
    case 412:
      return "Precondition Failed";
    case 413:
      return "Payload Too Large";
    case 416:
      return "Range Not Satisfiable";
    case 500:
      return "Internal Server Error";
    case 501:
      return "Not Implemented";
    case 503:
      return "Service Unavailable";
    default:
      return "Unknown";
  }
}

QByteArray httpDate() {
  return QLocale::c()
      .toString(QDateTime::currentDateTimeUtc(), u"ddd, dd MMM yyyy hh:mm:ss 'GMT'")
      .toLatin1();
}

////////////////////////////////////////////////////////////////////////////////

QByteArray HttpRequest::header(const QByteArray& name) const {
  return headers.value(name.toLower());
}

QString HttpRequest::path() const {
  auto rawPath = target;
  if (const auto query = rawPath.indexOf('?'); query >= 0) rawPath.truncate(query);
  return QUrl::fromPercentEncoding(rawPath);
}

////////////////////////////////////////////////////////////////////////////////

HttpConnection::HttpConnection(QTcpSocket* socket, HttpServer* server)
    : QObject(server), socket_(socket), server_(server) {
  socket->setParent(this);
  connect(socket, &QTcpSocket::readyRead, this, &HttpConnection::onReadyRead);
  connect(socket, &QTcpSocket::bytesWritten, this, &HttpConnection::bytesWritten);
  connect(socket, &QTcpSocket::disconnected, this, [this]() {
    emit closed();
    deleteLater();
  });
}

HttpConnection::~HttpConnection() = default;

void HttpConnection::respond(int status, const QByteArray& contentType, const QByteArray& body,
                             const HttpHeaders& headers, bool headOnly) {
  HttpHeaders allHeaders = headers;
  if (!contentType.isEmpty()) allHeaders.append({"Content-Type", contentType});
  allHeaders.append({"Content-Length", QByteArray::number(body.size())});
  beginResponse(status, allHeaders);
  if (!headOnly) write(body);
  endResponse();
}

void HttpConnection::beginResponse(int status, const HttpHeaders& headers, bool closeWhenDone) {
  if (!socket_) return;

  closeAfterResponse_ = closeWhenDone || !keepAlive_;

  QByteArray head;
  head.reserve(512);
  head += "HTTP/1.1 " + QByteArray::number(status) + ' ' + statusText(status) + "\r\n";
  head += "Date: " + httpDate() + "\r\n";
  head += "Server: " + serverHeader() + "\r\n";
  for (const auto& [name, value] : headers) {
    head += name + ": " + value + "\r\n";
  }
  head += closeAfterResponse_ ? "Connection: close\r\n" : "Connection: keep-alive\r\n";
  head += "\r\n";

  socket_->write(head);
}

void HttpConnection::write(const QByteArray& data) {
  if (socket_ && !data.isEmpty()) socket_->write(data);
}

void HttpConnection::endResponse() {
  busy_ = false;
  if (!socket_) return;

  if (closeAfterResponse_) {
    socket_->disconnectFromHost();
    return;
  }

  // Handle pipelined requests, if any
  if (!buffer_.isEmpty()) {
    QTimer::singleShot(0, this, &HttpConnection::onReadyRead);
  }
}

void HttpConnection::close() {
  if (socket_) socket_->disconnectFromHost();
}

qint64 HttpConnection::bytesToWrite() const {
  return socket_ ? socket_->bytesToWrite() : 0;
}

QHostAddress HttpConnection::peerAddress() const {
  return socket_ ? socket_->peerAddress() : QHostAddress{};
}

bool HttpConnection::isOpen() const {
  return socket_ && socket_->state() == QAbstractSocket::ConnectedState;
}

void HttpConnection::onReadyRead() {
  if (!socket_) return;

  buffer_ += socket_->readAll();

  while (!busy_ && socket_ && parseRequest()) {
  }
}

bool HttpConnection::parseRequest() {
  const auto headerEnd = buffer_.indexOf("\r\n\r\n");
  if (headerEnd < 0) {
    if (buffer_.size() > kMaxHeaderSize) close();
    return false;
  }

  const auto lines = buffer_.left(headerEnd).split('\n');
  const auto requestLine = lines.value(0).trimmed().split(' ');
  if (requestLine.size() != 3) {
    buffer_.clear();
    busy_ = true;
    keepAlive_ = false;
    respond(400, "text/plain", "Bad Request");
    return false;
  }

  HttpRequest request{
      .method = requestLine[0].toUpper(),
      .target = requestLine[1],
      .version = requestLine[2].toUpper(),
      .headers = {},
      .body = {},
      .peerAddress = socket_->peerAddress(),
      .localAddress = socket_->localAddress(),
      .localPort = socket_->localPort(),
  };

  for (qsizetype i = 1; i < lines.size(); ++i) {
    const auto& line = lines[i];
    const auto colon = line.indexOf(':');
    if (colon <= 0) continue;
    const auto name = line.left(colon).trimmed().toLower();
    const auto value = line.mid(colon + 1).trimmed();
    if (request.headers.contains(name)) {
      request.headers[name] += ", " + value;
    } else {
      request.headers.insert(name, value);
    }
  }

  const auto contentLength = request.header("content-length").toLongLong();
  if (contentLength < 0 || contentLength > kMaxBodySize) {
    buffer_.clear();
    busy_ = true;
    keepAlive_ = false;
    respond(413, "text/plain", "Payload Too Large");
    return false;
  }

  const auto requestSize = headerEnd + 4 + contentLength;
  if (buffer_.size() < requestSize) return false;  // wait for the rest of the body

  request.body = buffer_.mid(headerEnd + 4, contentLength);
  buffer_.remove(0, requestSize);

  const auto connection = request.header("connection").toLower();
  keepAlive_ = request.version == "HTTP/1.1" ? !connection.contains("close")
                                             : connection.contains("keep-alive");

  busy_ = true;
  server_->handle(this, request);
  return true;
}

////////////////////////////////////////////////////////////////////////////////

HttpServer::HttpServer(QObject* parent) : QObject(parent), server_(new QTcpServer(this)) {
  connect(server_, &QTcpServer::newConnection, this, &HttpServer::onNewConnection);
}

bool HttpServer::listen(quint16 port) {
  if (server_->isListening()) server_->close();
  return server_->listen(QHostAddress::AnyIPv4, port);
}

void HttpServer::close() {
  server_->close();
}

bool HttpServer::isListening() const {
  return server_->isListening();
}

quint16 HttpServer::port() const {
  return server_->serverPort();
}

QString HttpServer::errorString() const {
  return server_->errorString();
}

void HttpServer::setHandler(handler_t handler) {
  handler_ = std::move(handler);
}

void HttpServer::setClientFilter(filter_t filter) {
  filter_ = std::move(filter);
}

void HttpServer::onNewConnection() {
  while (auto socket = server_->nextPendingConnection()) {
    if (filter_ && !filter_(socket->peerAddress())) {
      socket->abort();
      socket->deleteLater();
      continue;
    }
    new HttpConnection(socket, this);
  }
}

void HttpServer::handle(HttpConnection* connection, const HttpRequest& request) {
  if (handler_) {
    handler_(connection, request);
  } else {
    connection->respond(503, "text/plain", "Service Unavailable");
  }
}

}  // namespace media_server
