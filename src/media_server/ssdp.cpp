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

#include "ssdp.hpp"

#include <QMap>
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QRandomGenerator>
#include <QTimer>
#include <QUdpSocket>
#include <QVariant>
#include <algorithm>
#include <chrono>

#include "base/log.hpp"
#include "media_server/http_server.hpp"
#include "taiga/version.hpp"

namespace media_server {

namespace {

const QHostAddress kMulticastGroup{QStringLiteral("239.255.255.250")};
constexpr quint16 kSsdpPort = 1900;
constexpr int kMaxAge = 1800;

constexpr QByteArrayView kDeviceType = "urn:schemas-upnp-org:device:MediaServer:1";
constexpr QByteArrayView kContentDirectory = "urn:schemas-upnp-org:service:ContentDirectory:1";
constexpr QByteArrayView kConnectionManager = "urn:schemas-upnp-org:service:ConnectionManager:1";

QByteArray serverString() {
  return QByteArray{"Linux UPnP/1.0 DLNADOC/1.50 Taiga/"} +
         QByteArray::fromStdString(taiga::version().to_string());
}

QList<QNetworkInterface> multicastInterfaces() {
  QList<QNetworkInterface> interfaces;
  for (const auto& iface : QNetworkInterface::allInterfaces()) {
    const auto flags = iface.flags();
    if (!flags.testFlag(QNetworkInterface::IsUp) || !flags.testFlag(QNetworkInterface::IsRunning) ||
        !flags.testFlag(QNetworkInterface::CanMulticast) ||
        flags.testFlag(QNetworkInterface::IsLoopBack)) {
      continue;
    }
    for (const auto& entry : iface.addressEntries()) {
      if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol) {
        interfaces.append(iface);
        break;
      }
    }
  }
  return interfaces;
}

QHostAddress ipv4Address(const QNetworkInterface& iface) {
  for (const auto& entry : iface.addressEntries()) {
    if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol) return entry.ip();
  }
  return {};
}

QMap<QByteArray, QByteArray> parseHeaders(const QByteArray& message) {
  QMap<QByteArray, QByteArray> headers;
  const auto lines = message.split('\n');
  for (qsizetype i = 1; i < lines.size(); ++i) {
    const auto colon = lines[i].indexOf(':');
    if (colon <= 0) continue;
    headers.insert(lines[i].left(colon).trimmed().toUpper(), lines[i].mid(colon + 1).trimmed());
  }
  return headers;
}

}  // namespace

Ssdp::Ssdp(QObject* parent)
    : QObject(parent),
      socket_(new QUdpSocket(this)),
      sender_(new QUdpSocket(this)),
      timer_(new QTimer(this)) {
  connect(socket_, &QUdpSocket::readyRead, this, &Ssdp::onReadyRead);
  connect(timer_, &QTimer::timeout, this, [this]() { notifyAll("ssdp:alive"); });
}

Ssdp::~Ssdp() {
  stop();
}

bool Ssdp::start(const QString& uuid, quint16 httpPort) {
  stop();

  uuid_ = uuid.toLatin1();
  httpPort_ = httpPort;
  error_.clear();

  // Other UPnP software (e.g. Kodi) listens on the same port, which is fine for multicast.
  if (!socket_->bind(QHostAddress::AnyIPv4, kSsdpPort,
                     QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
    error_ = socket_->errorString();
    qWarning() << "SSDP: could not bind to port 1900:" << error_;
    return false;
  }

  bool joined = false;
  for (const auto& iface : multicastInterfaces()) {
    joined = socket_->joinMulticastGroup(kMulticastGroup, iface) || joined;
  }
  if (!joined) joined = socket_->joinMulticastGroup(kMulticastGroup);
  if (!joined) {
    error_ = socket_->errorString();
    qWarning() << "SSDP: could not join the multicast group:" << error_;
  }

  sender_->bind(QHostAddress::AnyIPv4, 0);
  sender_->setSocketOption(QAbstractSocket::MulticastTtlOption, QVariant{4});

  // Announce twice, as UDP is unreliable, and then periodically before the announcement expires.
  notifyAll("ssdp:alive");
  QTimer::singleShot(std::chrono::seconds{2}, this, [this]() {
    if (isRunning()) notifyAll("ssdp:alive");
  });
  timer_->start(std::chrono::seconds{kMaxAge / 3});

  return true;
}

void Ssdp::stop() {
  if (!isRunning()) return;
  notifyAll("ssdp:byebye");
  timer_->stop();
  socket_->close();
  sender_->close();
}

bool Ssdp::isRunning() const {
  return socket_->state() == QAbstractSocket::BoundState;
}

QString Ssdp::errorString() const {
  return error_;
}

QHostAddress Ssdp::localAddressFor(const QHostAddress& peer) {
  const auto peerAddress = QHostAddress{peer.toIPv4Address()};
  QHostAddress fallback;

  for (const auto& iface : QNetworkInterface::allInterfaces()) {
    if (!iface.flags().testFlag(QNetworkInterface::IsUp)) continue;
    for (const auto& entry : iface.addressEntries()) {
      const auto ip = entry.ip();
      if (ip.protocol() != QAbstractSocket::IPv4Protocol) continue;
      if (peerAddress.isInSubnet(ip, entry.prefixLength())) return ip;
      if (fallback.isNull() && !ip.isLoopback()) fallback = ip;
    }
  }

  return fallback;
}

QList<QByteArray> Ssdp::targets() const {
  return {
      "upnp:rootdevice",
      "uuid:" + uuid_,
      kDeviceType.toByteArray(),
      kContentDirectory.toByteArray(),
      kConnectionManager.toByteArray(),
  };
}

QByteArray Ssdp::usn(const QByteArray& target) const {
  if (target.startsWith("uuid:")) return target;
  return "uuid:" + uuid_ + "::" + target;
}

QByteArray Ssdp::location(const QHostAddress& address) const {
  return "http://" + address.toString().toLatin1() + ':' + QByteArray::number(httpPort_) +
         "/description.xml";
}

void Ssdp::onReadyRead() {
  while (socket_->hasPendingDatagrams()) {
    const auto datagram = socket_->receiveDatagram();
    const auto data = datagram.data();
    if (!data.startsWith("M-SEARCH")) continue;

    const auto headers = parseHeaders(data);
    if (headers.value("MAN").replace('"', "") != "ssdp:discover") continue;

    const auto searchTarget = headers.value("ST");
    QList<QByteArray> matches;
    if (searchTarget == "ssdp:all") {
      matches = targets();
    } else if (searchTarget == "urn:schemas-upnp-org:device:MediaServer:2") {
      matches = {kDeviceType.toByteArray()};  // we only implement version 1
    } else if (targets().contains(searchTarget)) {
      matches = {searchTarget};
    }
    if (matches.isEmpty()) continue;

    // Responses are spread over the time allowed by the requester (MX), as the spec asks.
    const int mx = std::clamp(headers.value("MX").toInt(), 1, 5);
    const auto address = datagram.senderAddress();
    const auto port = static_cast<quint16>(datagram.senderPort());

    for (const auto& target : matches) {
      const auto delay = QRandomGenerator::global()->bounded(mx * 1000 / 2);
      QTimer::singleShot(std::chrono::milliseconds{delay}, this, [this, address, port, target]() {
        if (isRunning()) respond(address, port, target);
      });
    }
  }
}

void Ssdp::respond(const QHostAddress& address, quint16 port, const QByteArray& searchTarget) {
  const auto local = localAddressFor(address);
  if (local.isNull()) return;

  QByteArray response;
  response += "HTTP/1.1 200 OK\r\n";
  response += "CACHE-CONTROL: max-age=" + QByteArray::number(kMaxAge) + "\r\n";
  response += "DATE: " + httpDate() + "\r\n";
  response += "EXT:\r\n";
  response += "LOCATION: " + location(local) + "\r\n";
  response += "SERVER: " + serverString() + "\r\n";
  response += "ST: " + searchTarget + "\r\n";
  response += "USN: " + usn(searchTarget) + "\r\n";
  response += "Content-Length: 0\r\n\r\n";

  sender_->writeDatagram(response, address, port);
}

void Ssdp::notifyAll(const QByteArray& subtype) {
  for (const auto& iface : multicastInterfaces()) {
    const auto local = ipv4Address(iface);
    if (local.isNull()) continue;

    sender_->setMulticastInterface(iface);

    for (const auto& target : targets()) {
      QByteArray message;
      message += "NOTIFY * HTTP/1.1\r\n";
      message += "HOST: 239.255.255.250:1900\r\n";
      message += "NT: " + target + "\r\n";
      message += "NTS: " + subtype + "\r\n";
      message += "USN: " + usn(target) + "\r\n";
      if (subtype == "ssdp:alive") {
        message += "CACHE-CONTROL: max-age=" + QByteArray::number(kMaxAge) + "\r\n";
        message += "LOCATION: " + location(local) + "\r\n";
        message += "SERVER: " + serverString() + "\r\n";
      }
      message += "\r\n";
      sender_->writeDatagram(message, kMulticastGroup, kSsdpPort);
    }
  }
}

}  // namespace media_server
