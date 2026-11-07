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

#include "kodi.hpp"

#include <QJsonArray>
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

namespace track::kodi {

namespace {

QJsonObject call(int id, const QString& method, const QJsonObject& params = {}) {
  QJsonObject object{
      {"jsonrpc", "2.0"},
      {"id", id},
      {"method", method},
  };
  if (!params.isEmpty()) object["params"] = params;
  return object;
}

std::chrono::milliseconds parseTime(const QJsonValue& value) {
  using namespace std::chrono;
  const auto time = value.toObject();
  return hours{time.value("hours").toInt()} + minutes{time.value("minutes").toInt()} +
         seconds{time.value("seconds").toInt()} +
         milliseconds{time.value("milliseconds").toInt()};
}

QJsonObject findResponse(const QJsonArray& responses, int id) {
  for (const QJsonValue& response : responses) {
    const auto object = response.toObject();
    if (object.value("id").toInt() == id) return object;
  }
  return {};
}

QString errorMessage(const QJsonValue& response) {
  return response.toObject()
      .value("error")
      .toObject()
      .value("message")
      .toString(u"Unexpected response from Kodi"_s);
}

}  // namespace

Endpoint Endpoint::fromSettings() {
  return {
      .host = QString::fromStdString(taiga::settings.kodiHost()),
      .port = taiga::settings.kodiPort(),
      .username = QString::fromStdString(taiga::settings.kodiUsername()),
      .password = QString::fromStdString(taiga::settings.kodiPassword()),
  };
}

Client::Client(QObject* parent) : QObject(parent), manager_(new QNetworkAccessManager(this)) {
  // Kodi is normally on the same machine or the local network; never go through a proxy.
  manager_->setProxy(QNetworkProxy::NoProxy);
  manager_->setTransferTimeout(std::chrono::seconds{3});
  manager_->setAutoDeleteReplies(true);
}

bool Client::isEnabled() const {
  return taiga::settings.kodiEnabled();
}

bool Client::isReachable() const {
  return reachable_;
}

const std::optional<Playback>& Client::playback() const {
  return playback_;
}

void Client::poll() {
  if (pollInFlight_) return;

  if (!isEnabled()) {
    reachable_ = false;
    setPlayback(std::nullopt);
    return;
  }

  pollInFlight_ = true;

  const auto itemProperties = QJsonArray{"file", "title", "showtitle", "season", "episode"};
  const auto playerProperties = QJsonArray{"speed", "time", "totaltime"};

  // A batch request, so that a single round trip is enough for each poll.
  const QJsonArray batch{
      call(1, u"Player.GetActivePlayers"_s),
      call(2, u"Player.GetItem"_s,
           {{"playerid", videoPlayerId_}, {"properties", itemProperties}}),
      call(3, u"Player.GetProperties"_s,
           {{"playerid", videoPlayerId_}, {"properties", playerProperties}}),
  };

  send(Endpoint::fromSettings(), batch, [this](const QJsonValue& result, const QString& error) {
    pollInFlight_ = false;
    handlePollResult(result, error);
  });
}

void Client::handlePollResult(const QJsonValue& result, const QString& error) {
  if (!error.isEmpty()) {
    if (reachable_) qDebug() << "Kodi is no longer reachable:" << error;
    reachable_ = false;
    setPlayback(std::nullopt);
    return;
  }

  if (!reachable_) qDebug() << "Connected to Kodi";
  reachable_ = true;

  const auto responses = result.toArray();

  std::optional<int> videoPlayerId;
  for (const QJsonValue& value : findResponse(responses, 1).value("result").toArray()) {
    const auto player = value.toObject();
    if (player.value("type").toString() == u"video") {
      videoPlayerId = player.value("playerid").toInt();
      break;
    }
  }

  if (!videoPlayerId) {
    setPlayback(std::nullopt);
    return;
  }

  if (*videoPlayerId != videoPlayerId_) {
    // Item and properties were requested for another player, we'll ask again on the next poll.
    videoPlayerId_ = *videoPlayerId;
    return;
  }

  const auto item =
      findResponse(responses, 2).value("result").toObject().value("item").toObject();
  const auto properties = findResponse(responses, 3).value("result").toObject();

  setPlayback(Playback{
      .file = item.value("file").toString(),
      .label = item.value("label").toString(),
      .title = item.value("title").toString(),
      .showTitle = item.value("showtitle").toString(),
      .season = item.value("season").toInt(-1),
      .episode = item.value("episode").toInt(-1),
      .paused = properties.value("speed").toInt(1) == 0,
      .position = parseTime(properties.value("time")),
      .duration = parseTime(properties.value("totaltime")),
  });
}

void Client::setPlayback(std::optional<Playback> playback) {
  const auto isSame = [](const Playback& a, const Playback& b) {
    return a.file == b.file && a.label == b.label && a.paused == b.paused;
  };

  const bool changed = playback.has_value() != playback_.has_value() ||
                       (playback && !isSame(*playback, *playback_));

  playback_ = std::move(playback);

  if (changed) emit playbackChanged(playback_);
}

void Client::openFile(const QString& path, callback_t callback) {
  const auto params = QJsonObject{{"item", QJsonObject{{"file", path}}}};

  send(Endpoint::fromSettings(), call(1, u"Player.Open"_s, params),
       [callback = std::move(callback)](const QJsonValue& result, const QString& error) {
         if (!callback) return;
         if (!error.isEmpty()) return callback(false, error);
         if (result.toObject().value("result").toString() != u"OK") {
           return callback(false, errorMessage(result));
         }
         callback(true, {});
       });
}

void Client::testConnection(const Endpoint& endpoint, callback_t callback) {
  const auto params = QJsonObject{{"properties", QJsonArray{"name", "version"}}};

  send(endpoint, call(1, u"Application.GetProperties"_s, params),
       [callback = std::move(callback)](const QJsonValue& result, const QString& error) {
         if (!error.isEmpty()) return callback(false, error);
         const auto app = result.toObject().value("result").toObject();
         if (app.isEmpty()) return callback(false, errorMessage(result));
         const auto version = app.value("version").toObject();
         callback(true, u"Connected to %1 %2.%3"_s.arg(app.value("name").toString())
                            .arg(version.value("major").toInt())
                            .arg(version.value("minor").toInt()));
       });
}

QNetworkRequest Client::createRequest(const Endpoint& endpoint) const {
  QUrl url;
  url.setScheme(u"http"_s);
  url.setHost(endpoint.host);
  url.setPort(endpoint.port);
  url.setPath(u"/jsonrpc"_s);

  QNetworkRequest request{url};
  request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

  if (!endpoint.username.isEmpty() || !endpoint.password.isEmpty()) {
    const auto credentials = u"%1:%2"_s.arg(endpoint.username, endpoint.password).toUtf8();
    request.setRawHeader("Authorization", "Basic " + credentials.toBase64());
  }

  return request;
}

void Client::send(const Endpoint& endpoint, const QJsonValue& body, reply_callback_t callback) {
  const auto data = body.isArray() ? QJsonDocument{body.toArray()}.toJson(QJsonDocument::Compact)
                                   : QJsonDocument{body.toObject()}.toJson(QJsonDocument::Compact);

  auto reply = manager_->post(createRequest(endpoint), data);

  connect(reply, &QNetworkReply::finished, this, [reply, callback = std::move(callback)]() {
    const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    if (status == 401) {
      return callback({}, u"Kodi rejected the username or password"_s);
    }
    if (reply->error() != QNetworkReply::NoError) {
      return callback({}, reply->errorString());
    }

    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(reply->readAll(), &error);
    if (error.error != QJsonParseError::NoError) {
      return callback({}, u"Invalid response (is this Kodi's web server port?)"_s);
    }

    callback(document.isArray() ? QJsonValue{document.array()} : QJsonValue{document.object()}, {});
  });
}

}  // namespace track::kodi
