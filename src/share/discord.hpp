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
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <optional>

class QLocalSocket;
class QTimer;

namespace share::discord {

struct Activity {
  QString details;
  QString state;
  QString largeImage;
  QString largeText;
  QString smallImage;
  QString smallText;
  qint64 startTimestamp = 0;  // seconds since epoch, 0 for none
  QString buttonLabel;
  QString buttonUrl;

  bool operator==(const Activity&) const = default;
};

// Discord's local RPC protocol, over the IPC socket that the Discord client creates (a Unix domain
// socket in $XDG_RUNTIME_DIR, or a named pipe on Windows). Taiga 1.x used the now-deprecated
// discord-rpc library for this; the protocol is simple enough to speak directly.
class Client final : public QObject {
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(Client)

public:
  explicit Client(QObject* parent);
  ~Client() override;

  void setActivity(const std::optional<Activity>& activity);
  bool isReady() const;

signals:
  void readyChanged(bool ready);

private:
  enum Opcode : quint32 {
    Handshake = 0,
    Frame = 1,
    Close = 2,
    Ping = 3,
    Pong = 4,
  };

  void connectToDiscord();
  void tryNextPath();
  void onConnected();
  void onReadyRead();
  void onDisconnected();
  void handleMessage(quint32 opcode, const QJsonObject& message);
  void send(quint32 opcode, const QJsonObject& payload);
  void sendActivity();
  QJsonObject activityJson() const;
  static QStringList candidatePaths();

  QLocalSocket* socket_ = nullptr;
  QTimer* reconnectTimer_ = nullptr;
  QByteArray buffer_;
  QStringList pendingPaths_;
  QString applicationId_;
  std::optional<Activity> activity_;
  bool ready_ = false;
  bool sendActivityType_ = true;
};

// Shows the anime that you're watching (as detected by Taiga) on your Discord profile
class Presence final : public QObject {
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(Presence)

public:
  explicit Presence(QObject* parent);

  void init();
  void refresh();

private:
  std::optional<Activity> currentActivity();

  Client* client_ = nullptr;
  QTimer* timer_ = nullptr;
  QString episodeKey_;
  qint64 startedAt_ = 0;
};

inline Presence* presence() {
  static auto presence = new Presence(qApp);
  return presence;
}

}  // namespace share::discord
