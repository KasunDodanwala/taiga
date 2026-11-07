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
#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <chrono>
#include <optional>

namespace media_server {

struct MediaInfo {
  std::chrono::milliseconds duration{0};
  int width = 0;
  int height = 0;
  QString videoCodec;
  int subtitleStreams = 0;
  QString subtitleCodec;  // of the first subtitle stream

  // Text subtitles (e.g. ASS/SSA, SRT) can be rendered into the video by FFmpeg's `subtitles`
  // filter; image-based ones (e.g. PGS from Blu-ray rips) can't.
  bool hasTextSubtitles() const;
};

bool isVideoFile(const QString& path);
QByteArray mimeType(const QString& path);

// `protocolInfo` of a resource, which tells clients how it can be streamed
QByteArray protocolInfo(const QByteArray& mimeType, bool transcoded);
QByteArray contentFeatures(bool transcoded);

QString formatDuration(std::chrono::milliseconds duration);  // H:MM:SS.mmm

// Durations and stream details from `ffprobe`, gathered in the background so that browsing never
// has to wait for them.
class MediaInfoCache final : public QObject {
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(MediaInfoCache)

public:
  explicit MediaInfoCache(QObject* parent);

  std::optional<MediaInfo> find(const QString& path) const;
  void request(const QStringList& paths);
  std::optional<MediaInfo> probeNow(const QString& path);

private:
  struct Entry {
    qint64 size = 0;
    QDateTime modified;
    MediaInfo info;
  };

  void probeNext();
  void store(const QString& path, const MediaInfo& info);
  static std::optional<MediaInfo> parse(const QByteArray& json);
  static QStringList arguments(const QString& path);

  QHash<QString, Entry> cache_;
  QStringList queue_;
  int running_ = 0;
};

}  // namespace media_server
