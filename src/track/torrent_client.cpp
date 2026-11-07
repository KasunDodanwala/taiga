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

#include "torrent_client.hpp"

#include <QHttpMultiPart>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

#include "base/log.hpp"
#include "base/string.hpp"
#include "taiga/settings.hpp"

namespace track::torrent {

namespace {

QUrl apiUrl(const QBittorrent::Endpoint& endpoint, const QString& path) {
  auto base = endpoint.url.trimmed();
  if (!base.contains(u"://"_s)) base.prepend(u"http://"_s);
  while (base.endsWith(u'/')) base.chop(1);
  return QUrl{u"%1/api/v2/%2"_s.arg(base, path)};
}

int statusCode(const QNetworkReply* reply) {
  return reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
}

QHttpPart textPart(const QString& name, const QString& value) {
  QHttpPart part;
  part.setHeader(QNetworkRequest::ContentDispositionHeader,
                 u"form-data; name=\"%1\""_s.arg(name));
  part.setBody(value.toUtf8());
  return part;
}

QString networkError(const QNetworkReply* reply) {
  if (reply->error() == QNetworkReply::ConnectionRefusedError) {
    return u"qBittorrent is not running, or its Web UI is disabled (%1)"_s.arg(
        reply->url().authority());
  }
  return reply->errorString();
}

// Older versions reply with "Ok." / "Fails.", newer ones may return JSON or 204 No Content.
std::pair<bool, QString> addResult(QNetworkReply* reply) {
  const auto status = statusCode(reply);
  const auto body = reply->readAll().trimmed();

  if (status == 415) return {false, u"qBittorrent says the torrent file is not valid"_s};
  if (status == 409) return {false, u"The torrent is already in qBittorrent"_s};
  if (reply->error() != QNetworkReply::NoError && status < 200) {
    return {false, networkError(reply)};
  }
  if (status < 200 || status >= 300) {
    return {false, u"qBittorrent returned an error (%1 %2)"_s.arg(status).arg(
                       QString::fromUtf8(body).left(100))};
  }
  if (body == "Fails.") {
    return {false, u"qBittorrent rejected the torrent (it may already be added)"_s};
  }
  if (body.startsWith('{')) {
    const auto json = QJsonDocument::fromJson(body).object();
    if (json.value("failure_count").toInt() > 0 && json.value("success_count").toInt() == 0 &&
        json.value("pending_count").toInt() == 0) {
      return {false, u"qBittorrent rejected the torrent (it may already be added)"_s};
    }
  }
  return {true, {}};
}

}  // namespace

QBittorrent::Endpoint QBittorrent::Endpoint::fromSettings() {
  return {
      .url = QString::fromStdString(taiga::settings.torrentClientUrl()),
      .username = QString::fromStdString(taiga::settings.torrentClientUsername()),
      .password = QString::fromStdString(taiga::settings.torrentClientPassword()),
  };
}

QBittorrent::QBittorrent(QObject* parent) : QObject(parent), manager_(new QNetworkAccessManager(this)) {
  // The Web UI is usually on the same machine or the local network.
  manager_->setProxy(QNetworkProxy::NoProxy);
  manager_->setTransferTimeout(std::chrono::seconds{30});
  manager_->setAutoDeleteReplies(true);
  // The session cookie (`SID`, or `QBT_SID_<port>` in newer versions) is kept by the default
  // cookie jar of the network access manager.
}

void QBittorrent::add(const AddRequest& request, callback_t callback) {
  const auto endpoint = Endpoint::fromSettings();

  sendAdd(endpoint, request, [this, endpoint, request, callback](QNetworkReply* reply) {
    const auto status = statusCode(reply);

    // Not logged in (or the session has expired). With "Bypass authentication for clients on
    // localhost" enabled in qBittorrent, this never happens for local connections.
    if (status == 401 || status == 403) {
      login(endpoint, [this, endpoint, request, callback](bool success, const QString& message) {
        if (!success) return callback(false, message);
        sendAdd(endpoint, request, [callback](QNetworkReply* reply) {
          const auto [ok, error] = addResult(reply);
          callback(ok, error);
        });
      });
      return;
    }

    const auto [ok, error] = addResult(reply);
    callback(ok, error);
  });
}

void QBittorrent::testConnection(const Endpoint& endpoint, callback_t callback) {
  const auto describe = [](QNetworkReply* reply) {
    return u"Connected to qBittorrent %1"_s.arg(QString::fromUtf8(reply->readAll().trimmed()));
  };

  getVersion(endpoint, [this, endpoint, callback, describe](QNetworkReply* reply) {
    const auto status = statusCode(reply);
    if (status == 200) return callback(true, describe(reply));
    if (status != 401 && status != 403) {
      return callback(false, status ? u"Unexpected response (%1)"_s.arg(status) : networkError(reply));
    }
    login(endpoint, [this, endpoint, callback, describe](bool success, const QString& message) {
      if (!success) return callback(false, message);
      getVersion(endpoint, [callback, describe](QNetworkReply* reply) {
        if (statusCode(reply) == 200) return callback(true, describe(reply));
        callback(false, u"Unexpected response (%1)"_s.arg(statusCode(reply)));
      });
    });
  });
}

void QBittorrent::login(const Endpoint& endpoint, callback_t callback) {
  if (endpoint.username.isEmpty()) {
    return callback(false,
                    u"qBittorrent requires a login. Enter the Web UI username and password in "
                    u"Settings > Torrents, or enable \"Bypass authentication for clients on "
                    u"localhost\" in qBittorrent."_s);
  }

  QNetworkRequest request{apiUrl(endpoint, u"auth/login"_s)};
  request.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
  // Required by qBittorrent's CSRF protection when it validates the Referer/Origin headers
  request.setRawHeader("Referer", apiUrl(endpoint, {}).adjusted(QUrl::RemovePath).toEncoded());

  // `QUrlQuery` would leave "+" as is, which servers decode as a space.
  const auto body = "username=" + QUrl::toPercentEncoding(endpoint.username) +
                    "&password=" + QUrl::toPercentEncoding(endpoint.password);

  auto reply = manager_->post(request, body);

  connect(reply, &QNetworkReply::finished, this, [reply, callback]() {
    const auto status = statusCode(reply);
    const auto body = reply->readAll().trimmed();
    if (status == 403) {
      return callback(false, u"qBittorrent has banned this IP after too many failed logins"_s);
    }
    if (reply->error() != QNetworkReply::NoError && status == 0) {
      return callback(false, networkError(reply));
    }
    if ((status == 200 && body == "Ok.") || status == 204) {
      return callback(true, {});
    }
    callback(false, u"qBittorrent rejected the username or password"_s);
  });
}

void QBittorrent::sendAdd(const Endpoint& endpoint, const AddRequest& request,
                          reply_callback_t callback) {
  auto multiPart = new QHttpMultiPart(QHttpMultiPart::FormDataType);

  if (!request.torrentData.isEmpty()) {
    QHttpPart part;
    part.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-bittorrent");
    part.setHeader(QNetworkRequest::ContentDispositionHeader,
                   u"form-data; name=\"torrents\"; filename=\"%1\""_s.arg(
                       request.torrentFileName.isEmpty() ? u"file.torrent"_s
                                                         : request.torrentFileName));
    part.setBody(request.torrentData);
    multiPart->append(part);
  } else {
    multiPart->append(textPart(u"urls"_s, request.urls));
  }

  if (!request.savePath.isEmpty()) {
    multiPart->append(textPart(u"savepath"_s, request.savePath));
    // Automatic torrent management would ignore our save path in favor of the category's.
    multiPart->append(textPart(u"autoTMM"_s, u"false"_s));
  }
  if (!request.category.isEmpty()) multiPart->append(textPart(u"category"_s, request.category));
  if (!request.tags.isEmpty()) multiPart->append(textPart(u"tags"_s, request.tags));

  QNetworkRequest networkRequest{apiUrl(endpoint, u"torrents/add"_s)};
  networkRequest.setRawHeader("Referer",
                              apiUrl(endpoint, {}).adjusted(QUrl::RemovePath).toEncoded());

  auto reply = manager_->post(networkRequest, multiPart);
  multiPart->setParent(reply);

  connect(reply, &QNetworkReply::finished, this, [reply, callback]() { callback(reply); });
}

void QBittorrent::getVersion(const Endpoint& endpoint, reply_callback_t callback) {
  QNetworkRequest request{apiUrl(endpoint, u"app/version"_s)};
  auto reply = manager_->get(request);
  connect(reply, &QNetworkReply::finished, this, [reply, callback]() { callback(reply); });
}

}  // namespace track::torrent
