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

#include "server.hpp"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QProcess>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>
#include <algorithm>
#include <tuple>

#ifdef Q_OS_UNIX
#include <signal.h>
#include <sys/types.h>
#endif

#include "base/log.hpp"
#include "base/string.hpp"
#include "media_server/http_server.hpp"
#include "media_server/media_info.hpp"
#include "media_server/ssdp.hpp"
#include "taiga/settings.hpp"
#include "taiga/version.hpp"

namespace media_server {

namespace {

using namespace std::chrono_literals;
using clock = std::chrono::steady_clock;

constexpr auto kMinimumStreamTime = 5s;   // shorter streams are probes (metadata, thumbnails)
constexpr auto kStreamGracePeriod = 30s;  // clients reconnect, e.g. when seeking
constexpr auto kPausedAfter = 20s;        // connected, but no data requested
constexpr qint64 kChunkSize = 256 * 1024;
constexpr qint64 kSocketHighWater = 2 * 1024 * 1024;
constexpr qint64 kProcessStopAbove = 32 * 1024 * 1024;
constexpr qint64 kProcessResumeBelow = 8 * 1024 * 1024;

const QByteArray kContentDirectoryType = "urn:schemas-upnp-org:service:ContentDirectory:1";
const QByteArray kConnectionManagerType = "urn:schemas-upnp-org:service:ConnectionManager:1";
const QByteArray kXmlContentType = "text/xml; charset=\"utf-8\"";

constexpr auto kContentDirectoryScpd = R"(<?xml version="1.0" encoding="utf-8"?>
<scpd xmlns="urn:schemas-upnp-org:service-1-0">
<specVersion><major>1</major><minor>0</minor></specVersion>
<actionList>
<action><name>Browse</name><argumentList>
<argument><name>ObjectID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_ObjectID</relatedStateVariable></argument>
<argument><name>BrowseFlag</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_BrowseFlag</relatedStateVariable></argument>
<argument><name>Filter</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_Filter</relatedStateVariable></argument>
<argument><name>StartingIndex</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_Index</relatedStateVariable></argument>
<argument><name>RequestedCount</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_Count</relatedStateVariable></argument>
<argument><name>SortCriteria</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_SortCriteria</relatedStateVariable></argument>
<argument><name>Result</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_Result</relatedStateVariable></argument>
<argument><name>NumberReturned</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_Count</relatedStateVariable></argument>
<argument><name>TotalMatches</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_Count</relatedStateVariable></argument>
<argument><name>UpdateID</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_UpdateID</relatedStateVariable></argument>
</argumentList></action>
<action><name>GetSearchCapabilities</name><argumentList>
<argument><name>SearchCaps</name><direction>out</direction><relatedStateVariable>SearchCapabilities</relatedStateVariable></argument>
</argumentList></action>
<action><name>GetSortCapabilities</name><argumentList>
<argument><name>SortCaps</name><direction>out</direction><relatedStateVariable>SortCapabilities</relatedStateVariable></argument>
</argumentList></action>
<action><name>GetSystemUpdateID</name><argumentList>
<argument><name>Id</name><direction>out</direction><relatedStateVariable>SystemUpdateID</relatedStateVariable></argument>
</argumentList></action>
</actionList>
<serviceStateTable>
<stateVariable sendEvents="no"><name>A_ARG_TYPE_ObjectID</name><dataType>string</dataType></stateVariable>
<stateVariable sendEvents="no"><name>A_ARG_TYPE_Result</name><dataType>string</dataType></stateVariable>
<stateVariable sendEvents="no"><name>A_ARG_TYPE_BrowseFlag</name><dataType>string</dataType><allowedValueList><allowedValue>BrowseMetadata</allowedValue><allowedValue>BrowseDirectChildren</allowedValue></allowedValueList></stateVariable>
<stateVariable sendEvents="no"><name>A_ARG_TYPE_Filter</name><dataType>string</dataType></stateVariable>
<stateVariable sendEvents="no"><name>A_ARG_TYPE_SortCriteria</name><dataType>string</dataType></stateVariable>
<stateVariable sendEvents="no"><name>A_ARG_TYPE_Index</name><dataType>ui4</dataType></stateVariable>
<stateVariable sendEvents="no"><name>A_ARG_TYPE_Count</name><dataType>ui4</dataType></stateVariable>
<stateVariable sendEvents="no"><name>A_ARG_TYPE_UpdateID</name><dataType>ui4</dataType></stateVariable>
<stateVariable sendEvents="no"><name>SearchCapabilities</name><dataType>string</dataType></stateVariable>
<stateVariable sendEvents="no"><name>SortCapabilities</name><dataType>string</dataType></stateVariable>
<stateVariable sendEvents="yes"><name>SystemUpdateID</name><dataType>ui4</dataType></stateVariable>
</serviceStateTable>
</scpd>
)";

constexpr auto kConnectionManagerScpd = R"(<?xml version="1.0" encoding="utf-8"?>
<scpd xmlns="urn:schemas-upnp-org:service-1-0">
<specVersion><major>1</major><minor>0</minor></specVersion>
<actionList>
<action><name>GetProtocolInfo</name><argumentList>
<argument><name>Source</name><direction>out</direction><relatedStateVariable>SourceProtocolInfo</relatedStateVariable></argument>
<argument><name>Sink</name><direction>out</direction><relatedStateVariable>SinkProtocolInfo</relatedStateVariable></argument>
</argumentList></action>
<action><name>GetCurrentConnectionIDs</name><argumentList>
<argument><name>ConnectionIDs</name><direction>out</direction><relatedStateVariable>CurrentConnectionIDs</relatedStateVariable></argument>
</argumentList></action>
<action><name>GetCurrentConnectionInfo</name><argumentList>
<argument><name>ConnectionID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_ConnectionID</relatedStateVariable></argument>
<argument><name>RcsID</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_RcsID</relatedStateVariable></argument>
<argument><name>AVTransportID</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_AVTransportID</relatedStateVariable></argument>
<argument><name>ProtocolInfo</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_ProtocolInfo</relatedStateVariable></argument>
<argument><name>PeerConnectionManager</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_ConnectionManager</relatedStateVariable></argument>
<argument><name>PeerConnectionID</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_ConnectionID</relatedStateVariable></argument>
<argument><name>Direction</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_Direction</relatedStateVariable></argument>
<argument><name>Status</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_ConnectionStatus</relatedStateVariable></argument>
</argumentList></action>
</actionList>
<serviceStateTable>
<stateVariable sendEvents="yes"><name>SourceProtocolInfo</name><dataType>string</dataType></stateVariable>
<stateVariable sendEvents="yes"><name>SinkProtocolInfo</name><dataType>string</dataType></stateVariable>
<stateVariable sendEvents="yes"><name>CurrentConnectionIDs</name><dataType>string</dataType></stateVariable>
<stateVariable sendEvents="no"><name>A_ARG_TYPE_ConnectionStatus</name><dataType>string</dataType><allowedValueList><allowedValue>OK</allowedValue><allowedValue>ContentFormatMismatch</allowedValue><allowedValue>InsufficientBandwidth</allowedValue><allowedValue>UnreliableChannel</allowedValue><allowedValue>Unknown</allowedValue></allowedValueList></stateVariable>
<stateVariable sendEvents="no"><name>A_ARG_TYPE_ConnectionManager</name><dataType>string</dataType></stateVariable>
<stateVariable sendEvents="no"><name>A_ARG_TYPE_Direction</name><dataType>string</dataType><allowedValueList><allowedValue>Input</allowedValue><allowedValue>Output</allowedValue></allowedValueList></stateVariable>
<stateVariable sendEvents="no"><name>A_ARG_TYPE_ProtocolInfo</name><dataType>string</dataType></stateVariable>
<stateVariable sendEvents="no"><name>A_ARG_TYPE_ConnectionID</name><dataType>i4</dataType></stateVariable>
<stateVariable sendEvents="no"><name>A_ARG_TYPE_AVTransportID</name><dataType>i4</dataType></stateVariable>
<stateVariable sendEvents="no"><name>A_ARG_TYPE_RcsID</name><dataType>i4</dataType></stateVariable>
</serviceStateTable>
</scpd>
)";

using Arguments = QList<std::pair<QString, QString>>;

QString sourceProtocolInfo() {
  QStringList infos;
  for (const auto& type : {"video/x-matroska", "video/mp4", "video/x-msvideo", "video/webm",
                           "video/mp2t", "video/mpeg", "video/quicktime", "video/x-ms-wmv",
                           "video/x-flv", "video/3gpp", "video/ogg"}) {
    infos.append(QString::fromLatin1(protocolInfo(type, false)));
  }
  return infos.join(u',');
}

QMap<QString, QString> parseSoapArguments(const QByteArray& body, QString& action) {
  QXmlStreamReader xml{body};
  QMap<QString, QString> arguments;
  bool inBody = false;
  bool inAction = false;

  while (!xml.atEnd()) {
    xml.readNext();
    if (xml.isStartElement()) {
      if (!inBody) {
        inBody = xml.name() == u"Body";
      } else if (!inAction) {
        // The action element, in case the SOAPACTION header didn't name it
        if (action.isEmpty()) action = xml.name().toString();
        inAction = true;
      } else {
        const auto name = xml.name().toString();
        arguments.insert(name, xml.readElementText(QXmlStreamReader::IncludeChildElements));
      }
    } else if (xml.isEndElement() && inAction && xml.name() == action) {
      break;
    }
  }

  return arguments;
}

QByteArray soapResponse(const QByteArray& serviceType, const QString& action,
                        const Arguments& values) {
  QByteArray data;
  QXmlStreamWriter xml{&data};
  xml.writeStartDocument();
  xml.writeStartElement(u"s:Envelope"_s);
  xml.writeAttribute(u"xmlns:s"_s, u"http://schemas.xmlsoap.org/soap/envelope/"_s);
  xml.writeAttribute(u"s:encodingStyle"_s, u"http://schemas.xmlsoap.org/soap/encoding/"_s);
  xml.writeStartElement(u"s:Body"_s);
  xml.writeStartElement(u"u:%1Response"_s.arg(action));
  xml.writeAttribute(u"xmlns:u"_s, QString::fromLatin1(serviceType));
  for (const auto& [name, value] : values) {
    xml.writeTextElement(name, value);  // escaped, which is how DIDL-Lite is embedded
  }
  xml.writeEndDocument();
  return data;
}

QByteArray soapFault(int code, const QString& description) {
  QByteArray data;
  QXmlStreamWriter xml{&data};
  xml.writeStartDocument();
  xml.writeStartElement(u"s:Envelope"_s);
  xml.writeAttribute(u"xmlns:s"_s, u"http://schemas.xmlsoap.org/soap/envelope/"_s);
  xml.writeAttribute(u"s:encodingStyle"_s, u"http://schemas.xmlsoap.org/soap/encoding/"_s);
  xml.writeStartElement(u"s:Body"_s);
  xml.writeStartElement(u"s:Fault"_s);
  xml.writeTextElement(u"faultcode"_s, u"s:Client"_s);
  xml.writeTextElement(u"faultstring"_s, u"UPnPError"_s);
  xml.writeStartElement(u"detail"_s);
  xml.writeStartElement(u"UPnPError"_s);
  xml.writeAttribute(u"xmlns"_s, u"urn:schemas-upnp-org:control-1-0"_s);
  xml.writeTextElement(u"errorCode"_s, QString::number(code));
  xml.writeTextElement(u"errorDescription"_s, description);
  xml.writeEndDocument();
  return data;
}

// First byte range of a "Range: bytes=..." header, as [first, last]
std::optional<std::pair<qint64, qint64>> parseRange(const QByteArray& header, qint64 size) {
  if (!header.trimmed().startsWith("bytes=") || size <= 0) return std::nullopt;

  const auto spec = header.trimmed().mid(6).split(',').value(0).trimmed();
  const auto dash = spec.indexOf('-');
  if (dash < 0) return std::nullopt;

  const auto first = spec.left(dash).trimmed();
  const auto last = spec.mid(dash + 1).trimmed();
  bool ok = false;
  qint64 start = 0;
  qint64 end = size - 1;

  if (first.isEmpty()) {
    const auto suffix = last.toLongLong(&ok);
    if (!ok || suffix <= 0) return std::nullopt;
    start = std::max<qint64>(0, size - suffix);
  } else {
    start = first.toLongLong(&ok);
    if (!ok) return std::nullopt;
    if (!last.isEmpty()) {
      end = last.toLongLong(&ok);
      if (!ok) return std::nullopt;
      end = std::min(end, size - 1);
    }
  }

  if (start > end || start >= size) return std::nullopt;
  return std::pair{start, end};
}

// Start of a "TimeSeekRange.dlna.org: npt=..." header, in seconds
std::optional<double> parseNpt(const QByteArray& header) {
  const auto value = header.trimmed();
  if (!value.startsWith("npt=")) return std::nullopt;

  const auto start = value.mid(4).split('-').value(0).trimmed();
  if (start.isEmpty()) return std::nullopt;

  double seconds = 0;
  for (const auto& part : start.split(':')) {
    bool ok = false;
    const auto number = part.toDouble(&ok);
    if (!ok) return std::nullopt;
    seconds = seconds * 60 + number;
  }
  return seconds;
}

bool isPrivateAddress(const QHostAddress& address) {
  bool isIPv4 = false;
  const auto ipv4 = QHostAddress{address.toIPv4Address(&isIPv4)};
  if (isIPv4) {
    static const QList<std::pair<QHostAddress, int>> subnets{
        QHostAddress::parseSubnet(u"127.0.0.0/8"_s),    QHostAddress::parseSubnet(u"10.0.0.0/8"_s),
        QHostAddress::parseSubnet(u"172.16.0.0/12"_s),  QHostAddress::parseSubnet(u"192.168.0.0/16"_s),
        QHostAddress::parseSubnet(u"169.254.0.0/16"_s),
        QHostAddress::parseSubnet(u"100.64.0.0/10"_s),  // carrier-grade NAT, used by Tailscale
    };
    return std::ranges::any_of(subnets, [&ipv4](const auto& subnet) {
      return ipv4.isInSubnet(subnet.first, subnet.second);
    });
  }
  return address.isLoopback() || address.isLinkLocal() || address.isUniqueLocalUnicast();
}

QString streamKey(const QHostAddress& client, const QString& path) {
  return client.toString() + u'|' + path;
}

QString ffmpegPath() {
  static const auto path = QStandardPaths::findExecutable(u"ffmpeg"_s);
  return path;
}

QStringList transcodeArguments(const QString& input, const QString& subtitleInput, double start,
                               bool hardware, const QString& device, int bitrate) {
  const auto kbps = [](int value) { return u"%1k"_s.arg(value); };

  QStringList args{u"-hide_banner"_s, u"-loglevel"_s, u"error"_s, u"-nostdin"_s};
  if (hardware) args << u"-vaapi_device"_s << device;
  if (start > 0) args << u"-ss"_s << QString::number(start, 'f', 3);
  // Keep original timestamps, so that burned-in subtitles stay in sync after seeking
  if (!subtitleInput.isEmpty()) args << u"-copyts"_s;
  args << u"-i"_s << input;
  args << u"-map"_s << u"0:v:0"_s << u"-map"_s << u"0:a:0?"_s;

  QStringList filters;
  if (!subtitleInput.isEmpty()) filters << u"subtitles=filename=%1:si=0"_s.arg(subtitleInput);

  if (hardware) {
    // Decoding and subtitles on the CPU, encoding on the GPU
    filters << u"format=nv12"_s << u"hwupload"_s;
    args << u"-vf"_s << filters.join(u',');
    args << u"-c:v"_s << u"h264_vaapi"_s << u"-b:v"_s << kbps(bitrate) << u"-maxrate"_s
         << kbps(bitrate * 3 / 2) << u"-bufsize"_s << kbps(bitrate * 2);
  } else {
    filters << u"format=yuv420p"_s;
    args << u"-vf"_s << filters.join(u',');
    args << u"-c:v"_s << u"libx264"_s << u"-preset"_s << u"veryfast"_s << u"-tune"_s
         << u"animation"_s << u"-crf"_s << u"20"_s << u"-maxrate"_s << kbps(bitrate)
         << u"-bufsize"_s << kbps(bitrate * 2);
  }

  args << u"-g"_s << u"120"_s;
  args << u"-c:a"_s << u"aac"_s << u"-b:a"_s << u"192k"_s << u"-ac"_s << u"2"_s;
  args << u"-f"_s << u"mpegts"_s << u"-muxdelay"_s << u"0"_s << u"pipe:1"_s;
  return args;
}

////////////////////////////////////////////////////////////////////////////////

// Sends (a range of) a file, reading more as the client consumes it
class FileResponse final : public QObject {
public:
  FileResponse(HttpConnection* connection, Server* server, QString key, const QString& path,
               qint64 start, qint64 length)
      : QObject(connection),
        connection_(connection),
        server_(server),
        key_(std::move(key)),
        file_(path),
        start_(start),
        remaining_(length) {}

  bool start() {
    if (!file_.open(QIODevice::ReadOnly) || !file_.seek(start_)) return false;
    connect(connection_, &HttpConnection::bytesWritten, this, &FileResponse::pump);
    connect(connection_, &HttpConnection::closed, this, &FileResponse::finish);
    pump();
    return true;
  }

private:
  void pump() {
    if (finished_) return;

    while (connection_ && connection_->isOpen() && remaining_ > 0 &&
           connection_->bytesToWrite() < kSocketHighWater) {
      const auto data = file_.read(std::min(kChunkSize, remaining_));
      if (data.isEmpty()) {
        connection_->close();
        finish();
        return;
      }
      connection_->write(data);
      remaining_ -= data.size();
      server_->streamProgress(key_, data.size());
    }

    if (remaining_ == 0) {
      const auto connection = connection_;
      finish();
      if (connection) connection->endResponse();
    }
  }

  void finish() {
    if (finished_) return;
    finished_ = true;
    if (connection_) disconnect(connection_, nullptr, this, nullptr);
    file_.close();
    server_->streamFinished(key_);
    deleteLater();
  }

  QPointer<HttpConnection> connection_;
  Server* server_ = nullptr;
  QString key_;
  QFile file_;
  qint64 start_ = 0;
  qint64 remaining_ = 0;
  bool finished_ = false;
};

// Streams the output of FFmpeg. FFmpeg is suspended while too much of its output is waiting to be
// sent, so that a paused client doesn't make us buffer the whole video in memory.
class TranscodeResponse final : public QObject {
public:
  TranscodeResponse(HttpConnection* connection, Server* server, QString key,
                    QStringList arguments, QString temporaryFile)
      : QObject(connection),
        connection_(connection),
        server_(server),
        key_(std::move(key)),
        arguments_(std::move(arguments)),
        temporaryFile_(std::move(temporaryFile)),
        process_(new QProcess(this)) {}

  bool start() {
    process_->setReadChannel(QProcess::StandardOutput);

    connect(process_, &QProcess::readyReadStandardOutput, this, &TranscodeResponse::pump);
    connect(process_, &QProcess::readyReadStandardError, this, [this]() {
      errors_ += process_->readAllStandardError();
      if (errors_.size() > 4096) errors_ = errors_.right(4096);
    });
    connect(process_, &QProcess::finished, this, [this](int exitCode) {
      if (exitCode != 0 && !finished_) {
        qWarning() << "Transcoding failed:" << QString::fromUtf8(errors_).trimmed();
      }
      processFinished_ = true;
      pump();
    });
    connect(connection_, &HttpConnection::bytesWritten, this, &TranscodeResponse::pump);
    connect(connection_, &HttpConnection::closed, this, &TranscodeResponse::finish);

    process_->start(ffmpegPath(), arguments_);
    return process_->waitForStarted(5000);
  }

private:
  void pump() {
    if (finished_) return;

    while (connection_ && connection_->isOpen() &&
           connection_->bytesToWrite() < kSocketHighWater) {
      const auto data = process_->read(kChunkSize);
      if (data.isEmpty()) break;
      connection_->write(data);
      server_->streamProgress(key_, data.size());
    }

    const auto pending = process_->bytesAvailable();
    if (pending > kProcessStopAbove) {
      setSuspended(true);
    } else if (pending < kProcessResumeBelow) {
      setSuspended(false);
    }

    if (processFinished_ && process_->bytesAvailable() == 0) {
      const auto connection = connection_;
      finish();
      if (connection) connection->endResponse();  // closes the connection, which ends the stream
    }
  }

  void setSuspended(bool suspended) {
#ifdef Q_OS_UNIX
    if (suspended == suspended_ || process_->state() != QProcess::Running) return;
    ::kill(static_cast<pid_t>(process_->processId()), suspended ? SIGSTOP : SIGCONT);
    suspended_ = suspended;
#else
    Q_UNUSED(suspended);
#endif
  }

  void finish() {
    if (finished_) return;
    finished_ = true;
    if (connection_) disconnect(connection_, nullptr, this, nullptr);
    if (process_->state() != QProcess::NotRunning) {
      setSuspended(false);
      process_->kill();
    }
    if (!temporaryFile_.isEmpty()) QFile::remove(temporaryFile_);
    server_->streamFinished(key_);
    deleteLater();
  }

  QPointer<HttpConnection> connection_;
  Server* server_ = nullptr;
  QString key_;
  QStringList arguments_;
  QString temporaryFile_;
  QProcess* process_ = nullptr;
  QByteArray errors_;
  bool processFinished_ = false;
  bool suspended_ = false;
  bool finished_ = false;
};

}  // namespace

////////////////////////////////////////////////////////////////////////////////

Server::Server(QObject* parent)
    : QObject(parent),
      http_(new HttpServer(this)),
      ssdp_(new Ssdp(this)),
      mediaInfo_(new MediaInfoCache(this)),
      network_(new QNetworkAccessManager(this)),
      cleanupTimer_(new QTimer(this)) {
  network_->setProxy(QNetworkProxy::NoProxy);
  network_->setTransferTimeout(std::chrono::seconds{5});
  network_->setAutoDeleteReplies(true);

  http_->setHandler(
      [this](HttpConnection* connection, const HttpRequest& request) { handle(connection, request); });
  http_->setClientFilter([this](const QHostAddress& address) { return isAllowedClient(address); });

  connect(cleanupTimer_, &QTimer::timeout, this, &Server::cleanUpStreams);
}

Server::~Server() {
  stop();
}

void Server::init() {
  if (taiga::settings.mediaServerEnabled()) restart();
}

void Server::restart() {
  stop();

  if (!taiga::settings.mediaServerEnabled()) {
    emit statusChanged();
    return;
  }

  uuid_ = QString::fromStdString(taiga::settings.mediaServerUuid());
  library_.setVirtualFoldersEnabled(taiga::settings.mediaServerVirtualFolders());
  library_.invalidate();
  ++systemUpdateId_;

  const auto port = static_cast<quint16>(taiga::settings.mediaServerPort());
  if (!http_->listen(port)) {
    error_ = tr("Port %1 is not available (%2)").arg(port).arg(http_->errorString());
    qWarning() << "Media server:" << error_;
    emit statusChanged();
    return;
  }

  if (!ssdp_->start(uuid_, http_->port())) {
    warning_ = tr("Clients can't discover the server automatically (%1)").arg(ssdp_->errorString());
  }

  cleanupTimer_->start(std::chrono::seconds{30});
  qDebug() << "Media server: listening on port" << http_->port();
  emit statusChanged();
}

void Server::stop() {
  ssdp_->stop();
  http_->close();
  cleanupTimer_->stop();
  error_.clear();
  warning_.clear();
}

bool Server::isRunning() const {
  return http_->isListening();
}

QString Server::statusText() const {
  if (!taiga::settings.mediaServerEnabled()) return tr("Disabled");
  if (!error_.isEmpty()) return error_;
  if (!isRunning()) return tr("Not running");

  auto text = tr("Running on port %1 as \"%2\"")
                  .arg(http_->port())
                  .arg(QString::fromStdString(taiga::settings.mediaServerName()));
  if (!warning_.isEmpty()) text += u" — "_s + warning_;
  return text;
}

std::optional<Stream> Server::currentStream() const {
  const auto now = clock::now();
  const Stream* current = nullptr;

  for (const auto& stream : streams_) {
    const bool active = stream.connections > 0 || now - stream.lastActivity < kStreamGracePeriod;
    if (!active) continue;
    if (stream.lastActivity - stream.started < kMinimumStreamTime) continue;
    if (!current || stream.lastActivity > current->lastActivity) current = &stream;
  }

  if (!current) return std::nullopt;

  auto stream = *current;
  stream.paused = stream.connections > 0 && now - stream.lastActivity > kPausedAfter;
  return stream;
}

void Server::streamStarted(const QString& key, const QString& path, const QHostAddress& client,
                           bool transcoded) {
  const auto now = clock::now();
  auto& stream = streams_[key];

  // A new viewing session, unless the client is just reconnecting (e.g. after seeking)
  if (stream.path.isEmpty() ||
      (stream.connections == 0 && now - stream.lastActivity > kStreamGracePeriod)) {
    stream = Stream{
        .path = path,
        .client = client,
        .started = now,
        .lastActivity = now,
        .connections = 0,
        .bytesSent = 0,
        .transcoded = transcoded,
        .paused = false,
    };
  }

  ++stream.connections;
}

void Server::streamProgress(const QString& key, qint64 bytes) {
  const auto it = streams_.find(key);
  if (it == streams_.end()) return;
  it->lastActivity = clock::now();
  it->bytesSent += bytes;
}

void Server::streamFinished(const QString& key) {
  // `lastActivity` stays as is: the grace period starts when data was last sent, and connections
  // that were opened without reading much (e.g. by a client probing the file) don't count.
  const auto it = streams_.find(key);
  if (it == streams_.end()) return;
  it->connections = std::max(0, it->connections - 1);
}

void Server::cleanUpStreams() {
  const auto now = clock::now();
  streams_.removeIf([&now](const QHash<QString, Stream>::iterator it) {
    return it->connections == 0 && now - it->lastActivity > kStreamGracePeriod * 2;
  });
}

bool Server::isAllowedClient(const QHostAddress& address) const {
  return !taiga::settings.mediaServerLocalOnly() || isPrivateAddress(address);
}

QString Server::baseUrl(const HttpRequest& request) const {
  bool isIPv4 = false;
  const auto ipv4 = request.localAddress.toIPv4Address(&isIPv4);
  const auto host = isIPv4 ? QHostAddress{ipv4}.toString() : request.localAddress.toString();
  return u"http://%1:%2"_s.arg(host).arg(request.localPort);
}

////////////////////////////////////////////////////////////////////////////////

void Server::handle(HttpConnection* connection, const HttpRequest& request) {
  const auto path = request.path();
  const bool isGet = request.method == "GET" || request.method == "HEAD";
  const bool headOnly = request.method == "HEAD";

  if (isGet && (path == u"/description.xml" || path == u"/")) {
    return handleDescription(connection, request);
  }
  if (isGet && path == u"/ContentDirectory.xml") {
    return connection->respond(200, kXmlContentType, kContentDirectoryScpd, {}, headOnly);
  }
  if (isGet && path == u"/ConnectionManager.xml") {
    return connection->respond(200, kXmlContentType, kConnectionManagerScpd, {}, headOnly);
  }
  if (isGet && path == u"/icon.png") return handleIcon(connection, request);
  if (isGet && path.startsWith(u"/media/"_s)) return handleMedia(connection, request);
  if (isGet && path.startsWith(u"/transcode/"_s)) return handleTranscode(connection, request);
  if (request.method == "POST" && path.startsWith(u"/control/"_s)) {
    return handleControl(connection, request);
  }
  if ((request.method == "SUBSCRIBE" || request.method == "UNSUBSCRIBE") &&
      path.startsWith(u"/event/"_s)) {
    return handleEvent(connection, request);
  }

  if (isGet) return connection->respond(404, "text/plain", "Not Found", {}, headOnly);
  connection->respond(405, "text/plain", "Method Not Allowed");
}

void Server::handleDescription(HttpConnection* connection, const HttpRequest& request) {
  const auto name = QString::fromStdString(taiga::settings.mediaServerName()).toHtmlEscaped();
  const auto version = QString::fromStdString(taiga::version().to_string());

  const auto xml = uR"(<?xml version="1.0" encoding="utf-8"?>
<root xmlns="urn:schemas-upnp-org:device-1-0" xmlns:dlna="urn:schemas-dlna-org:device-1-0">
<specVersion><major>1</major><minor>0</minor></specVersion>
<device>
<deviceType>urn:schemas-upnp-org:device:MediaServer:1</deviceType>
<friendlyName>%1</friendlyName>
<manufacturer>Taiga</manufacturer>
<manufacturerURL>https://taiga.moe</manufacturerURL>
<modelDescription>Anime library shared by Taiga</modelDescription>
<modelName>Taiga</modelName>
<modelNumber>%2</modelNumber>
<modelURL>https://taiga.moe</modelURL>
<serialNumber>%3</serialNumber>
<UDN>uuid:%3</UDN>
<dlna:X_DLNADOC>DMS-1.50</dlna:X_DLNADOC>
<iconList>
<icon><mimetype>image/png</mimetype><width>120</width><height>120</height><depth>32</depth><url>/icon.png</url></icon>
</iconList>
<serviceList>
<service>
<serviceType>urn:schemas-upnp-org:service:ContentDirectory:1</serviceType>
<serviceId>urn:upnp-org:serviceId:ContentDirectory</serviceId>
<SCPDURL>/ContentDirectory.xml</SCPDURL>
<controlURL>/control/ContentDirectory</controlURL>
<eventSubURL>/event/ContentDirectory</eventSubURL>
</service>
<service>
<serviceType>urn:schemas-upnp-org:service:ConnectionManager:1</serviceType>
<serviceId>urn:upnp-org:serviceId:ConnectionManager</serviceId>
<SCPDURL>/ConnectionManager.xml</SCPDURL>
<controlURL>/control/ConnectionManager</controlURL>
<eventSubURL>/event/ConnectionManager</eventSubURL>
</service>
</serviceList>
</device>
</root>
)"_s.arg(name, version, uuid_);

  connection->respond(200, kXmlContentType, xml.toUtf8(), {}, request.method == "HEAD");
}

void Server::handleIcon(HttpConnection* connection, const HttpRequest& request) {
  static const QByteArray icon = []() {
    QByteArray data;
    QBuffer buffer{&data};
    buffer.open(QIODevice::WriteOnly);
    QImage{u":/icons/taiga.png"_s}
        .scaled(120, 120, Qt::KeepAspectRatio, Qt::SmoothTransformation)
        .save(&buffer, "PNG");
    return data;
  }();

  connection->respond(200, "image/png", icon, {}, request.method == "HEAD");
}

void Server::handleControl(HttpConnection* connection, const HttpRequest& request) {
  auto soapAction = request.header("soapaction");
  soapAction.replace('"', "");
  const auto hash = soapAction.indexOf('#');

  QString action = hash >= 0 ? QString::fromLatin1(soapAction.mid(hash + 1)) : QString{};
  const auto arguments = parseSoapArguments(request.body, action);

  const bool isContentDirectory = request.path().endsWith(u"ContentDirectory"_s);
  const auto serviceType = isContentDirectory ? kContentDirectoryType : kConnectionManagerType;

  Arguments output;
  int error = 0;

  if (isContentDirectory) {
    if (action == u"Browse") {
      const auto response = browse(arguments, request, error);
      if (!error) {
        connection->respond(200, kXmlContentType, response);
        return;
      }
    } else if (action == u"GetSearchCapabilities") {
      output = {{u"SearchCaps"_s, {}}};
    } else if (action == u"GetSortCapabilities") {
      output = {{u"SortCaps"_s, {}}};
    } else if (action == u"GetSystemUpdateID") {
      output = {{u"Id"_s, QString::number(systemUpdateId_)}};
    } else {
      error = 401;
    }
  } else {
    if (action == u"GetProtocolInfo") {
      output = {{u"Source"_s, sourceProtocolInfo()}, {u"Sink"_s, {}}};
    } else if (action == u"GetCurrentConnectionIDs") {
      output = {{u"ConnectionIDs"_s, u"0"_s}};
    } else if (action == u"GetCurrentConnectionInfo") {
      output = {
          {u"RcsID"_s, u"-1"_s},
          {u"AVTransportID"_s, u"-1"_s},
          {u"ProtocolInfo"_s, {}},
          {u"PeerConnectionManager"_s, {}},
          {u"PeerConnectionID"_s, u"-1"_s},
          {u"Direction"_s, u"Output"_s},
          {u"Status"_s, u"OK"_s},
      };
    } else {
      error = 401;
    }
  }

  if (error) {
    const auto description = error == 401   ? u"Invalid Action"_s
                             : error == 701 ? u"No such object"_s
                                            : u"Invalid Args"_s;
    connection->respond(500, kXmlContentType, soapFault(error, description));
    return;
  }

  connection->respond(200, kXmlContentType, soapResponse(serviceType, action, output));
}

QByteArray Server::browse(const QMap<QString, QString>& arguments, const HttpRequest& request,
                          int& errorCode) {
  const auto objectId = arguments.value(u"ObjectID"_s, u"0"_s);
  const auto flag = arguments.value(u"BrowseFlag"_s);
  const auto start = static_cast<size_t>(std::max(0, arguments.value(u"StartingIndex"_s).toInt()));
  const auto count = static_cast<size_t>(std::max(0, arguments.value(u"RequestedCount"_s).toInt()));

  std::vector<Object> objects;
  size_t totalMatches = 0;

  if (flag == u"BrowseMetadata") {
    auto object = library_.object(objectId);
    if (!object) {
      errorCode = 701;
      return {};
    }
    if (object->id == u"0") object->title = QString::fromStdString(taiga::settings.mediaServerName());
    objects.push_back(std::move(*object));
    totalMatches = 1;
  } else if (flag == u"BrowseDirectChildren") {
    const auto children = library_.children(objectId);
    if (!children) {
      errorCode = 701;
      return {};
    }
    totalMatches = children->size();
    const auto first = std::min(start, children->size());
    const auto last = count == 0 ? children->size() : std::min(first + count, children->size());
    objects.assign(children->begin() + static_cast<ptrdiff_t>(first),
                   children->begin() + static_cast<ptrdiff_t>(last));
  } else {
    errorCode = 402;
    return {};
  }

  return soapResponse(kContentDirectoryType, u"Browse"_s,
                      {
                          {u"Result"_s, didl(objects, request)},
                          {u"NumberReturned"_s, QString::number(objects.size())},
                          {u"TotalMatches"_s, QString::number(totalMatches)},
                          {u"UpdateID"_s, QString::number(systemUpdateId_)},
                      });
}

QString Server::didl(const std::vector<Object>& objects, const HttpRequest& request) {
  const auto base = baseUrl(request);
  const auto transcoding = taiga::settings.mediaServerTranscoding();

  QString data;
  QXmlStreamWriter xml{&data};
  xml.writeStartElement(u"DIDL-Lite"_s);
  xml.writeAttribute(u"xmlns"_s, u"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/"_s);
  xml.writeAttribute(u"xmlns:dc"_s, u"http://purl.org/dc/elements/1.1/"_s);
  xml.writeAttribute(u"xmlns:upnp"_s, u"urn:schemas-upnp-org:metadata-1-0/upnp/"_s);
  xml.writeAttribute(u"xmlns:dlna"_s, u"urn:schemas-dlna-org:metadata-1-0/"_s);

  QStringList pathsToProbe;

  for (const auto& object : objects) {
    if (object.isContainer) {
      xml.writeStartElement(u"container"_s);
      xml.writeAttribute(u"id"_s, object.id);
      xml.writeAttribute(u"parentID"_s, object.parentId);
      xml.writeAttribute(u"restricted"_s, u"1"_s);
      xml.writeAttribute(u"searchable"_s, u"0"_s);
      xml.writeAttribute(u"childCount"_s, QString::number(object.childCount));
      xml.writeTextElement(u"dc:title"_s, object.title);
      xml.writeTextElement(u"upnp:class"_s, u"object.container.storageFolder"_s);
      if (!object.albumArtUrl.isEmpty()) {
        xml.writeTextElement(u"upnp:albumArtURI"_s, object.albumArtUrl);
      }
      xml.writeEndElement();
      continue;
    }

    const QFileInfo info{object.path};
    const auto token = Library::encodePath(object.path);
    const auto mediaInfo = mediaInfo_->find(object.path);
    if (!mediaInfo) pathsToProbe.append(object.path);

    xml.writeStartElement(u"item"_s);
    xml.writeAttribute(u"id"_s, object.id);
    xml.writeAttribute(u"parentID"_s, object.parentId);
    xml.writeAttribute(u"restricted"_s, u"1"_s);
    xml.writeTextElement(u"dc:title"_s, object.title);
    xml.writeTextElement(u"upnp:class"_s, u"object.item.videoItem"_s);
    if (!object.albumArtUrl.isEmpty()) {
      xml.writeTextElement(u"upnp:albumArtURI"_s, object.albumArtUrl);
    }

    const auto writeResource = [&](const QByteArray& protocol, const QString& url, bool withSize) {
      xml.writeStartElement(u"res"_s);
      xml.writeAttribute(u"protocolInfo"_s, QString::fromLatin1(protocol));
      if (withSize) xml.writeAttribute(u"size"_s, QString::number(info.size()));
      if (mediaInfo && mediaInfo->duration.count() > 0) {
        xml.writeAttribute(u"duration"_s, formatDuration(mediaInfo->duration));
      }
      if (mediaInfo && mediaInfo->width > 0) {
        xml.writeAttribute(u"resolution"_s, u"%1x%2"_s.arg(mediaInfo->width).arg(mediaInfo->height));
      }
      xml.writeCharacters(url);
      xml.writeEndElement();
    };

    // Clients pick the first resource they can play, so a transcoded stream is offered first only
    // if transcoding is enabled. The original file is always available.
    if (transcoding != "none") {
      const auto name = QString::fromUtf8(QUrl::toPercentEncoding(info.completeBaseName() + u".ts"_s));
      writeResource(protocolInfo("video/mp2t", true), u"%1/transcode/%2/%3"_s.arg(base, token, name),
                    false);
    }
    const auto name = QString::fromUtf8(QUrl::toPercentEncoding(info.fileName()));
    writeResource(protocolInfo(mimeType(object.path), false),
                  u"%1/media/%2/%3"_s.arg(base, token, name), true);

    xml.writeEndElement();
  }

  xml.writeEndElement();

  // Durations will be included the next time the client browses here
  if (!pathsToProbe.isEmpty()) mediaInfo_->request(pathsToProbe);

  return data;
}

void Server::handleEvent(HttpConnection* connection, const HttpRequest& request) {
  if (request.method == "UNSUBSCRIBE") {
    connection->respond(200, {}, {});
    return;
  }

  // Renewal of an existing subscription
  if (const auto sid = request.header("sid"); !sid.isEmpty()) {
    connection->respond(200, {}, {}, {{"SID", sid}, {"TIMEOUT", "Second-1800"}});
    return;
  }

  if (request.header("nt") != "upnp:event") {
    connection->respond(412, {}, {});
    return;
  }

  const auto sid = "uuid:" + QUuid::createUuid().toString(QUuid::WithoutBraces).toLatin1();
  connection->respond(200, {}, {}, {{"SID", sid}, {"TIMEOUT", "Second-1800"}});

  // Callback URLs are only accepted on the subscriber's own address.
  auto callback = request.header("callback");
  const auto begin = callback.indexOf('<');
  const auto end = callback.indexOf('>', begin);
  if (begin < 0 || end < 0) return;
  callback = callback.mid(begin + 1, end - begin - 1);
  const QUrl url{QString::fromLatin1(callback)};
  if (!url.isValid() || QHostAddress{url.host()} != QHostAddress{request.peerAddress.toIPv4Address()}) {
    return;
  }

  const bool contentDirectory = request.path().endsWith(u"ContentDirectory"_s);
  QTimer::singleShot(200ms, this, [this, callback, sid, contentDirectory]() {
    sendInitialEvent(callback, sid, contentDirectory);
  });
}

void Server::sendInitialEvent(const QByteArray& callback, const QByteArray& sid,
                              bool contentDirectory) {
  QByteArray properties;
  const auto property = [&properties](const QByteArray& name, const QByteArray& value) {
    properties += "<e:property><" + name + ">" + value + "</" + name + "></e:property>";
  };
  if (contentDirectory) {
    property("SystemUpdateID", QByteArray::number(systemUpdateId_));
  } else {
    property("SourceProtocolInfo", sourceProtocolInfo().toHtmlEscaped().toUtf8());
    property("SinkProtocolInfo", {});
    property("CurrentConnectionIDs", "0");
  }

  const auto body = R"(<?xml version="1.0" encoding="utf-8"?><e:propertyset xmlns:e="urn:schemas-upnp-org:event-1-0">)" +
                    properties + "</e:propertyset>";

  QNetworkRequest request{QUrl{QString::fromLatin1(callback)}};
  request.setHeader(QNetworkRequest::ContentTypeHeader, kXmlContentType);
  request.setRawHeader("NT", "upnp:event");
  request.setRawHeader("NTS", "upnp:propchange");
  request.setRawHeader("SID", sid);
  request.setRawHeader("SEQ", "0");
  network_->sendCustomRequest(request, "NOTIFY", body);
}

void Server::handleMedia(HttpConnection* connection, const HttpRequest& request) {
  const auto parts = request.path().split(u'/', Qt::SkipEmptyParts);  // media, token, name
  const auto path = parts.size() >= 2 ? library_.resolve(Library::decodePath(parts[1]))
                                      : std::optional<QString>{};
  if (!path || !isVideoFile(*path)) {
    connection->respond(404, "text/plain", "Not Found", {}, request.method == "HEAD");
    return;
  }

  const auto size = QFileInfo{*path}.size();

  HttpHeaders headers{
      {"Content-Type", mimeType(*path)},
      {"Accept-Ranges", "bytes"},
      {"transferMode.dlna.org", "Streaming"},
      {"contentFeatures.dlna.org", contentFeatures(false)},
  };

  int status = 200;
  qint64 start = 0;
  qint64 end = size - 1;

  if (const auto range = request.header("range"); !range.isEmpty()) {
    const auto parsed = parseRange(range, size);
    if (!parsed) {
      connection->respond(416, {}, {}, {{"Content-Range", "bytes */" + QByteArray::number(size)}});
      return;
    }
    std::tie(start, end) = *parsed;
    status = 206;
    headers.append({"Content-Range", "bytes " + QByteArray::number(start) + '-' +
                                         QByteArray::number(end) + '/' + QByteArray::number(size)});
  }

  const auto length = std::max<qint64>(0, end - start + 1);
  headers.append({"Content-Length", QByteArray::number(length)});

  connection->beginResponse(status, headers);

  if (request.method == "HEAD" || length == 0) {
    connection->endResponse();
    return;
  }

  const auto key = streamKey(request.peerAddress, *path);
  streamStarted(key, *path, request.peerAddress, false);

  auto response = new FileResponse(connection, this, key, *path, start, length);
  if (!response->start()) {
    streamFinished(key);
    delete response;
    connection->close();
  }
}

void Server::handleTranscode(HttpConnection* connection, const HttpRequest& request) {
  const auto mode = taiga::settings.mediaServerTranscoding();
  const auto parts = request.path().split(u'/', Qt::SkipEmptyParts);  // transcode, token, name
  const auto path = parts.size() >= 2 ? library_.resolve(Library::decodePath(parts[1]))
                                      : std::optional<QString>{};

  if (mode == "none" || !path || !isVideoFile(*path)) {
    connection->respond(404, "text/plain", "Not Found", {}, request.method == "HEAD");
    return;
  }
  if (ffmpegPath().isEmpty()) {
    connection->respond(503, "text/plain", "FFmpeg is not installed", {},
                        request.method == "HEAD");
    return;
  }

  const auto info = mediaInfo_->probeNow(*path);
  const double duration = info ? static_cast<double>(info->duration.count()) / 1000 : 0;
  const auto seek = parseNpt(request.header("timeseekrange.dlna.org"));
  const double start = seek ? std::clamp(*seek, 0.0, std::max(0.0, duration - 1)) : 0;

  HttpHeaders headers{
      {"Content-Type", "video/mp2t"},
      {"transferMode.dlna.org", "Streaming"},
      {"contentFeatures.dlna.org", contentFeatures(true)},
  };
  if (seek && duration > 0) {
    const auto total = QByteArray::number(duration, 'f', 3);
    headers.append({"TimeSeekRange.dlna.org",
                    "npt=" + QByteArray::number(start, 'f', 3) + '-' + total + '/' + total});
  }

  if (request.method == "HEAD") {
    connection->beginResponse(200, headers);
    connection->endResponse();
    return;
  }

  // FFmpeg's `subtitles` filter takes a file name with its own escaping rules; a link with a plain
  // name avoids having to escape anything.
  QString subtitleInput;
  if (taiga::settings.mediaServerBurnSubtitles() && info && info->hasTextSubtitles()) {
    const auto directory = QDir::temp().filePath(u"taiga-media-server"_s);
    QDir{}.mkpath(directory);
    subtitleInput = u"%1/%2.%3"_s.arg(directory)
                        .arg(QRandomGenerator::global()->generate64(), 0, 16)
                        .arg(QFileInfo{*path}.suffix().toLower());
    if (!QFile::link(*path, subtitleInput)) subtitleInput.clear();
  }

  const auto arguments = transcodeArguments(
      *path, subtitleInput, start, mode == "hardware",
      QString::fromStdString(taiga::settings.mediaServerVaapiDevice()),
      taiga::settings.mediaServerBitrate());

  // The length is unknown, so the end of the stream is signaled by closing the connection.
  connection->beginResponse(200, headers, true);

  const auto key = streamKey(request.peerAddress, *path);
  streamStarted(key, *path, request.peerAddress, true);

  auto response = new TranscodeResponse(connection, this, key, arguments, subtitleInput);
  if (!response->start()) {
    qWarning() << "Media server: could not start FFmpeg";
    connection->close();
  }
}

}  // namespace media_server
