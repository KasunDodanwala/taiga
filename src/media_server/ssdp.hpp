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
#include <QObject>
#include <QString>

class QTimer;
class QUdpSocket;

namespace media_server {

// Simple Service Discovery Protocol: answers M-SEARCH requests from control points (e.g. VLC,
// Infuse, smart TVs) and announces the server with NOTIFY messages, so that it shows up on the
// network without any configuration.
class Ssdp final : public QObject {
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(Ssdp)

public:
  explicit Ssdp(QObject* parent);
  ~Ssdp() override;

  bool start(const QString& uuid, quint16 httpPort);
  void stop();
  bool isRunning() const;
  QString errorString() const;

  // Local IPv4 address in the same subnet as `peer`, used to build URLs that the peer can reach
  static QHostAddress localAddressFor(const QHostAddress& peer);

private:
  void onReadyRead();
  void respond(const QHostAddress& address, quint16 port, const QByteArray& searchTarget);
  void notifyAll(const QByteArray& subtype);
  QList<QByteArray> targets() const;
  QByteArray usn(const QByteArray& target) const;
  QByteArray location(const QHostAddress& address) const;

  QUdpSocket* socket_ = nullptr;  // receives multicast on port 1900
  QUdpSocket* sender_ = nullptr;  // sends announcements and responses
  QTimer* timer_ = nullptr;
  QByteArray uuid_;
  quint16 httpPort_ = 0;
  QString error_;
};

}  // namespace media_server
