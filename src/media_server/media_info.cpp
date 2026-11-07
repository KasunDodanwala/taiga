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

#include "media_info.hpp"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <cmath>

#include "base/string.hpp"

namespace media_server {

namespace {

constexpr int kMaxConcurrentProbes = 2;
constexpr qsizetype kMaxCacheSize = 20000;

// The same video container extensions that Anitomy recognizes, mapped to MIME types
const QHash<QString, QByteArray>& mimeTypes() {
  static const QHash<QString, QByteArray> types{
      {u"3gp"_s, "video/3gpp"},
      {u"avi"_s, "video/x-msvideo"},
      {u"divx"_s, "video/x-msvideo"},
      {u"flv"_s, "video/x-flv"},
      {u"m2ts"_s, "video/vnd.dlna.mpeg-tts"},
      {u"m4v"_s, "video/mp4"},
      {u"mkv"_s, "video/x-matroska"},
      {u"mov"_s, "video/quicktime"},
      {u"mp4"_s, "video/mp4"},
      {u"mpg"_s, "video/mpeg"},
      {u"ogm"_s, "video/ogg"},
      {u"rm"_s, "application/vnd.rn-realmedia"},
      {u"rmvb"_s, "application/vnd.rn-realmedia-vbr"},
      {u"ts"_s, "video/mp2t"},
      {u"webm"_s, "video/webm"},
      {u"wmv"_s, "video/x-ms-wmv"},
  };
  return types;
}

QString ffprobePath() {
  static const auto path = QStandardPaths::findExecutable(u"ffprobe"_s);
  return path;
}

}  // namespace

bool MediaInfo::hasTextSubtitles() const {
  static const QStringList textCodecs{u"ass"_s, u"ssa"_s, u"subrip"_s, u"srt"_s, u"webvtt"_s,
                                      u"mov_text"_s, u"text"_s};
  return subtitleStreams > 0 && textCodecs.contains(subtitleCodec);
}

bool isVideoFile(const QString& path) {
  return mimeTypes().contains(QFileInfo{path}.suffix().toLower());
}

QByteArray mimeType(const QString& path) {
  return mimeTypes().value(QFileInfo{path}.suffix().toLower(), "application/octet-stream");
}

QByteArray contentFeatures(bool transcoded) {
  // OP: seeking by byte range (01), or by time for transcoded streams (10)
  // CI: whether the content is converted
  // FLAGS: streaming transfer mode, background transfer mode, connection stalling, DLNA 1.5
  return transcoded ? "DLNA.ORG_OP=10;DLNA.ORG_CI=1;DLNA.ORG_FLAGS=01700000000000000000000000000000"
                    : "DLNA.ORG_OP=01;DLNA.ORG_CI=0;DLNA.ORG_FLAGS=01700000000000000000000000000000";
}

QByteArray protocolInfo(const QByteArray& mimeType, bool transcoded) {
  return "http-get:*:" + mimeType + ':' + contentFeatures(transcoded);
}

QString formatDuration(std::chrono::milliseconds duration) {
  using namespace std::chrono;
  const auto h = duration_cast<hours>(duration);
  const auto m = duration_cast<minutes>(duration - h);
  const auto s = duration_cast<seconds>(duration - h - m);
  const auto ms = duration - h - m - s;
  return u"%1:%2:%3.%4"_s.arg(h.count())
      .arg(m.count(), 2, 10, QChar{u'0'})
      .arg(s.count(), 2, 10, QChar{u'0'})
      .arg(ms.count(), 3, 10, QChar{u'0'});
}

////////////////////////////////////////////////////////////////////////////////

MediaInfoCache::MediaInfoCache(QObject* parent) : QObject(parent) {}

std::optional<MediaInfo> MediaInfoCache::find(const QString& path) const {
  const auto it = cache_.find(path);
  if (it == cache_.end()) return std::nullopt;

  const QFileInfo info{path};
  if (info.size() != it->size || info.lastModified() != it->modified) return std::nullopt;

  return it->info;
}

void MediaInfoCache::request(const QStringList& paths) {
  if (ffprobePath().isEmpty()) return;

  for (const auto& path : paths) {
    if (find(path) || queue_.contains(path)) continue;
    queue_.append(path);
  }

  while (running_ < kMaxConcurrentProbes && !queue_.isEmpty()) {
    probeNext();
  }
}

std::optional<MediaInfo> MediaInfoCache::probeNow(const QString& path) {
  if (const auto info = find(path)) return info;
  if (ffprobePath().isEmpty()) return std::nullopt;

  QProcess process;
  process.start(ffprobePath(), arguments(path));
  if (!process.waitForFinished(10000) || process.exitCode() != 0) return std::nullopt;

  const auto info = parse(process.readAllStandardOutput());
  if (info) store(path, *info);
  return info;
}

void MediaInfoCache::probeNext() {
  if (queue_.isEmpty()) return;

  const auto path = queue_.takeFirst();
  auto process = new QProcess(this);
  ++running_;

  connect(process, &QProcess::finished, this, [this, process, path](int exitCode) {
    --running_;
    if (exitCode == 0) {
      if (const auto info = parse(process->readAllStandardOutput())) store(path, *info);
    }
    process->deleteLater();
    while (running_ < kMaxConcurrentProbes && !queue_.isEmpty()) {
      probeNext();
    }
  });
  connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
    if (error != QProcess::FailedToStart) return;  // `finished` follows other errors
    --running_;
    queue_.clear();  // ffprobe is not usable
    process->deleteLater();
  });

  process->start(ffprobePath(), arguments(path));
}

void MediaInfoCache::store(const QString& path, const MediaInfo& info) {
  if (cache_.size() >= kMaxCacheSize) cache_.clear();

  const QFileInfo fileInfo{path};
  cache_.insert(path, Entry{
                          .size = fileInfo.size(),
                          .modified = fileInfo.lastModified(),
                          .info = info,
                      });
}

std::optional<MediaInfo> MediaInfoCache::parse(const QByteArray& json) {
  const auto root = QJsonDocument::fromJson(json).object();
  if (root.isEmpty()) return std::nullopt;

  MediaInfo info;

  const auto seconds = root.value("format").toObject().value("duration").toString().toDouble();
  info.duration = std::chrono::milliseconds{std::llround(seconds * 1000)};

  for (const QJsonValue& value : root.value("streams").toArray()) {
    const auto stream = value.toObject();
    const auto type = stream.value("codec_type").toString();
    if (type == u"video" && info.videoCodec.isEmpty()) {
      // Skip attached pictures (e.g. cover art in MKV/MP4)
      if (stream.value("disposition").toObject().value("attached_pic").toInt() == 1) continue;
      info.videoCodec = stream.value("codec_name").toString();
      info.width = stream.value("width").toInt();
      info.height = stream.value("height").toInt();
    } else if (type == u"subtitle") {
      if (++info.subtitleStreams == 1) info.subtitleCodec = stream.value("codec_name").toString();
    }
  }

  return info;
}

QStringList MediaInfoCache::arguments(const QString& path) {
  return {
      u"-v"_s,
      u"error"_s,
      u"-print_format"_s,
      u"json"_s,
      u"-show_entries"_s,
      u"format=duration:stream=codec_type,codec_name,width,height:stream_disposition=attached_pic"_s,
      path,
  };
}

}  // namespace media_server
