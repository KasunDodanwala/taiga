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

#include "feed_aggregator.hpp"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTimer>
#include <QUrl>
#include <algorithm>
#include <chrono>

#include "base/log.hpp"
#include "base/string.hpp"
#include "media/anime.hpp"
#include "media/anime_db.hpp"
#include "media/anime_utils.hpp"
#include "taiga/network.hpp"
#include "taiga/path.hpp"
#include "taiga/settings.hpp"
#include "track/recognition.hpp"
#include "track/recognition_validate.hpp"
#include "track/torrent_client.hpp"

namespace track::feed {

namespace {

constexpr qsizetype kArchiveMaxCount = 1000;

QString archivePath() {
  return u"%1/torrent_archive.json"_s.arg(QString::fromStdString(taiga::get_data_path()));
}

QString sanitizeFileName(QString name) {
  static const QRegularExpression invalid{u"[<>:\"/\\\\|?*\\x00-\\x1F]"_s};
  name.replace(invalid, u" "_s);
  name = name.simplified();
  while (name.endsWith(u'.') || name.endsWith(u' ')) name.chop(1);
  return name;
}

// Selected items come first, then the rest in the order of the feed.
int statePriority(const ItemState state) {
  switch (state) {
    case ItemState::Selected:
      return 0;
    case ItemState::Blank:
      return 1;
    case ItemState::DiscardedNormal:
      return 2;
    case ItemState::DiscardedInactive:
      return 3;
    case ItemState::DiscardedHidden:
      return 4;
  }
  return 1;
}

TorrentCategory categorize(const Item& item) {
  // Respect the source's own categorization
  if (item.torrent_category != TorrentCategory::Anime) return item.torrent_category;
  if (QString::fromStdString(item.category.value).contains(u"Batch", Qt::CaseInsensitive)) {
    return TorrentCategory::Batch;
  }

  const auto range = item.episode.episodeNumberRange();
  if (range && range->second > range->first) return TorrentCategory::Batch;

  for (const auto& value : item.episode.elements(anitomy::ElementKind::ReleaseInformation)) {
    if (compareStrings(value, "batch", Qt::CaseInsensitive) == 0 ||
        compareStrings(value, "complete", Qt::CaseInsensitive) == 0) {
      return TorrentCategory::Batch;
    }
  }

  if (!recognition::isValidEpisodeType(item.episode)) return TorrentCategory::Other;

  return TorrentCategory::Anime;
}

// A bencoded dictionary with an "info" key, as opposed to e.g. an HTML error page
bool isTorrentFile(const QByteArray& data) {
  return data.startsWith('d') && data.contains("4:info");
}

}  // namespace

////////////////////////////////////////////////////////////////////////////////

void LibraryIndex::rebuildIfStale() {
  constexpr auto kMaxAge = std::chrono::minutes{5};

  const auto now = QDateTime::currentDateTimeUtc();
  if (builtAt_.isValid() &&
      builtAt_.secsTo(now) < std::chrono::duration_cast<std::chrono::seconds>(kMaxAge).count()) {
    return;
  }

  episodes_.clear();
  folders_.clear();

  for (const auto& folder : taiga::settings.libraryFolders()) {
    QDirIterator it{QString::fromStdString(folder), QDir::Files, QDirIterator::Subdirectories};
    while (it.hasNext()) {
      const auto info = it.nextFileInfo();
      auto episode = recognition::parseFileInfo(info);
      if (!recognition::isVideoFile(episode)) continue;

      const int id = recognition::identify(episode);
      if (id == anime::kUnknownId) continue;

      if (const auto range = episode.episodeNumberRange()) {
        for (int number = range->first; number <= range->second && number <= anime::kMaxEpisodeCount;
             ++number) {
          episodes_[id].insert(number);
        }
      } else {
        episodes_[id].insert(1);  // e.g. movies
      }

      if (!folders_.contains(id)) folders_.insert(id, info.absolutePath());
    }
  }

  builtAt_ = now;
}

void LibraryIndex::invalidate() {
  builtAt_ = {};
}

bool LibraryIndex::contains(int animeId, int episodeNumber) const {
  return episodes_.value(animeId).contains(episodeNumber);
}

std::optional<QString> LibraryIndex::folder(int animeId) const {
  if (const auto it = folders_.find(animeId); it != folders_.end()) return *it;
  return std::nullopt;
}

////////////////////////////////////////////////////////////////////////////////

Aggregator::Aggregator(QObject* parent) : QObject(parent), timer_(new QTimer(this)) {
  connect(timer_, &QTimer::timeout, this, [this]() { check(true); });
}

void Aggregator::init() {
  loadArchive();
  updateTimer();

  // Also check soon after startup, once the anime list has had a chance to synchronize.
  if (taiga::settings.torrentAutoCheckEnabled()) {
    QTimer::singleShot(std::chrono::minutes{2}, this, [this]() { check(true); });
  }
}

const Feed& Aggregator::feed() const {
  return feed_;
}

bool Aggregator::isBusy() const {
  return busy_;
}

QString Aggregator::query() const {
  return query_;
}

void Aggregator::check(bool automatic) {
  if (busy_) return;
  query_.clear();
  fetch(QString::fromStdString(taiga::settings.torrentFeedUrl()), automatic);
}

void Aggregator::search(const QString& query) {
  if (busy_ || query.trimmed().isEmpty()) return;

  auto url = QString::fromStdString(taiga::settings.torrentSearchUrl());
  const auto encoded = QString::fromUtf8(QUrl::toPercentEncoding(query.trimmed()));
  if (url.contains(u"%title%"_s)) {
    url.replace(u"%title%"_s, encoded);
  } else {
    url += (url.contains(u'?') ? u"&q="_s : u"?q="_s) + encoded;
  }

  query_ = query.trimmed();
  fetch(url, false);
}

void Aggregator::reexamine() {
  library_.invalidate();
  examine();
  emit feedChanged();
}

void Aggregator::fetch(const QString& url, bool automatic) {
  const QUrl feedUrl{url};
  if (!feedUrl.isValid() || feedUrl.scheme().isEmpty()) {
    emit statusMessage(u"Invalid feed URL: %1"_s.arg(url), false);
    return;
  }

  const auto host = feedUrl.host();
  setBusy(true);
  emit statusMessage(u"Checking new torrents via %1..."_s.arg(host), true);

  QNetworkRequest request{feedUrl};
  request.setHeaders(taiga::NetworkAccessManager::commonHeaders());
  request.setRawHeader("Accept", "application/rss+xml, application/xml;q=0.9, */*;q=0.8");

  auto reply = taiga::network()->get(request);

  connect(reply, &QNetworkReply::finished, this, [this, reply, url, host, automatic]() {
    setBusy(false);

    const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const auto server = reply->rawHeader("Server").toLower();

    if (reply->error() != QNetworkReply::NoError) {
      QString message;
      if ((status == 403 && server.startsWith("ddos-guard")) ||
          ((status == 429 || status == 503) && server.startsWith("cloudflare"))) {
        message = u"Cannot connect to %1 because of DDoS protection"_s.arg(host);
      } else if (status >= 400) {
        message = u"%1 returned an error (%2)"_s.arg(host).arg(status);
      } else {
        message = u"Cannot connect to %1: %2"_s.arg(host, reply->errorString());
      }
      emit statusMessage(message, false);
      return;
    }

    Feed feed;
    feed.url = url;
    if (QString error; !parse(reply->readAll(), feed, &error)) {
      emit statusMessage(u"Could not read the feed from %1: %2"_s.arg(host, error), false);
      return;
    }

    feed_ = std::move(feed);
    examine();

    QStringList selected;
    for (const auto& item : feed_.items) {
      if (item.state == ItemState::Selected) selected.append(QString::fromStdString(item.title));
    }

    emit statusMessage(selected.isEmpty()
                           ? u"No new torrents found (%1 items)"_s.arg(feed_.items.size())
                           : u"%1 torrents marked for download"_s.arg(selected.size()),
                       false);
    emit feedChanged();

    if (automatic && !selected.isEmpty()) {
      const auto action = taiga::settings.torrentNewAction();
      if (action == "download") {
        downloadSelected();
      } else if (action == "notify") {
        emit newTorrentsFound(selected);
      }
    }
  });
}

void Aggregator::examine() {
  library_.rebuildIfStale();

  for (auto& item : feed_.items) {
    item.state = ItemState::Blank;
    item.decided_by.clear();
    item.episode = recognition::parse(item.title);
    item.episode.setAnimeId(recognition::identify(item.episode));
    item.torrent_category = categorize(item);
  }

  markNewEpisodes(feed_);

  if (taiga::settings.torrentFilterEnabled()) {
    const auto isAvailable = [this](int animeId, int number) {
      return library_.contains(animeId, number);
    };
    applyFilters(feed_, filters(), isAvailable);
  }

  // Archived items must be discarded after other filters are processed.
  for (auto& item : feed_.items) {
    if (!item.isDiscarded() && archive_.contains(QString::fromStdString(item.title))) {
      item.state = ItemState::DiscardedNormal;
      item.decided_by = "Downloaded before";
    }
  }

  std::ranges::stable_sort(feed_.items, {},
                           [](const Item& item) { return statePriority(item.state); });
}

void Aggregator::download(const QStringList& links) {
  for (const auto& link : links) {
    if (!link.isEmpty() && !queue_.contains(link)) queue_.append(link);
  }
  if (!busy_) processQueue();
}

void Aggregator::downloadSelected() {
  std::vector<const Item*> selected;
  for (const auto& item : feed_.items) {
    if (item.state == ItemState::Selected) selected.push_back(&item);
  }

  // Older episodes first, so that they appear in order in the BitTorrent client
  std::ranges::stable_sort(selected, [](const Item* a, const Item* b) {
    if (a->episode.animeId() != b->episode.animeId()) {
      return a->episode.animeId() < b->episode.animeId();
    }
    return a->episodeNumber().value_or(0) < b->episodeNumber().value_or(0);
  });

  QStringList links;
  for (const auto* item : selected) links.append(QString::fromStdString(item->link));

  if (links.isEmpty()) {
    emit statusMessage(u"There are no torrents marked for download"_s, false);
    return;
  }

  download(links);
}

void Aggregator::setItemState(const QString& link, ItemState state) {
  if (auto* item = findItem(link)) {
    item->state = state;
  }
}

std::vector<Filter> Aggregator::filters() const {
  if (const auto json = taiga::settings.torrentFilters()) {
    return filtersFromJson(*json);
  }
  return defaultFilters();
}

void Aggregator::setFilters(const std::vector<Filter>& filters) {
  taiga::settings.setTorrentFilters(toJson(filters));
}

void Aggregator::updateTimer() {
  if (taiga::settings.torrentAutoCheckEnabled()) {
    timer_->start(taiga::settings.torrentAutoCheckInterval());
  } else {
    timer_->stop();
  }
}

QString Aggregator::savePath(const Item& item) {
  const int animeId = item.episode.animeId();
  const auto* anime = anime::db.item(animeId);

  if (anime) {
    // A folder chosen for this anime, or the one that already has its episodes
    if (const auto* settings = anime::db.settings(animeId); settings && !settings->folder.empty()) {
      const auto folder = QString::fromStdString(settings->folder);
      if (QDir{folder}.exists()) return folder;
    }
    if (const auto folder = library_.folder(animeId)) return *folder;
  }

  auto base = QString::fromStdString(taiga::settings.torrentDownloadPath());
  if (base.isEmpty()) {
    if (const auto folders = taiga::settings.libraryFolders(); !folders.empty()) {
      base = QString::fromStdString(folders.front());
    }
  }
  if (base.isEmpty()) return {};  // the client's default

  if (!taiga::settings.torrentCreateSubfolder()) return base;

  const auto name = sanitizeFileName(
      anime ? QString::fromStdString(anime::preferredTitle(*anime))
            : QString::fromStdString(item.episode.element(anitomy::ElementKind::Title)));

  return name.isEmpty() ? base : QDir{base}.filePath(name);
}

void Aggregator::sendToClient(const Item& item) {
  setBusy(true);

  const auto title = QString::fromStdString(item.title);
  const auto link = QString::fromStdString(item.link);
  const bool useMagnet =
      !item.magnet_link.empty() && (taiga::settings.torrentUseMagnet() || item.link.empty());
  const bool isMagnet = useMagnet || link.startsWith(u"magnet:"_s);

  torrent::AddRequest request{
      .urls = useMagnet ? QString::fromStdString(item.magnet_link) : link,
      .torrentData = {},
      .torrentFileName = sanitizeFileName(title) + u".torrent"_s,
      .savePath = savePath(item),
      .category = QString::fromStdString(taiga::settings.torrentCategory()),
      .tags = u"taiga"_s,
  };

  const auto finish = [this, link, title](bool success, const QString& message) {
    setBusy(false);

    if (success) {
      if (auto* item = findItem(link)) {
        item->state = ItemState::DiscardedNormal;
        item->decided_by = "Downloaded";
      }
      archive_.removeAll(title);
      archive_.append(title);
      saveArchive();
      library_.invalidate();
      emit statusMessage(u"Added to qBittorrent: %1"_s.arg(title), false);
      emit feedChanged();
    } else {
      emit statusMessage(u"Could not add \"%1\": %2"_s.arg(title, message), false);
    }

    emit downloadFinished(title, success, message);
    processQueue();
  };

  emit statusMessage(u"Adding \"%1\" to qBittorrent..."_s.arg(title), true);

  if (isMagnet) {
    torrent::qbittorrent()->add(request, finish);
    return;
  }

  // Download the .torrent file ourselves, as Taiga always did. If that fails, the client gets the
  // URL and can try on its own.
  QNetworkRequest networkRequest{QUrl{link}};
  networkRequest.setHeaders(taiga::NetworkAccessManager::commonHeaders());
  networkRequest.setRawHeader("Accept", "application/x-bittorrent, */*");

  auto reply = taiga::network()->get(networkRequest);

  connect(reply, &QNetworkReply::finished, this, [reply, request, finish]() mutable {
    const auto data = reply->readAll();
    if (reply->error() == QNetworkReply::NoError && isTorrentFile(data)) {
      request.torrentData = data;
    } else {
      qWarning() << "Could not download torrent file:" << reply->url() << reply->errorString();
    }
    torrent::qbittorrent()->add(request, finish);
  });
}

void Aggregator::processQueue() {
  while (!queue_.isEmpty()) {
    const auto link = queue_.takeFirst();
    if (const auto* item = findItem(link)) {
      sendToClient(*item);
      return;
    }
  }
}

void Aggregator::setBusy(bool busy) {
  if (busy_ == busy) return;
  busy_ = busy;
  emit busyChanged(busy);
}

void Aggregator::loadArchive() {
  QFile file{archivePath()};
  if (!file.open(QIODevice::ReadOnly)) return;

  archive_.clear();
  for (const QJsonValue& value : QJsonDocument::fromJson(file.readAll()).array()) {
    archive_.append(value.toString());
  }
}

void Aggregator::saveArchive() const {
  auto titles = archive_;
  if (titles.size() > kArchiveMaxCount) titles = titles.mid(titles.size() - kArchiveMaxCount);

  QSaveFile file{archivePath()};
  if (!file.open(QIODevice::WriteOnly)) return;
  file.write(QJsonDocument{QJsonArray::fromStringList(titles)}.toJson());
  file.commit();
}

Item* Aggregator::findItem(const QString& link) {
  const auto it = std::ranges::find_if(
      feed_.items, [&link](const Item& item) { return QString::fromStdString(item.link) == link; });
  return it != feed_.items.end() ? &*it : nullptr;
}

}  // namespace track::feed
