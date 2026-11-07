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

#include "feed.hpp"

#include <QRegularExpression>
#include <QUrl>
#include <QXmlStreamReader>
#include <array>
#include <cmath>
#include <format>
#include <utility>

#include "base/string.hpp"

namespace track::feed {

namespace {

QString between(const QString& str, const QString& begin, const QString& end) {
  const auto start = str.indexOf(begin);
  if (start < 0) return {};
  const auto from = start + begin.size();
  if (end.isEmpty()) return str.mid(from);
  const auto stop = str.indexOf(end, from);
  return stop < 0 ? QString{} : str.mid(from, stop - from);
}

std::optional<int> toOptionalInt(const std::string& value) {
  bool ok = false;
  const int number = QString::fromStdString(value).trimmed().toInt(&ok);
  return ok ? std::optional{number} : std::nullopt;
}

void tidyDescription(std::string& description) {
  auto str = QString::fromStdString(description);
  str.replace(u"</p>"_s, u"\n"_s);
  str.replace(u"<br/>"_s, u"\n"_s);
  str.replace(u"<br />"_s, u"\n"_s);
  str.replace(u"<br>"_s, u"\n"_s);
  removeHtmlTags(str);
  const auto lines = str.split(u'\n', Qt::SkipEmptyParts);
  QStringList parts;
  for (const auto& line : lines) {
    if (const auto part = line.simplified(); !part.isEmpty()) parts.append(part);
  }
  description = parts.join(u" | "_s).toStdString();
}

void parseMagnetLink(Item& item) {
  if (item.enclosure.url.starts_with("magnet:")) {
    item.magnet_link = item.enclosure.url;
    return;
  }
  const auto magnet =
      between(QString::fromStdString(item.description), u"<a href=\"magnet:?"_s, u"\">"_s);
  if (!magnet.isEmpty()) item.magnet_link = u"magnet:?%1"_s.arg(magnet).toStdString();
}

void parseItemFromSource(const Source source, Item& item) {
  const auto description = QString::fromStdString(item.description);
  auto& elements = item.namespace_elements;

  switch (source) {
    case Source::Acgnx: {
      const auto parts = description.split(u" | "_s);
      if (parts.size() > 2) item.file_size = parseSize(parts.at(2));
      parseMagnetLink(item);
      break;
    }
    case Source::AniDex:
      item.file_size = parseSize(between(description, u"Size: "_s, u" |"_s));
      if (description.contains(u" Batch "_s)) item.torrent_category = TorrentCategory::Batch;
      parseMagnetLink(item);
      break;
    case Source::AnimeTosho:
      if (item.link.find("/view/") != std::string::npos && !item.enclosure.url.empty()) {
        item.link = item.enclosure.url;
      }
      item.info_link = item.guid.value;
      item.file_size = parseSize(between(description, u"<strong>Total Size</strong>: "_s, u"<"_s));
      parseMagnetLink(item);
      break;
    case Source::NyaaSi:
      item.info_link = item.guid.value;
      if (elements.contains("nyaa:size")) item.file_size = parseSize(QString::fromStdString(elements["nyaa:size"]));
      if (elements.contains("nyaa:seeders")) item.seeders = toOptionalInt(elements["nyaa:seeders"]);
      if (elements.contains("nyaa:leechers")) item.leechers = toOptionalInt(elements["nyaa:leechers"]);
      if (elements.contains("nyaa:downloads")) item.downloads = toOptionalInt(elements["nyaa:downloads"]);
      if (elements.contains("nyaa:infoHash") && !elements["nyaa:infoHash"].empty()) {
        const auto title = QUrl::toPercentEncoding(QString::fromStdString(item.title));
        item.magnet_link = std::format("magnet:?xt=urn:btih:{}&dn={}", elements["nyaa:infoHash"],
                                       title.toStdString());
      }
      break;
    case Source::SubsPlease:
      if (elements.contains("subsplease:size")) {
        item.file_size = parseSize(QString::fromStdString(elements["subsplease:size"]));
      }
      parseMagnetLink(item);
      break;
    case Source::TokyoToshokan:
      item.file_size = parseSize(between(description, u"Size: "_s, u"<"_s));
      parseMagnetLink(item);
      item.description = between(description, u"Comment: "_s, {}).toStdString();
      item.info_link = item.guid.value;
      break;
    case Source::Unknown:
      parseMagnetLink(item);
      if (!item.enclosure.length.empty()) {
        item.file_size = QString::fromStdString(item.enclosure.length).toULongLong();
      }
      break;
  }

  if (item.link.empty() && !item.enclosure.url.empty()) item.link = item.enclosure.url;

  tidyDescription(item.description);
}

}  // namespace

bool Item::isDiscarded() const {
  switch (state) {
    case ItemState::DiscardedNormal:
    case ItemState::DiscardedInactive:
    case ItemState::DiscardedHidden:
      return true;
    default:
      return false;
  }
}

bool Item::isSameItem(const Item& item) const {
  // Check for guid element first
  if (guid.is_permalink && item.guid.is_permalink && !guid.value.empty()) {
    if (guid.value == item.guid.value) return true;
  }
  // Fall back to link element, then title element
  if (!link.empty() && link == item.link) return true;
  if (!title.empty() && title == item.title) return true;
  return false;
}

std::optional<int> Item::episodeNumber() const {
  const auto range = episode.episodeNumberRange();
  if (!range) return std::nullopt;
  return range->second;
}

Source sourceFromUrl(const QString& url) {
  static constexpr std::array<std::pair<std::string_view, Source>, 9> sources{{
      {"acgnx", Source::Acgnx},
      {"anidex", Source::AniDex},
      {"animetosho", Source::AnimeTosho},
      {"nyaa.si", Source::NyaaSi},
      {"nyaa.land", Source::NyaaSi},
      {"nyaa.iss", Source::NyaaSi},
      {"subsplease", Source::SubsPlease},
      {"tokyotosho", Source::TokyoToshokan},
      {"tokyo-tosho", Source::TokyoToshokan},
  }};

  const auto host = QUrl{url}.host().toLower().toStdString();
  for (const auto& [name, source] : sources) {
    if (host.find(name) != std::string::npos) return source;
  }
  return Source::Unknown;
}

bool parse(const QByteArray& data, Feed& feed, QString* error) {
  feed.channel = {};
  feed.items.clear();

  QXmlStreamReader xml{data};
  std::optional<Item> item;
  bool foundChannel = false;

  const auto text = [&xml]() {
    return xml.readElementText(QXmlStreamReader::IncludeChildElements).trimmed().toStdString();
  };
  const auto attribute = [&xml](QAnyStringView name) {
    return xml.attributes().value(name).toString().toStdString();
  };

  while (!xml.atEnd()) {
    xml.readNext();

    if (xml.isEndElement() && xml.qualifiedName() == u"item" && item) {
      feed.items.push_back(std::move(*item));
      item.reset();
      continue;
    }

    if (!xml.isStartElement()) continue;

    const auto name = xml.qualifiedName();

    if (name == u"channel") {
      foundChannel = true;
    } else if (name == u"item") {
      item.emplace();
    } else if (item) {
      if (name == u"title") {
        item->title = text();
      } else if (name == u"link") {
        item->link = text();
      } else if (name == u"description") {
        item->description = text();
      } else if (name == u"category") {
        item->category.domain = attribute(u"domain");
        item->category.value = text();
      } else if (name == u"guid") {
        item->guid.is_permalink = attribute(u"isPermaLink") != "false";
        item->guid.value = text();
      } else if (name == u"pubDate") {
        item->pub_date = text();
      } else if (name == u"enclosure") {
        item->enclosure.url = attribute(u"url");
        item->enclosure.length = attribute(u"length");
        item->enclosure.type = attribute(u"type");
      } else if (name.contains(u':')) {
        item->namespace_elements[name.toString().toStdString()] = text();
      }
    } else if (foundChannel) {
      if (name == u"title") {
        feed.channel.title = text();
      } else if (name == u"link") {
        feed.channel.link = text();
      } else if (name == u"description") {
        feed.channel.description = text();
      }
    }
  }

  if (xml.hasError() && feed.items.empty()) {
    if (error) *error = xml.errorString();
    return false;
  }

  if (!foundChannel) {
    if (error) *error = u"Not an RSS feed"_s;
    return false;
  }

  feed.source = sourceFromUrl(feed.url);
  for (auto& feedItem : feed.items) {
    parseItemFromSource(feed.source, feedItem);
  }

  return true;
}

uint64_t parseSize(const QString& text) {
  static const QRegularExpression re{u"([\\d.,]+)\\s*([KMGT]?i?B|bytes?)?"_s,
                                     QRegularExpression::CaseInsensitiveOption};

  const auto match = re.match(text);
  if (!match.hasMatch()) return 0;

  auto number = match.captured(1);
  // "1,234.5 MiB" or "1,5 GiB"
  if (number.contains(u'.')) {
    number.remove(u',');
  } else {
    number.replace(u',', u'.');
  }

  bool ok = false;
  const double value = number.toDouble(&ok);
  if (!ok) return 0;

  const auto unit = match.captured(2).toUpper();
  double multiplier = 1.0;
  if (unit.startsWith(u'K')) multiplier = 1024.0;
  else if (unit.startsWith(u'M')) multiplier = 1024.0 * 1024;
  else if (unit.startsWith(u'G')) multiplier = 1024.0 * 1024 * 1024;
  else if (unit.startsWith(u'T')) multiplier = 1024.0 * 1024 * 1024 * 1024;

  return static_cast<uint64_t>(std::llround(value * multiplier));
}

QString categoryName(TorrentCategory category) {
  switch (category) {
    default:
    case TorrentCategory::Anime:
      return u"Anime"_s;
    case TorrentCategory::Batch:
      return u"Batch"_s;
    case TorrentCategory::Other:
      return u"Other"_s;
  }
}

}  // namespace track::feed
