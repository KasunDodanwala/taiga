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

#include "library.hpp"

#include <QCollator>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <algorithm>
#include <map>

#include "base/string.hpp"
#include "media/anime.hpp"
#include "media/anime_db.hpp"
#include "media/anime_list.hpp"
#include "media/anime_list_utils.hpp"
#include "media/anime_utils.hpp"
#include "media_server/media_info.hpp"
#include "taiga/settings.hpp"
#include "track/episode.hpp"
#include "track/recognition.hpp"

namespace media_server {

namespace {

const auto kRootId = u"0"_s;
const auto kWatchingId = u"watching"_s;
const auto kRecentId = u"recent"_s;
constexpr size_t kRecentCount = 50;
constexpr qint64 kScanMaxAgeSeconds = 60;

bool lessThan(const Object& a, const Object& b) {
  static const QCollator collator = []() {
    QCollator collator;
    collator.setNumericMode(true);  // "Episode 2" before "Episode 10"
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    return collator;
  }();
  return collator.compare(a.title, b.title) < 0;
}

bool isHidden(const QFileInfo& info) {
  return info.fileName().startsWith(u'.');
}

QString episodeText(const std::pair<int, int>& range) {
  if (range.second > range.first) {
    return u"Episodes %1-%2"_s.arg(range.first, 2, 10, QChar{u'0'})
        .arg(range.second, 2, 10, QChar{u'0'});
  }
  return u"Episode %1"_s.arg(range.first, 2, 10, QChar{u'0'});
}

}  // namespace

QString Library::encodePath(const QString& path) {
  return QString::fromLatin1(path.toUtf8().toBase64(QByteArray::Base64UrlEncoding |
                                                    QByteArray::OmitTrailingEquals));
}

QString Library::decodePath(const QString& token) {
  const auto result = QByteArray::fromBase64Encoding(
      token.toLatin1(),
      QByteArray::Base64UrlEncoding | QByteArray::AbortOnBase64DecodingErrors);
  if (!result) return {};
  return QString::fromUtf8(*result);
}

void Library::setVirtualFoldersEnabled(bool enabled) {
  virtualFolders_ = enabled;
}

void Library::invalidate() {
  scannedAt_ = {};
}

QStringList Library::roots() const {
  QStringList roots;
  for (const auto& folder : taiga::settings.libraryFolders()) {
    const auto canonical = QFileInfo{QString::fromStdString(folder)}.canonicalFilePath();
    if (!canonical.isEmpty() && !roots.contains(canonical)) roots.append(canonical);
  }
  return roots;
}

std::optional<QString> Library::resolve(const QString& path) const {
  if (path.isEmpty()) return std::nullopt;

  // Resolves symbolic links and "..", so that nothing outside the library can be reached.
  const auto canonical = QFileInfo{path}.canonicalFilePath();
  if (canonical.isEmpty()) return std::nullopt;

  for (const auto& root : roots()) {
    if (canonical == root || canonical.startsWith(root + u'/')) return canonical;
  }

  return std::nullopt;
}

std::optional<Object> Library::object(const QString& id) {
  if (id == kRootId) {
    return Object{
        .id = kRootId,
        .parentId = u"-1"_s,
        .title = u"Taiga"_s,
        .isContainer = true,
        .childCount = static_cast<int>(rootChildren().size()),
    };
  }

  if (virtualFolders_ && id == kWatchingId) {
    return Object{
        .id = kWatchingId,
        .parentId = kRootId,
        .title = u"Currently watching"_s,
        .isContainer = true,
        .childCount = static_cast<int>(watchingChildren().size()),
    };
  }

  if (virtualFolders_ && id == kRecentId) {
    return Object{
        .id = kRecentId,
        .parentId = kRootId,
        .title = u"Recently added"_s,
        .isContainer = true,
        .childCount = static_cast<int>(recentChildren().size()),
    };
  }

  if (id.startsWith(u"d:"_s)) {
    const auto path = resolve(decodePath(id.mid(2)));
    if (!path || !QFileInfo{*path}.isDir()) return std::nullopt;
    return folderObject(*path);
  }

  if (id.startsWith(u"f:"_s)) {
    const auto path = resolve(decodePath(id.mid(2)));
    if (!path || !isVideoFile(*path)) return std::nullopt;
    const QFileInfo info{*path};
    return Object{
        .id = id,
        .parentId = u"d:"_s + encodePath(info.absolutePath()),
        .title = info.completeBaseName(),
        .path = *path,
    };
  }

  // Virtual containers and items: look them up among their parent's children
  QString parentId;
  if (id.startsWith(u"w:"_s)) {
    parentId = kWatchingId;
  } else if (id.startsWith(u"wf:"_s)) {
    parentId = u"w:"_s + id.section(u':', 1, 1);
  } else if (id.startsWith(u"rf:"_s)) {
    parentId = kRecentId;
  }

  if (!parentId.isEmpty() && virtualFolders_) {
    if (const auto siblings = children(parentId)) {
      const auto it = std::ranges::find(*siblings, id, &Object::id);
      if (it != siblings->end()) return *it;
    }
  }

  return std::nullopt;
}

std::optional<std::vector<Object>> Library::children(const QString& id) {
  if (id == kRootId) return rootChildren();

  if (virtualFolders_) {
    if (id == kWatchingId) return watchingChildren();
    if (id == kRecentId) return recentChildren();
    if (id.startsWith(u"w:"_s)) return animeChildren(id.mid(2).toInt());
  }

  if (id.startsWith(u"d:"_s)) return folderChildren(id, decodePath(id.mid(2)));

  // Items have no children
  if (object(id)) return std::vector<Object>{};

  return std::nullopt;
}

std::vector<Object> Library::rootChildren() {
  std::vector<Object> children;

  if (virtualFolders_) {
    children.push_back(*object(kWatchingId));
    children.push_back(*object(kRecentId));
  }

  for (const auto& root : roots()) {
    auto folder = folderObject(root);
    folder.parentId = kRootId;
    children.push_back(std::move(folder));
  }

  return children;
}

Object Library::folderObject(const QString& path) const {
  const QFileInfo info{path};
  const bool isRoot = roots().contains(path);

  const QDir dir{path};
  int childCount = 0;
  for (const auto& entry : dir.entryInfoList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot)) {
    if (!isHidden(entry) && (entry.isDir() || isVideoFile(entry.filePath()))) ++childCount;
  }

  return Object{
      .id = u"d:"_s + encodePath(path),
      .parentId = isRoot ? kRootId : u"d:"_s + encodePath(info.absolutePath()),
      .title = info.fileName(),
      .isContainer = true,
      .childCount = childCount,
      .path = path,
  };
}

std::optional<std::vector<Object>> Library::folderChildren(const QString& folderId,
                                                           const QString& path) {
  const auto resolved = resolve(path);
  if (!resolved || !QFileInfo{*resolved}.isDir()) return std::nullopt;

  std::vector<Object> folders;
  std::vector<Object> files;

  const QDir dir{*resolved};
  for (const auto& entry : dir.entryInfoList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot)) {
    if (isHidden(entry)) continue;
    if (entry.isDir()) {
      auto folder = folderObject(entry.absoluteFilePath());
      folder.parentId = folderId;
      folders.push_back(std::move(folder));
    } else if (isVideoFile(entry.filePath())) {
      files.push_back(Object{
          .id = u"f:"_s + encodePath(entry.absoluteFilePath()),
          .parentId = folderId,
          .title = entry.completeBaseName(),
          .path = entry.absoluteFilePath(),
      });
    }
  }

  std::ranges::sort(folders, lessThan);
  std::ranges::sort(files, lessThan);
  folders.insert(folders.end(), files.begin(), files.end());
  return folders;
}

std::vector<Object> Library::watchingChildren() {
  scanIfStale();

  std::map<int, int> fileCounts;
  for (const auto& file : files_) {
    const auto* entry = anime::db.entry(file.animeId);
    if (!anime::list::isInList(entry) || entry->status != anime::list::Status::Watching) continue;
    ++fileCounts[file.animeId];
  }

  std::vector<Object> children;
  for (const auto& [animeId, count] : fileCounts) {
    const auto* item = anime::db.item(animeId);
    if (!item) continue;
    children.push_back(Object{
        .id = u"w:%1"_s.arg(animeId),
        .parentId = kWatchingId,
        .title = QString::fromStdString(anime::preferredTitle(*item)),
        .isContainer = true,
        .childCount = count,
        .albumArtUrl = QString::fromStdString(item->image_url),
    });
  }

  std::ranges::sort(children, lessThan);
  return children;
}

std::vector<Object> Library::animeChildren(int animeId) {
  scanIfStale();

  std::vector<const IndexedFile*> files;
  for (const auto& file : files_) {
    if (file.animeId == animeId) files.push_back(&file);
  }

  std::ranges::sort(files, [](const IndexedFile* a, const IndexedFile* b) {
    const int x = a->episodes ? a->episodes->first : 0;
    const int y = b->episodes ? b->episodes->first : 0;
    return x != y ? x < y : a->path < b->path;
  });

  const auto* entry = anime::db.entry(animeId);
  const int watched = anime::list::isInList(entry) ? entry->watched_episodes : 0;
  const auto* item = anime::db.item(animeId);

  std::vector<Object> children;
  for (const auto* file : files) {
    QString title = QFileInfo{file->path}.completeBaseName();

    if (file->episodes) {
      title = episodeText(*file->episodes);

      // Tell different releases of the same episode apart
      const auto sameEpisode = std::ranges::count_if(files, [file](const IndexedFile* other) {
        return other->episodes == file->episodes;
      });
      if (sameEpisode > 1) {
        const auto release = u"%1 %2"_s.arg(file->group, file->resolution).trimmed();
        if (!release.isEmpty()) title += u" [%1]"_s.arg(release);
      }

      if (file->episodes->first == watched + 1) {
        title += u" · Next"_s;
      } else if (file->episodes->second <= watched) {
        title += u" · Watched"_s;
      }
    }

    children.push_back(Object{
        .id = u"wf:%1:%2"_s.arg(animeId).arg(encodePath(file->path)),
        .parentId = u"w:%1"_s.arg(animeId),
        .title = title,
        .path = file->path,
        .albumArtUrl = item ? QString::fromStdString(item->image_url) : QString{},
    });
  }

  return children;
}

std::vector<Object> Library::recentChildren() {
  scanIfStale();

  std::vector<const IndexedFile*> files;
  for (const auto& file : files_) files.push_back(&file);

  std::ranges::sort(files, [](const IndexedFile* a, const IndexedFile* b) {
    return a->modified > b->modified;
  });
  if (files.size() > kRecentCount) files.resize(kRecentCount);

  std::vector<Object> children;
  for (const auto* file : files) {
    QString title = QFileInfo{file->path}.completeBaseName();
    if (const auto* item = anime::db.item(file->animeId); item && file->episodes) {
      title = u"%1 - %2"_s.arg(QString::fromStdString(anime::preferredTitle(*item)),
                               episodeText(*file->episodes));
    }
    children.push_back(Object{
        .id = u"rf:"_s + encodePath(file->path),
        .parentId = kRecentId,
        .title = title,
        .path = file->path,
    });
  }

  return children;
}

void Library::scanIfStale() {
  const auto now = QDateTime::currentDateTimeUtc();
  if (scannedAt_.isValid() && scannedAt_.secsTo(now) < kScanMaxAgeSeconds) return;

  files_.clear();

  for (const auto& root : roots()) {
    QDirIterator it{root, QDir::Files, QDirIterator::Subdirectories};
    while (it.hasNext()) {
      const auto info = it.nextFileInfo();
      if (isHidden(info) || !isVideoFile(info.filePath())) continue;
      if (info.absoluteFilePath().contains(u"/."_s)) continue;  // inside a hidden folder

      auto episode = track::recognition::parseFileInfo(info);
      const int animeId = track::recognition::identify(episode);

      files_.push_back(IndexedFile{
          .path = info.absoluteFilePath(),
          .modified = info.lastModified(),
          .animeId = animeId,
          .episodes = episode.episodeNumberRange(),
          .group = QString::fromStdString(episode.element(anitomy::ElementKind::ReleaseGroup)),
          .resolution =
              QString::fromStdString(episode.element(anitomy::ElementKind::VideoResolution)),
      });
    }
  }

  scannedAt_ = now;
}

}  // namespace media_server
