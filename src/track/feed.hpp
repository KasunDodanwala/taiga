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
#include <QString>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "base/rss.hpp"
#include "track/episode.hpp"

namespace track::feed {

enum class Source {
  Unknown,
  Acgnx,
  AniDex,
  AnimeTosho,
  NyaaSi,
  SubsPlease,
  TokyoToshokan,
};

enum class ItemState {
  Blank,
  DiscardedNormal,
  DiscardedInactive,
  DiscardedHidden,
  Selected,
};

enum class TorrentCategory {
  Anime,
  Batch,
  Other,
};

struct Item : rss::Item {
  std::string info_link;
  std::string magnet_link;
  ItemState state = ItemState::Blank;
  TorrentCategory torrent_category = TorrentCategory::Anime;
  std::optional<int> seeders;
  std::optional<int> leechers;
  std::optional<int> downloads;
  uint64_t file_size = 0;
  Episode episode;
  bool new_episode = false;  // ahead of the user's progress
  std::string decided_by;    // name of the filter that last changed the state

  bool isDiscarded() const;
  bool isSameItem(const Item& item) const;
  std::optional<int> episodeNumber() const;  // highest number of the range
};

struct Feed {
  QString url;
  Source source = Source::Unknown;
  rss::Channel channel;
  std::vector<Item> items;
};

Source sourceFromUrl(const QString& url);

// Parses an RSS document, including source-specific extensions (e.g. `nyaa:seeders`)
bool parse(const QByteArray& data, Feed& feed, QString* error = nullptr);

uint64_t parseSize(const QString& text);
QString categoryName(TorrentCategory category);

}  // namespace track::feed
