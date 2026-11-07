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

#include <QDateTime>
#include <QString>
#include <QStringList>
#include <optional>
#include <utility>
#include <vector>

namespace media_server {

// An object of the ContentDirectory service: a container (folder) or an item (video file)
struct Object {
  QString id{};
  QString parentId{};
  QString title{};
  bool isContainer = false;
  int childCount = 0;
  QString path{};         // folder or file on disk
  QString albumArtUrl{};  // e.g. the anime's cover image
};

// Maps the library folders (Settings > Library) to ContentDirectory objects. Besides plain folder
// browsing, it provides "Currently watching" (episodes of anime that you're watching, next
// episode marked) and "Recently added".
class Library final {
public:
  std::optional<Object> object(const QString& id);
  std::optional<std::vector<Object>> children(const QString& id);

  // Canonical path, if it's inside one of the library folders (never serve anything else)
  std::optional<QString> resolve(const QString& path) const;

  void setVirtualFoldersEnabled(bool enabled);
  void invalidate();

  static QString encodePath(const QString& path);
  static QString decodePath(const QString& token);

private:
  struct IndexedFile {
    QString path;
    QDateTime modified;
    int animeId = 0;
    std::optional<std::pair<int, int>> episodes;
    QString group;
    QString resolution;
  };

  QStringList roots() const;
  std::vector<Object> rootChildren();
  std::optional<std::vector<Object>> folderChildren(const QString& folderId, const QString& path);
  std::vector<Object> watchingChildren();
  std::vector<Object> animeChildren(int animeId);
  std::vector<Object> recentChildren();
  Object folderObject(const QString& path) const;
  void scanIfStale();

  std::vector<IndexedFile> files_;
  QDateTime scannedAt_;
  bool virtualFolders_ = true;
};

}  // namespace media_server
