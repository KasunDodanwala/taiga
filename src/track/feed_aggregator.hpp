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

#include <QCoreApplication>
#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <optional>
#include <vector>

#include "track/feed.hpp"
#include "track/feed_filter.hpp"

class QTimer;

namespace track::feed {

// Episodes that exist in library folders, so that filters can discard what was already downloaded.
class LibraryIndex final {
public:
  void rebuildIfStale();
  void invalidate();

  bool contains(int animeId, int episodeNumber) const;
  std::optional<QString> folder(int animeId) const;

private:
  QHash<int, QSet<int>> episodes_;
  QHash<int, QString> folders_;
  QDateTime builtAt_;
};

class Aggregator final : public QObject {
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(Aggregator)

public:
  explicit Aggregator(QObject* parent);

  void init();

  const Feed& feed() const;
  bool isBusy() const;
  QString query() const;  // non-empty if the current feed is a search

  void check(bool automatic = false);
  void search(const QString& query);
  void reexamine();  // re-applies recognition and filters (e.g. after changing settings)

  void download(const QStringList& links);  // identified by item links
  void downloadSelected();
  void setItemState(const QString& link, ItemState state);

  std::vector<Filter> filters() const;
  void setFilters(const std::vector<Filter>& filters);
  void updateTimer();

  QString savePath(const Item& item);

signals:
  void busyChanged(bool busy);
  void feedChanged();
  void statusMessage(const QString& text, bool busy);
  void newTorrentsFound(const QStringList& titles);
  void downloadFinished(const QString& title, bool success, const QString& message);

private:
  void fetch(const QString& url, bool automatic);
  void examine();
  void sendToClient(const Item& item);
  void processQueue();
  void setBusy(bool busy);

  void loadArchive();
  void saveArchive() const;

  Item* findItem(const QString& link);

  Feed feed_;
  QString query_;
  QStringList archive_;
  QStringList queue_;
  LibraryIndex library_;
  QTimer* timer_ = nullptr;
  bool busy_ = false;
};

inline Aggregator* aggregator() {
  static auto aggregator = new Aggregator(qApp);
  return aggregator;
}

}  // namespace track::feed
