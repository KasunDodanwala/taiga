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

#include "discord.hpp"

#include <QFile>

#include <QDateTime>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QTimer>
#include <QUuid>
#include <QtEndian>
#include <chrono>

#include "base/log.hpp"
#include "base/string.hpp"
#include "media/anime.hpp"
#include "media/anime_db.hpp"
#include "media/anime_list.hpp"
#include "media/anime_utils.hpp"
#include "sync/service.hpp"
#include "taiga/accounts.hpp"
#include "taiga/settings.hpp"
#include "track/episode.hpp"
#include "track/media.hpp"

namespace share::discord {

namespace {

using namespace std::chrono_literals;

// Discord rejects fields that are shorter than 2 or longer than 128 characters.
QString fit(QString text) {
  text = text.simplified();
  if (text.size() > 128) text = text.left(127) + u'…';
  if (text.size() == 1) text += u' ';
  return text;
}

}  // namespace

Client::Client(QObject* parent)
    : QObject(parent), socket_(new QLocalSocket(this)), reconnectTimer_(new QTimer(this)) {
  connect(socket_, &QLocalSocket::connected, this, &Client::onConnected);
  connect(socket_, &QLocalSocket::readyRead, this, &Client::onReadyRead);
  connect(socket_, &QLocalSocket::disconnected, this, &Client::onDisconnected);
  connect(socket_, &QLocalSocket::errorOccurred, this, [this]() {
    if (!ready_ && socket_->state() != QLocalSocket::ConnectedState) tryNextPath();
  });

  // Discord may not be running yet, or may be restarted.
  reconnectTimer_->setInterval(15s);
  connect(reconnectTimer_, &QTimer::timeout, this, &Client::connectToDiscord);
}

Client::~Client() {
  if (ready_) {
    activity_.reset();
    sendActivity();
    socket_->flush();
  }
}

bool Client::isReady() const {
  return ready_;
}

void Client::setActivity(const std::optional<Activity>& activity) {
  const auto applicationId = QString::fromStdString(taiga::settings.discordApplicationId());
  if (applicationId != applicationId_ && socket_->state() != QLocalSocket::UnconnectedState) {
    // Reconnect with the new application
    socket_->abort();
  }
  applicationId_ = applicationId;

  if (activity == activity_ && (ready_ || !activity)) return;
  activity_ = activity;

  if (ready_) {
    sendActivity();
  } else if (activity_) {
    connectToDiscord();
    reconnectTimer_->start();
  }
}

void Client::connectToDiscord() {
  if (socket_->state() != QLocalSocket::UnconnectedState) return;
  if (!activity_) {
    reconnectTimer_->stop();
    return;
  }
  pendingPaths_ = candidatePaths();
  tryNextPath();
}

void Client::tryNextPath() {
  if (socket_->state() != QLocalSocket::UnconnectedState) socket_->abort();
  if (pendingPaths_.isEmpty()) return;  // not running; try again later
  socket_->connectToServer(pendingPaths_.takeFirst());
}

void Client::onConnected() {
  pendingPaths_.clear();
  buffer_.clear();
  sendActivityType_ = true;
  send(Handshake, QJsonObject{{"v", 1}, {"client_id", applicationId_}});
}

void Client::onDisconnected() {
  const bool wasReady = ready_;
  ready_ = false;
  buffer_.clear();
  if (wasReady) {
    qDebug() << "Discord: disconnected";
    emit readyChanged(false);
  }
  if (activity_) reconnectTimer_->start();
}

void Client::onReadyRead() {
  buffer_ += socket_->readAll();

  // Each message: opcode (uint32), length (uint32), then a JSON payload of that length
  while (buffer_.size() >= 8) {
    const auto opcode = qFromLittleEndian<quint32>(buffer_.constData());
    const auto length = qFromLittleEndian<quint32>(buffer_.constData() + 4);
    if (length > 64 * 1024) {
      socket_->abort();
      return;
    }
    if (buffer_.size() < 8 + static_cast<qsizetype>(length)) return;

    const auto payload = buffer_.mid(8, length);
    buffer_.remove(0, 8 + length);
    handleMessage(opcode, QJsonDocument::fromJson(payload).object());
  }
}

void Client::handleMessage(quint32 opcode, const QJsonObject& message) {
  switch (opcode) {
    case Ping:
      send(Pong, message);
      return;
    case Close:
      qDebug() << "Discord: connection closed:" << message.value("message").toString();
      socket_->disconnectFromServer();
      return;
    case Frame:
      break;
    default:
      return;
  }

  const auto event = message.value("evt").toString();

  if (message.value("cmd").toString() == u"DISPATCH" && event == u"READY") {
    ready_ = true;
    reconnectTimer_->stop();
    const auto user = message.value("data").toObject().value("user").toObject();
    qDebug() << "Discord: connected as" << user.value("username").toString();
    emit readyChanged(true);
    sendActivity();
    return;
  }

  // Discord echoes the activity it accepted
  if (message.value("cmd").toString() == u"SET_ACTIVITY" && event.isEmpty()) {
    const auto data = message.value("data").toObject();
    if (data.isEmpty()) {
      qDebug() << "Discord: activity cleared";
    } else {
      qDebug() << "Discord: activity set:" << data.value("details").toString() << "|"
               << data.value("state").toString();
    }
    return;
  }

  if (event == u"ERROR") {
    const auto error = message.value("data").toObject().value("message").toString();
    qWarning() << "Discord:" << error;
    // Older clients don't accept an activity type; fall back to "Playing".
    if (sendActivityType_ && message.value("cmd").toString() == u"SET_ACTIVITY") {
      sendActivityType_ = false;
      sendActivity();
    }
  }
}

void Client::send(quint32 opcode, const QJsonObject& payload) {
  const auto json = QJsonDocument{payload}.toJson(QJsonDocument::Compact);
  QByteArray header(8, Qt::Uninitialized);
  qToLittleEndian<quint32>(opcode, header.data());
  qToLittleEndian<quint32>(static_cast<quint32>(json.size()), header.data() + 4);
  socket_->write(header + json);
}

void Client::sendActivity() {
  if (!ready_) return;

  QJsonObject args{{"pid", static_cast<qint64>(QCoreApplication::applicationPid())}};
  args.insert("activity", activity_ ? QJsonValue{activityJson()} : QJsonValue{QJsonValue::Null});

  send(Frame, QJsonObject{
                  {"cmd", "SET_ACTIVITY"},
                  {"args", args},
                  {"nonce", QUuid::createUuid().toString(QUuid::WithoutBraces)},
              });
}

QJsonObject Client::activityJson() const {
  QJsonObject activity;
  if (sendActivityType_) activity.insert("type", 3);  // "Watching"
  if (!activity_->details.isEmpty()) activity.insert("details", fit(activity_->details));
  if (!activity_->state.isEmpty()) activity.insert("state", fit(activity_->state));

  if (activity_->startTimestamp > 0) {
    activity.insert("timestamps", QJsonObject{{"start", activity_->startTimestamp}});
  }

  QJsonObject assets;
  if (!activity_->largeImage.isEmpty()) assets.insert("large_image", activity_->largeImage);
  if (!activity_->largeText.isEmpty()) assets.insert("large_text", fit(activity_->largeText));
  if (!activity_->smallImage.isEmpty()) assets.insert("small_image", activity_->smallImage);
  if (!activity_->smallText.isEmpty()) assets.insert("small_text", fit(activity_->smallText));
  if (!assets.isEmpty()) activity.insert("assets", assets);

  if (!activity_->buttonLabel.isEmpty() && activity_->buttonUrl.startsWith(u"https://"_s)) {
    activity.insert("buttons", QJsonArray{QJsonObject{
                                   {"label", activity_->buttonLabel},
                                   {"url", activity_->buttonUrl},
                               }});
  }

  return activity;
}

QStringList Client::candidatePaths() {
  QStringList paths;
#ifdef Q_OS_WINDOWS
  for (int i = 0; i < 10; ++i) paths.append(u"discord-ipc-%1"_s.arg(i));
#else
  // Native packages, then Flatpak and Snap, which put the socket in their own directories
  QStringList directories;
  for (const auto& variable : {"XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP"}) {
    if (const auto value = qEnvironmentVariable(variable); !value.isEmpty()) {
      directories.append(value);
    }
  }
  directories.append(u"/tmp"_s);
  directories.removeDuplicates();

  for (const auto& directory : directories) {
    for (const auto& subdirectory :
         {u""_s, u"/app/com.discordapp.Discord"_s, u"/snap.discord"_s, u"/.flatpak/dev.vencord.Vesktop/xdg-run"_s}) {
      for (int i = 0; i < 10; ++i) {
        const auto path = u"%1%2/discord-ipc-%3"_s.arg(directory, subdirectory).arg(i);
        if (QFile::exists(path)) paths.append(path);
      }
    }
  }
#endif
  return paths;
}

////////////////////////////////////////////////////////////////////////////////

Presence::Presence(QObject* parent)
    : QObject(parent), client_(new Client(this)), timer_(new QTimer(this)) {
  // Also catches changes that have no signal of their own (e.g. pausing in Kodi)
  timer_->setInterval(5s);
  connect(timer_, &QTimer::timeout, this, &Presence::refresh);
}

void Presence::init() {
  connect(track::media::detection(), &track::media::Detection::currentEpisodeChanged, this,
          &Presence::refresh);
  connect(&anime::db, &anime::Database::entryUpdated, this, &Presence::refresh);
  timer_->start();
  refresh();
}

void Presence::refresh() {
  client_->setActivity(currentActivity());
}

std::optional<Activity> Presence::currentActivity() {
  if (!taiga::settings.discordEnabled()) return std::nullopt;

  const auto episode = track::media::detection()->getCurrentEpisode();
  if (!episode) {
    episodeKey_.clear();
    return std::nullopt;
  }

  // Like Taiga 1.x, only recognized anime are shared, and never those marked as private.
  const auto* item = anime::db.item(episode->animeId());
  if (!item) return std::nullopt;
  if (const auto* entry = anime::db.entry(item->id); entry && entry->is_private) {
    return std::nullopt;
  }

  // Elapsed time counts from when the episode was first detected
  const auto range = episode->episodeNumberRange();
  const auto key = u"%1:%2"_s.arg(item->id).arg(range ? range->second : 0);
  if (key != episodeKey_) {
    episodeKey_ = key;
    startedAt_ = QDateTime::currentSecsSinceEpoch();
  }

  const auto title = QString::fromStdString(anime::preferredTitle(*item));

  QString state;
  if (range) {
    state = range->second > range->first
                ? u"Episodes %1-%2"_s.arg(range->first).arg(range->second)
                : u"Episode %1"_s.arg(range->second);
    if (item->episode_count > 0) state += u"/%1"_s.arg(item->episode_count);
  } else if (item->episode_count == 1) {
    state = u"Movie"_s;
  }
  if (taiga::settings.discordShowGroup()) {
    const auto group = episode->element(anitomy::ElementKind::ReleaseGroup);
    if (!group.empty()) state += u" by %1"_s.arg(QString::fromStdString(group));
  }
  const bool paused = !track::media::detection()->isPlayerFocused();
  if (paused) state = state.isEmpty() ? u"Paused"_s : u"Paused · %1"_s.arg(state);
  if (state.trimmed().isEmpty()) state = u"Watching"_s;

  const auto serviceId = sync::currentServiceId();
  const auto serviceName = sync::serviceName(serviceId);
  const auto serviceSlug = sync::serviceSlug(serviceId);
  const auto username =
      QString::fromStdString(taiga::accounts.serviceUsername(serviceSlug.toStdString()));

  Activity activity{
      .details = title,
      .state = state,
      .largeImage = item->image_url.empty() ? u"default"_s : QString::fromStdString(item->image_url),
      .largeText = title,
      .smallImage = serviceSlug,
      .smallText = taiga::settings.discordShowUsername() && !username.isEmpty()
                       ? u"%1 at %2"_s.arg(username, serviceName)
                       : serviceName,
      .startTimestamp = taiga::settings.discordShowTime() && !paused ? startedAt_ : 0,
      .buttonLabel = taiga::settings.discordShowButton() ? u"View Anime"_s : QString{},
      .buttonUrl = taiga::settings.discordShowButton() ? sync::animePageUrl(item->id) : QString{},
  };

  return activity;
}

}  // namespace share::discord
