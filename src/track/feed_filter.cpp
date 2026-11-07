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

#include "feed_filter.hpp"

#include <QByteArray>
#include <QJsonObject>
#include <QXmlStreamReader>
#include <QtEndian>
#include <algorithm>
#include <map>
#include <ranges>
#include <utility>

#include "base/string.hpp"
#include "media/anime.hpp"
#include "media/anime_db.hpp"
#include "media/anime_list.hpp"
#include "media/anime_list_utils.hpp"
#include "media/anime_utils.hpp"
#include "sync/service.hpp"
#include "taiga/accounts.hpp"
#include "taiga/version.hpp"
#include "track/feed.hpp"

namespace track::feed {

namespace {

////////////////////////////////////////////////////////////////////////////////
// String functions of Taiga 1.x (base/string.cpp, base/file.cpp), with the same semantics

// `_wtoi`: leading whitespace, an optional sign, then digits up to the first non-digit
long long toInt(const QString& str) {
  qsizetype i = 0;
  while (i < str.size() && str[i].isSpace()) ++i;
  bool negative = false;
  if (i < str.size() && (str[i] == u'-' || str[i] == u'+')) negative = str[i++] == u'-';
  long long value = 0;
  for (; i < str.size() && str[i] >= u'0' && str[i] <= u'9'; ++i) {
    value = value * 10 + (str[i].unicode() - u'0');
  }
  return negative ? -value : value;
}

// `wcstoull` in base 10
uint64_t toUint64(const QString& str) {
  return static_cast<uint64_t>(std::max(0LL, toInt(str)));
}

bool isEqual(const QString& str1, const QString& str2) {
  return str1.size() == str2.size() && str1.compare(str2, Qt::CaseInsensitive) == 0;
}

// `_wcsnicmp` with MAX_PATH characters
int compareStrings(const QString& str1, const QString& str2) {
  constexpr qsizetype kMaxCount = 260;
  return str1.left(kMaxCount).compare(str2.left(kMaxCount), Qt::CaseInsensitive);
}

bool startsWith(const QString& str1, const QString& str2) {
  return str1.startsWith(str2, Qt::CaseSensitive);
}

bool endsWith(const QString& str1, const QString& str2) {
  return str1.endsWith(str2, Qt::CaseSensitive);
}

int inStr(const QString& str1, const QString& str2, bool caseInsensitive) {
  if (str1.isEmpty()) return -1;
  if (str2.isEmpty()) return 0;
  if (str1.size() < str2.size()) return -1;
  return static_cast<int>(
      str1.indexOf(str2, 0, caseInsensitive ? Qt::CaseInsensitive : Qt::CaseSensitive));
}

bool isNumericString(const QString& str) {
  return !str.isEmpty() &&
         std::ranges::all_of(str, [](QChar c) { return c >= u'0' && c <= u'9'; });
}

uint64_t parseSizeString(QString value) {
  QString unit;
  qsizetype pos = 0;
  while (pos < value.size() && (value[pos].isDigit() || value[pos] == u'.')) ++pos;
  if (pos < value.size()) {
    unit = value.mid(pos).trimmed();
    value.truncate(pos);
  }

  const uint64_t size = [&unit]() -> uint64_t {
    if (isEqual(unit, u"KB"_s)) return 1000ULL;
    if (isEqual(unit, u"KiB"_s)) return 1024ULL;
    if (isEqual(unit, u"MB"_s)) return 1000000ULL;
    if (isEqual(unit, u"MiB"_s)) return 1048576ULL;
    if (isEqual(unit, u"GB"_s)) return 1000000000ULL;
    if (isEqual(unit, u"GiB"_s)) return 1073741824ULL;
    if (isEqual(unit, u"TB"_s)) return 1000000000000ULL;
    if (isEqual(unit, u"TiB"_s)) return 1099511627776ULL;
    return 1;
  }();

  return static_cast<uint64_t>(static_cast<double>(size) * value.toDouble());
}

// track/episode_util.cpp
int getVideoResolutionHeight(const QString& str) {
  // *###x###*
  if (str.size() > 6) {
    const auto pos = str.indexOf(u'x');
    if (pos > -1) {
      for (qsizetype i = 0; i < str.size(); ++i) {
        if (i != pos && !(str[i] >= u'0' && str[i] <= u'9')) return 0;
      }
      return static_cast<int>(toInt(str.mid(pos + 1)));
    }
  // *###p
  } else if (str.size() > 3) {
    if (str.back() == u'p') {
      for (qsizetype i = 0; i < str.size() - 1; ++i) {
        if (!(str[i] >= u'0' && str[i] <= u'9')) return 0;
      }
      return static_cast<int>(toInt(str.left(str.size() - 1)));
    }
  }

  // 1080, 720, 480
  if (isNumericString(str)) {
    const auto height = static_cast<int>(toInt(str));
    switch (height) {
      case 1080:
      case 720:
      case 480:
        return height;
    }
  }

  return 0;
}

QString translateNumber(int value, const QString& defaultText) {
  return value > 0 ? QString::number(value) : defaultText;
}

QString seasonString(const FuzzyDate& date) {
  if (date.empty()) return {};
  const auto text = QString::fromStdString(date.to_string());  // YYYY-MM-DD
  const int year = text.left(4).toInt();
  const int month = text.mid(5, 2).toInt();
  if (year <= 0 || month <= 0) return {};
  static const char* names[] = {"Winter", "Spring", "Summer", "Fall"};
  return u"%1 %2"_s.arg(QString::fromLatin1(names[((month % 12) / 3)])).arg(year);
}

////////////////////////////////////////////////////////////////////////////////
// Evaluation (track/feed_filter.cpp)

using ElementKind = anitomy::ElementKind;

QString elementText(const Item& item, ElementKind kind) {
  return QString::fromStdString(item.episode.element(kind));
}

int episodeHigh(const Item& item) {
  return item.episodeNumber().value_or(0);
}

// taiga/script.cpp: ReplaceVariables
QString replaceVariables(const QString& value, const Item& item) {
  if (!value.contains(u'%')) return value;

  const int animeId = item.episode.animeId();
  const auto* anime = anime::db.item(animeId);
  const auto* entry = anime::db.entry(animeId);
  const bool inList = anime::list::isInList(entry);

  std::map<QString, QString> fields;
  fields[u"title"_s] = anime ? QString::fromStdString(anime::preferredTitle(*anime))
                             : elementText(item, ElementKind::Title);
  if (anime) {
    fields[u"watched"_s] = translateNumber(inList ? entry->watched_episodes : 0, {});
    fields[u"total"_s] = translateNumber(anime->episode_count, {});
    fields[u"score"_s] = translateNumber(inList ? entry->score : 0, {});
    fields[u"season"_s] = seasonString(anime->date_started);
    fields[u"id"_s] = QString::number(anime->id);
    fields[u"image"_s] = QString::fromStdString(anime->image_url);
    fields[u"status"_s] =
        QString::number(static_cast<int>(inList ? entry->status : anime::list::Status::NotInList));
    fields[u"rewatching"_s] = QString::number(inList && entry->rewatching ? 1 : 0);
    fields[u"notes"_s] = inList ? QString::fromStdString(entry->notes) : QString{};
    fields[u"animeurl"_s] = sync::animePageUrl(anime->id);
  }

  auto episodeNumber = QString::number(episodeHigh(item));
  while (episodeNumber.startsWith(u'0')) episodeNumber.remove(0, 1);

  fields[u"name"_s] = elementText(item, ElementKind::EpisodeTitle);
  fields[u"episode"_s] = episodeNumber;
  fields[u"version"_s] = QString::fromStdString(item.episode.element(ElementKind::ReleaseVersion, "1"));
  fields[u"group"_s] = elementText(item, ElementKind::ReleaseGroup);
  fields[u"resolution"_s] = elementText(item, ElementKind::VideoResolution);
  fields[u"video"_s] = QString::fromStdString(
      [&item]() {
        std::string terms;
        for (const auto& term : item.episode.elements(ElementKind::VideoTerm)) {
          if (!terms.empty()) terms += ' ';
          terms += term;
        }
        return terms;
      }());
  fields[u"audio"_s] = elementText(item, ElementKind::AudioTerm);
  fields[u"checksum"_s] = elementText(item, ElementKind::FileChecksum);
  fields[u"user"_s] = QString::fromStdString(taiga::accounts.serviceUsername(
      sync::serviceSlug(sync::currentServiceId()).toStdString()));

  auto result = value;
  for (const auto& [name, replacement] : fields) {
    result.replace(u'%' + name + u'%', replacement);
  }
  // Variables without a value are replaced with nothing
  for (const auto* name : {"title", "watched", "total", "score", "season", "id", "image", "status",
                           "rewatching", "notes", "animeurl", "file", "folder", "playstatus",
                           "manual"}) {
    result.replace(u"%%1%"_s.arg(QLatin1StringView{name}), QString{});
  }
  return result;
}

QString elementValue(const Condition& condition, const Item& item,
                     const episode_available_t& isEpisodeAvailable) {
  const int animeId = item.episode.animeId();
  const auto* anime = anime::db.item(animeId);
  const auto* entry = anime::db.entry(animeId);
  const bool inList = anime::list::isInList(entry);

  switch (condition.element) {
    using enum Element;
    case FileTitle:
      return QString::fromStdString(item.title);
    case FileCategory:
      return categoryName(item.torrent_category);
    case FileDescription:
      return QString::fromStdString(item.description);
    case FileLink:
      return QString::fromStdString(item.link);
    case FileSize:
      return QString::number(item.file_size);
    case MetaId:
      return QString::number(anime ? anime->id : anime::kUnknownId);
    case EpisodeTitle:
      return elementText(item, ElementKind::Title);
    case MetaDateStart:
      return anime ? QString::fromStdString(anime->date_started.to_string()) : QString{};
    case MetaDateEnd:
      return anime ? QString::fromStdString(anime->date_finished.to_string()) : QString{};
    case MetaEpisodes:
      return anime ? QString::number(anime->episode_count) : QString{};
    case MetaStatus:
      return QString::number(static_cast<int>(anime ? anime->status : anime::Status::Unknown));
    case MetaType:
      return QString::number(static_cast<int>(anime ? anime->type : anime::Type::Unknown));
    case UserNotes:
      return anime && inList ? QString::fromStdString(entry->notes) : QString{};
    case UserStatus:
      return QString::number(static_cast<int>(
          anime && inList ? entry->status : anime::list::Status::NotInList));
    case EpisodeNumber:
      if (!item.episodeNumber()) {
        return anime ? QString::number(anime->episode_count) : QString{};
      }
      return QString::number(episodeHigh(item));
    case EpisodeVersion:
      return QString::fromStdString(item.episode.element(ElementKind::ReleaseVersion, "1"));
    case LocalEpisodeAvailable:
      if (!anime) return {};
      return isEpisodeAvailable && isEpisodeAvailable(animeId, episodeHigh(item)) ? u"1"_s
                                                                                  : u"0"_s;
    case EpisodeGroup:
      return elementText(item, ElementKind::ReleaseGroup);
    case EpisodeVideoResolution:
      return elementText(item, ElementKind::VideoResolution);
    case EpisodeVideoType: {
      QStringList terms;
      for (const auto& term : item.episode.elements(ElementKind::VideoTerm)) {
        terms.append(QString::fromStdString(term));
      }
      return terms.join(u' ');
    }
  }

  return {};
}

template <typename T>
bool applyFilterOperator(const T& a, const T& b, const Operator op) {
  switch (op) {
    default:
    case Operator::Equals:
      return a == b;
    case Operator::NotEquals:
      return a != b;
    case Operator::IsGreaterThan:
      return a > b;
    case Operator::IsGreaterThanOrEqualTo:
      return a >= b;
    case Operator::IsLessThan:
      return a < b;
    case Operator::IsLessThanOrEqualTo:
      return a <= b;
  }
}

bool evaluateCondition(const Condition& condition, const Item& item,
                       const episode_available_t& isEpisodeAvailable) {
  const auto element = elementValue(condition, item, isEpisodeAvailable);
  const auto value = replaceVariables(QString::fromStdString(condition.value), item);

  const auto isNumeric = [&condition, &element, &value]() {
    if (element.isEmpty() || value.isEmpty()) return false;  // see issue #639
    switch (condition.element) {
      using enum Element;
      case FileSize:
      case MetaId:
      case MetaEpisodes:
      case MetaStatus:
      case MetaType:
      case UserStatus:
      case EpisodeNumber:
      case EpisodeVersion:
      case LocalEpisodeAvailable:
        return true;
      default:
        return false;
    }
  };

  switch (condition.op) {
    case Operator::Equals:
    case Operator::NotEquals:
    case Operator::IsGreaterThan:
    case Operator::IsGreaterThanOrEqualTo:
    case Operator::IsLessThan:
    case Operator::IsLessThanOrEqualTo:
      switch (condition.element) {
        case Element::FileSize:
          return applyFilterOperator(toUint64(element), parseSizeString(value), condition.op);
        case Element::EpisodeVideoResolution:
          return applyFilterOperator(getVideoResolutionHeight(element),
                                     getVideoResolutionHeight(value), condition.op);
        default:
          break;
      }
      if (isNumeric()) {
        if (condition.op == Operator::Equals || condition.op == Operator::NotEquals) {
          if (isEqual(value, u"True"_s)) {
            return applyFilterOperator(toInt(element), 1LL, condition.op);
          }
        }
        return applyFilterOperator(toInt(element), toInt(value), condition.op);
      } else {
        if (condition.op == Operator::Equals || condition.op == Operator::NotEquals) {
          return applyFilterOperator(isEqual(element, value), true, condition.op);
        }
        return applyFilterOperator(compareStrings(element, value), 0, condition.op);
      }
    case Operator::BeginsWith:
      return startsWith(element, value);
    case Operator::EndsWith:
      return endsWith(element, value);
    case Operator::Contains:
      return inStr(element, value, true) > -1;
    case Operator::NotContains:
      return inStr(element, value, true) == -1;
  }

  return false;
}

void discard(Item& item, const Option option) {
  switch (option) {
    default:
    case Option::Default:
      item.state = ItemState::DiscardedNormal;
      break;
    case Option::Deactivate:
      item.state = ItemState::DiscardedInactive;
      break;
    case Option::Hide:
      item.state = ItemState::DiscardedHidden;
      break;
  }
}

bool applyFilter(const Filter& filter, Feed& feed, Item& item, bool recursive,
                 const episode_available_t& isEpisodeAvailable);

bool applyPreferenceFilter(const Filter& filter, Feed& feed, Item& item,
                           const episode_available_t& isEpisodeAvailable) {
  std::map<Element, bool> elementFound;

  for (const auto& condition : filter.conditions) {
    switch (condition.element) {
      case Element::MetaId:
      case Element::EpisodeTitle:
      case Element::EpisodeNumber:
      case Element::EpisodeGroup:
        elementFound[condition.element] = true;
        break;
      default:
        break;
    }
  }

  bool filterApplied = false;

  for (auto& feedItem : feed.items) {
    // Do not bother if the item was discarded before
    if (feedItem.isDiscarded()) continue;
    // Do not filter the same item again
    if (feedItem.isSameItem(item)) continue;

    // Is it the same title/anime?
    if (feedItem.episode.animeId() == anime::kUnknownId &&
        item.episode.animeId() == anime::kUnknownId) {
      if (!elementFound[Element::EpisodeTitle]) {
        if (!isEqual(elementText(feedItem, ElementKind::Title),
                     elementText(item, ElementKind::Title))) {
          continue;
        }
      }
    } else {
      if (!elementFound[Element::MetaId]) {
        if (feedItem.episode.animeId() != item.episode.animeId()) continue;
      }
    }
    // Is it the same episode?
    if (!elementFound[Element::EpisodeNumber]) {
      if (feedItem.episode.episodeNumberRange() != item.episode.episodeNumberRange()) continue;
    }
    // Is it from the same fansub group?
    if (!elementFound[Element::EpisodeGroup]) {
      if (!isEqual(elementText(feedItem, ElementKind::ReleaseGroup),
                   elementText(item, ElementKind::ReleaseGroup))) {
        continue;
      }
    }

    // Try applying the same filter
    const bool result = applyFilter(filter, feed, feedItem, false, isEpisodeAvailable);
    filterApplied = filterApplied || result;
  }

  return filterApplied;
}

bool applyFilter(const Filter& filter, Feed& feed, Item& item, bool recursive,
                 const episode_available_t& isEpisodeAvailable) {
  if (!filter.enabled) return false;

  // No need to filter if the item was discarded before
  if (item.isDiscarded()) return false;

  if (!filter.anime_ids.empty()) {
    if (!std::ranges::contains(filter.anime_ids, item.episode.animeId())) {
      return false;  // Filter doesn't apply to this item
    }
  }

  bool matched = false;

  switch (filter.match) {
    case Match::All:
      matched = true;
      for (const auto& condition : filter.conditions) {
        if (!evaluateCondition(condition, item, isEpisodeAvailable)) {
          matched = false;
          break;
        }
      }
      break;
    case Match::Any:
      matched = false;
      for (const auto& condition : filter.conditions) {
        if (evaluateCondition(condition, item, isEpisodeAvailable)) {
          matched = true;
          break;
        }
      }
      break;
  }

  switch (filter.action) {
    case Action::Discard:
      if (matched) {
        // Discard matched items, regardless of their previous state
        discard(item, filter.option);
      } else {
        return false;  // Filter doesn't apply to this item
      }
      break;

    case Action::Select:
      if (matched) {
        // Select matched items, if they were not discarded before
        item.state = ItemState::Selected;
      } else {
        return false;  // Filter doesn't apply to this item
      }
      break;

    case Action::Prefer: {
      if (recursive) {
        // Filters are strong if they're limited, weak otherwise
        const bool strongPreference = !filter.anime_ids.empty();
        if (strongPreference) {
          if (matched) {
            // Select matched items, if they were not discarded before
            item.state = ItemState::Selected;
          } else {
            // Discard mismatched items, regardless of their previous state
            discard(item, filter.option);
          }
        } else {
          if (matched) {
            if (!applyPreferenceFilter(filter, feed, item, isEpisodeAvailable)) {
              return false;  // Filter didn't have any effect
            }
          } else {
            return false;  // Filter doesn't apply to this item
          }
        }
      } else {
        // The fact that we're here means that the preference filter matched an item before, and
        // now we're checking other items for mismatches. At this point, we don't care whether the
        // preference is weak or strong.
        if (!matched) {
          // Discard mismatched items, regardless of their previous state
          discard(item, filter.option);
        } else {
          return false;  // Filter doesn't apply to this item
        }
      }
      break;
    }
  }

  item.decided_by = filter.name;
  return true;
}

////////////////////////////////////////////////////////////////////////////////
// Shortcodes (track/feed_filter_util.cpp)

constexpr std::pair<Action, std::string_view> kActionCodes[] = {
    {Action::Discard, "discard"},
    {Action::Select, "select"},
    {Action::Prefer, "prefer"},
};

constexpr std::pair<Element, std::string_view> kElementCodes[] = {
    {Element::MetaId, "meta_id"},
    {Element::MetaStatus, "meta_status"},
    {Element::MetaType, "meta_type"},
    {Element::MetaEpisodes, "meta_episodes"},
    {Element::MetaDateStart, "meta_date_start"},
    {Element::MetaDateEnd, "meta_date_end"},
    {Element::UserNotes, "user_tags"},
    {Element::UserStatus, "user_status"},
    {Element::LocalEpisodeAvailable, "local_episode_available"},
    {Element::EpisodeTitle, "episode_title"},
    {Element::EpisodeNumber, "episode_number"},
    {Element::EpisodeVersion, "episode_version"},
    {Element::EpisodeGroup, "episode_group"},
    {Element::EpisodeVideoResolution, "episode_video_resolution"},
    {Element::EpisodeVideoType, "episode_video_type"},
    {Element::FileTitle, "file_title"},
    {Element::FileCategory, "file_category"},
    {Element::FileDescription, "file_description"},
    {Element::FileLink, "file_link"},
    {Element::FileSize, "file_size"},
};

constexpr std::pair<Match, std::string_view> kMatchCodes[] = {
    {Match::All, "all"},
    {Match::Any, "any"},
};

constexpr std::pair<Operator, std::string_view> kOperatorCodes[] = {
    {Operator::Equals, "equals"},
    {Operator::NotEquals, "notequals"},
    {Operator::IsGreaterThan, "gt"},
    {Operator::IsGreaterThanOrEqualTo, "ge"},
    {Operator::IsLessThan, "lt"},
    {Operator::IsLessThanOrEqualTo, "le"},
    {Operator::BeginsWith, "beginswith"},
    {Operator::EndsWith, "endswith"},
    {Operator::Contains, "contains"},
    {Operator::NotContains, "notcontains"},
};

constexpr std::pair<Option, std::string_view> kOptionCodes[] = {
    {Option::Default, "default"},
    {Option::Deactivate, "deactivate"},
    {Option::Hide, "hide"},
};

template <typename T, size_t N>
QString toCode(const std::pair<T, std::string_view> (&codes)[N], const T value) {
  for (const auto& [key, code] : codes) {
    if (key == value) return QString::fromLatin1(code.data(), static_cast<qsizetype>(code.size()));
  }
  return {};
}

// Taiga 1.x falls back to the first value for unknown shortcodes
template <typename T, size_t N>
T fromCode(const std::pair<T, std::string_view> (&codes)[N], QStringView code) {
  for (const auto& [key, value] : codes) {
    if (code == QLatin1StringView{value.data(), static_cast<qsizetype>(value.size())}) return key;
  }
  return codes[0].first;
}

template <typename E>
std::string number(E value) {
  return std::to_string(static_cast<int>(value));
}

Filter makeFilter(Action action, Match match, Option option, std::string name,
                  std::vector<Condition> conditions = {}) {
  return {
      .name = std::move(name),
      .enabled = true,
      .action = action,
      .match = match,
      .option = option,
      .anime_ids = {},
      .conditions = std::move(conditions),
  };
}

QString animeTitle(int id) {
  const auto* item = anime::db.item(id);
  return item ? QString::fromStdString(anime::preferredTitle(*item)) : QString{};
}

////////////////////////////////////////////////////////////////////////////////
// StringCoder of Taiga 1.x (base/crypto.cpp)

constexpr QByteArrayView kMagicString = "TAI";
constexpr char kCoderVersion = 0x01;

void appendSize(QByteArray& buffer, quint16 value) {
  char bytes[2];
  qToLittleEndian(value, bytes);
  buffer.append(bytes, 2);
}

std::optional<quint16> readSize(const QByteArray& data, qsizetype pos) {
  if (pos + 2 > data.size()) return std::nullopt;
  return qFromLittleEndian<quint16>(data.constData() + pos);
}

std::optional<QString> encode(const QString& metadata, const QString& text) {
  const auto data = text.toUtf8();
  const auto meta = metadata.toUtf8();
  if (data.isEmpty() || data.size() > 0xFFFF || meta.size() > 0xFFFF) return std::nullopt;

  // `qCompress` prepends the uncompressed size (4 bytes) to a zlib stream
  const auto compressed = qCompress(data).mid(4);
  if (compressed.size() > 0xFFFF) return std::nullopt;

  QByteArray buffer;
  buffer.append(kMagicString);
  buffer.append(kCoderVersion);
  appendSize(buffer, static_cast<quint16>(meta.size()));
  buffer.append(meta);
  appendSize(buffer, static_cast<quint16>(compressed.size()));
  appendSize(buffer, static_cast<quint16>(data.size()));
  buffer.append(compressed);

  return QString::fromLatin1(buffer.toBase64());
}

std::optional<QString> decode(const QString& input, QString* metadata) {
  const auto decoded = QByteArray::fromBase64(input.simplified().remove(u' ').toLatin1());

  constexpr qsizetype kMinLength = 3 + 1 + 2 + 2 + 2;
  if (decoded.size() < kMinLength) return std::nullopt;
  if (!decoded.startsWith(kMagicString)) return std::nullopt;

  qsizetype pos = kMagicString.size();
  if (decoded.at(pos++) != kCoderVersion) return std::nullopt;

  const auto metadataSize = readSize(decoded, pos);
  if (!metadataSize) return std::nullopt;
  pos += 2;
  if (*metadataSize > 0) {
    if (metadata) *metadata = QString::fromUtf8(decoded.mid(pos, *metadataSize));
    pos += *metadataSize;
  }

  const auto dataSize = readSize(decoded, pos);
  const auto inflatedSize = readSize(decoded, pos + 2);
  if (!dataSize || !inflatedSize || *dataSize == 0 || *inflatedSize == 0) return std::nullopt;
  pos += 4;

  // `qUncompress` expects the uncompressed size first (4 bytes, big-endian)
  QByteArray compressed(4, '\0');
  qToBigEndian<quint32>(*inflatedSize, compressed.data());
  compressed.append(decoded.mid(pos, *dataSize));

  const auto inflated = qUncompress(compressed);
  if (inflated.isEmpty()) return std::nullopt;

  return QString::fromUtf8(inflated);
}

QString xmlEscape(const QString& text) {
  QString escaped = text.toHtmlEscaped();  // & < > "
  return escaped;
}

}  // namespace

////////////////////////////////////////////////////////////////////////////////
// FeedFilterManager

const std::vector<Preset>& presets() {
  using enum Element;
  using enum Operator;

  static const std::vector<Preset> presets = []() {
    std::vector<Preset> presets;

    const auto addPreset = [&presets](Action action, Match match, bool isDefault, Option option,
                                      std::string name, std::string description) {
      presets.push_back({
          .description = std::move(description),
          .filter = makeFilter(action, match, option, std::move(name)),
          .is_default = isDefault,
      });
    };
    const auto addCondition = [&presets](Element element, Operator op, std::string value) {
      presets.back().filter.conditions.push_back({element, op, std::move(value)});
    };

    /* Preset filters */

    // Custom
    addPreset(Action::Discard, Match::All, false, Option::Default, "(Custom)",
              "Lets you create a custom filter from scratch");

    // Fansub group
    addPreset(Action::Prefer, Match::All, false, Option::Default, "[Fansub] Anime",
              "Lets you choose a fansub group for one or more anime");
    addCondition(EpisodeGroup, Equals, "TaigaSubs (change this)");

    // Discard bad video keywords
    addPreset(Action::Discard, Match::Any, false, Option::Default, "Discard bad video keywords",
              "Discards everything that is AVI, DIVX, LQ, RMVB, SD, WMV or XVID");
    addCondition(EpisodeVideoType, Contains, "AVI");
    addCondition(EpisodeVideoType, Contains, "DIVX");
    addCondition(EpisodeVideoType, Contains, "LQ");
    addCondition(EpisodeVideoType, Contains, "RMVB");
    addCondition(EpisodeVideoType, Contains, "SD");
    addCondition(EpisodeVideoType, Contains, "WMV");
    addCondition(EpisodeVideoType, Contains, "XVID");

    // Prefer new versions
    addPreset(Action::Prefer, Match::Any, false, Option::Default, "Prefer new versions",
              "Prefers v2 files and above when there are earlier releases of the same episode as "
              "well");
    addCondition(EpisodeVersion, IsGreaterThan, "1");

    /* Default filters */

    // Select currently watching
    addPreset(Action::Select, Match::Any, true, Option::Default, "Select currently watching",
              "Selects files that belong to anime that you're currently watching");
    addCondition(UserStatus, Equals, number(anime::list::Status::Watching));

    // Select airing anime in plan to watch
    addPreset(Action::Select, Match::All, true, Option::Default,
              "Select airing anime in plan to watch",
              "Selects files that belong to an airing anime that you're planning to watch");
    addCondition(MetaStatus, Equals, number(anime::Status::Airing));
    addCondition(UserStatus, Equals, number(anime::list::Status::PlanToWatch));

    // Discard dropped
    addPreset(Action::Discard, Match::All, true, Option::Default, "Discard dropped",
              "Discards files that belong to anime that you've dropped watching");
    addCondition(UserStatus, Equals, number(anime::list::Status::Dropped));

    // Discard and deactivate not-in-list anime
    addPreset(Action::Discard, Match::Any, true, Option::Deactivate,
              "Discard and deactivate not-in-list anime",
              "Discards files that do not belong to any anime in your list");
    addCondition(UserStatus, Equals, number(anime::list::Status::NotInList));

    // Discard watched and available episodes
    addPreset(Action::Discard, Match::Any, true, Option::Default,
              "Discard watched and available episodes",
              "Discards episodes you've already watched or downloaded");
    addCondition(EpisodeNumber, IsLessThanOrEqualTo, "%watched%");
    addCondition(LocalEpisodeAvailable, Equals, "True");

    // Prefer high-resolution files
    addPreset(Action::Prefer, Match::Any, true, Option::Default, "Prefer high-resolution files",
              "Prefers 1080p files when there are other files of the same episode as well");
    addCondition(EpisodeVideoResolution, Equals, "1080p");

    return presets;
  }();

  return presets;
}

std::vector<Filter> defaultFilters() {
  std::vector<Filter> filters;
  for (const auto& preset : presets()) {
    if (preset.is_default) filters.push_back(preset.filter);
  }
  return filters;
}

void applyFilters(Feed& feed, const std::vector<Filter>& filters,
                  const episode_available_t& isEpisodeAvailable) {
  for (const bool preferences : {false, true}) {
    for (auto& item : feed.items) {
      for (const auto& filter : filters) {
        if (preferences != (filter.action == Action::Prefer)) continue;
        applyFilter(filter, feed, item, true, isEpisodeAvailable);
      }
    }
  }
}

void markNewEpisodes(Feed& feed) {
  for (auto& item : feed.items) {
    if (!anime::db.item(item.episode.animeId())) continue;
    const auto* entry = anime::db.entry(item.episode.animeId());
    const int watched = anime::list::isInList(entry) ? entry->watched_episodes : 0;
    if (episodeHigh(item) > watched) item.new_episode = true;
  }
}

bool getFansubFilter(const std::vector<Filter>& filters, int animeId,
                     std::vector<std::string>& groups) {
  bool found = false;
  for (const auto& filter : filters) {
    if (!std::ranges::contains(filter.anime_ids, animeId)) continue;
    for (const auto& condition : filter.conditions) {
      if (condition.element == Element::EpisodeGroup) {
        groups.push_back(condition.value);
        found = true;
      }
    }
  }
  return found;
}

bool setFansubFilter(std::vector<Filter>& filters, int animeId, const std::string& group,
                     const std::string& videoResolution) {
  const auto findCondition = [](Filter& filter, const Element element) {
    return std::ranges::find(filter.conditions, element, &Condition::element);
  };

  // Check existing filters
  for (auto it = filters.begin(); it != filters.end(); ++it) {
    auto& filter = *it;
    const auto id = std::ranges::find(filter.anime_ids, animeId);
    if (id == filter.anime_ids.end()) continue;
    const auto groupCondition = findCondition(filter, Element::EpisodeGroup);
    if (groupCondition == filter.conditions.end()) continue;
    const auto resolutionCondition = findCondition(filter, Element::EpisodeVideoResolution);

    if (filter.anime_ids.size() > 1) {
      filter.anime_ids.erase(id);
      if (group.empty()) return true;
      break;
    }

    if (group.empty()) {
      filters.erase(it);
    } else {
      groupCondition->value = group;
      if (!videoResolution.empty()) {
        if (resolutionCondition != filter.conditions.end()) {
          resolutionCondition->value = videoResolution;
        } else {
          filter.conditions.push_back(
              {Element::EpisodeVideoResolution, Operator::Equals, videoResolution});
        }
      }
    }
    return true;
  }

  if (group.empty()) return false;

  const auto* item = anime::db.item(animeId);
  if (!item) return false;

  // Create new filter
  auto filter = makeFilter(Action::Prefer, Match::All, Option::Default,
                           "[Fansub] " + anime::preferredTitle(*item),
                           {{Element::EpisodeGroup, Operator::Equals, group}});
  if (!videoResolution.empty()) {
    filter.conditions.push_back(
        {Element::EpisodeVideoResolution, Operator::Equals, videoResolution});
  }
  filter.anime_ids.push_back(animeId);
  filters.push_back(std::move(filter));
  return true;
}

bool addDiscardFilter(std::vector<Filter>& filters, int animeId) {
  const auto* item = anime::db.item(animeId);
  if (!item) return false;

  filters.push_back(makeFilter(
      Action::Discard, Match::All, Option::Default,
      u"Discard \"%1\""_s.arg(QString::fromStdString(anime::preferredTitle(*item))).toStdString(),
      {{Element::MetaId, Operator::Equals, std::to_string(item->id)}}));
  return true;
}

////////////////////////////////////////////////////////////////////////////////
// Import and export

QString exportFilters(const std::vector<Filter>& filters) {
  const auto metadata = QString::fromStdString(taiga::version().to_string());
  return encode(metadata, filtersToXml(filters)).value_or(QString{});
}

std::optional<std::vector<Filter>> importFilters(const QString& text) {
  const auto xml = decode(text, nullptr);
  if (!xml) return std::nullopt;
  return filtersFromXml(*xml);
}

QString filtersToXml(const std::vector<Filter>& filters) {
  // Same layout as pugixml's output in Taiga 1.x
  QString xml;
  for (const auto& filter : filters) {
    xml += u"<item action=\"%1\" match=\"%2\" option=\"%3\" enabled=\"%4\" name=\"%5\">\n"_s
               .arg(toCode(kActionCodes, filter.action), toCode(kMatchCodes, filter.match),
                    toCode(kOptionCodes, filter.option),
                    filter.enabled ? u"true"_s : u"false"_s,
                    xmlEscape(QString::fromStdString(filter.name)));
    for (const auto id : filter.anime_ids) {
      xml += u"\t<anime id=\"%1\" />\n"_s.arg(id);
    }
    for (const auto& condition : filter.conditions) {
      xml += u"\t<condition element=\"%1\" operator=\"%2\" value=\"%3\" />\n"_s.arg(
          toCode(kElementCodes, condition.element), toCode(kOperatorCodes, condition.op),
          xmlEscape(QString::fromStdString(condition.value)));
    }
    xml += u"</item>\n"_s;
  }
  return xml;
}

std::optional<std::vector<Filter>> filtersFromXml(const QString& xml) {
  // The exported text is a list of items without a root element.
  QXmlStreamReader reader{u"<filters>"_s + xml + u"</filters>"_s};
  std::vector<Filter> filters;

  while (!reader.atEnd()) {
    reader.readNext();
    if (!reader.isStartElement()) continue;

    const auto attributes = reader.attributes();

    if (reader.name() == u"item") {
      const auto enabled = attributes.value(u"enabled");
      filters.push_back(Filter{
          .name = attributes.value(u"name").toString().toStdString(),
          .enabled = enabled == u"true" || enabled == u"1",
          .action = fromCode(kActionCodes, attributes.value(u"action")),
          .match = fromCode(kMatchCodes, attributes.value(u"match")),
          .option = fromCode(kOptionCodes, attributes.value(u"option")),
          .anime_ids = {},
          .conditions = {},
      });
    } else if (reader.name() == u"anime" && !filters.empty()) {
      filters.back().anime_ids.push_back(attributes.value(u"id").toInt());
    } else if (reader.name() == u"condition" && !filters.empty()) {
      filters.back().conditions.push_back({
          .element = fromCode(kElementCodes, attributes.value(u"element")),
          .op = fromCode(kOperatorCodes, attributes.value(u"operator")),
          .value = attributes.value(u"value").toString().toStdString(),
      });
    }
  }

  if (reader.hasError()) return std::nullopt;
  return filters;
}

QJsonArray toJson(const std::vector<Filter>& filters) {
  QJsonArray array;

  for (const auto& filter : filters) {
    QJsonArray animeIds;
    for (const auto id : filter.anime_ids) animeIds.append(id);

    QJsonArray conditions;
    for (const auto& condition : filter.conditions) {
      conditions.append(QJsonObject{
          {"element", toCode(kElementCodes, condition.element)},
          {"operator", toCode(kOperatorCodes, condition.op)},
          {"value", QString::fromStdString(condition.value)},
      });
    }

    array.append(QJsonObject{
        {"name", QString::fromStdString(filter.name)},
        {"enabled", filter.enabled},
        {"action", toCode(kActionCodes, filter.action)},
        {"match", toCode(kMatchCodes, filter.match)},
        {"option", toCode(kOptionCodes, filter.option)},
        {"anime", animeIds},
        {"conditions", conditions},
    });
  }

  return array;
}

std::vector<Filter> filtersFromJson(const QJsonArray& array) {
  std::vector<Filter> filters;

  for (const QJsonValue& value : array) {
    const auto object = value.toObject();

    Filter filter{
        .name = object.value("name").toString().toStdString(),
        .enabled = object.value("enabled").toBool(true),
        .action = fromCode(kActionCodes, object.value("action").toString()),
        .match = fromCode(kMatchCodes, object.value("match").toString()),
        .option = fromCode(kOptionCodes, object.value("option").toString()),
        .anime_ids = {},
        .conditions = {},
    };

    for (const QJsonValue& id : object.value("anime").toArray()) {
      filter.anime_ids.push_back(id.toInt());
    }

    for (const QJsonValue& item : object.value("conditions").toArray()) {
      const auto condition = item.toObject();
      filter.conditions.push_back({
          .element = fromCode(kElementCodes, condition.value("element").toString()),
          .op = fromCode(kOperatorCodes, condition.value("operator").toString()),
          .value = condition.value("value").toString().toStdString(),
      });
    }

    filters.push_back(std::move(filter));
  }

  return filters;
}

////////////////////////////////////////////////////////////////////////////////
// Translations

QString createNameFromConditions(const Filter&) {
  // @TODO (as in Taiga 1.x)
  return u"New Filter"_s;
}

QString translateCondition(const Condition& condition) {
  return u"%1 %2 \"%3\""_s.arg(translateElement(condition.element),
                               translateOperator(condition.op), translateValue(condition));
}

QString translateConditions(const Filter& filter, size_t index) {
  QString str;
  const size_t maxIndex = filter.match == Match::All ? filter.conditions.size() : index + 1;
  for (size_t i = index; i < maxIndex && i < filter.conditions.size(); ++i) {
    if (i > index) str += u" & "_s;
    str += translateCondition(filter.conditions[i]);
  }
  return str;
}

QString translateElement(Element element) {
  switch (element) {
    using enum Element;
    case FileTitle: return u"File name"_s;
    case FileCategory: return u"File category"_s;
    case FileDescription: return u"File description"_s;
    case FileLink: return u"File link"_s;
    case FileSize: return u"File size"_s;
    case MetaId: return u"Anime ID"_s;
    case EpisodeTitle: return u"Episode title"_s;
    case MetaDateStart: return u"Anime date started"_s;
    case MetaDateEnd: return u"Anime date ended"_s;
    case MetaEpisodes: return u"Anime episode count"_s;
    case MetaStatus: return u"Anime airing status"_s;
    case MetaType: return u"Anime type"_s;
    case UserNotes: return u"Anime notes"_s;
    case UserStatus: return u"Anime watching status"_s;
    case EpisodeNumber: return u"Episode number"_s;
    case EpisodeVersion: return u"Episode version"_s;
    case LocalEpisodeAvailable: return u"Episode availability"_s;
    case EpisodeGroup: return u"Episode fansub group"_s;
    case EpisodeVideoResolution: return u"Episode video resolution"_s;
    case EpisodeVideoType: return u"Episode video type"_s;
  }
  return u"?"_s;
}

QString translateOperator(Operator op) {
  switch (op) {
    using enum Operator;
    case Equals: return u"is"_s;
    case NotEquals: return u"is not"_s;
    case IsGreaterThan: return u"is greater than"_s;
    case IsGreaterThanOrEqualTo: return u"is greater than or equal to"_s;
    case IsLessThan: return u"is less than"_s;
    case IsLessThanOrEqualTo: return u"is less than or equal to"_s;
    case BeginsWith: return u"begins with"_s;
    case EndsWith: return u"ends with"_s;
    case Contains: return u"contains"_s;
    case NotContains: return u"does not contain"_s;
  }
  return u"?"_s;
}

QString translateValue(const Condition& condition) {
  const auto value = QString::fromStdString(condition.value);

  switch (condition.element) {
    case Element::MetaId: {
      if (value.isEmpty()) return u"(?)"_s;
      const auto title = animeTitle(static_cast<int>(toInt(value)));
      return title.isEmpty() ? value + u" (?)"_s : value + u" (" + title + u")";
    }
    case Element::UserStatus:
      return translateListStatus(static_cast<int>(toInt(value)));
    case Element::MetaStatus:
      return translateAiringStatus(static_cast<int>(toInt(value)));
    case Element::MetaType:
      return translateType(static_cast<int>(toInt(value)));
    default:
      return !value.isEmpty() ? value : u"(empty)"_s;
  }
}

QString translateMatching(Match match) {
  switch (match) {
    case Match::All: return u"All conditions"_s;
    case Match::Any: return u"Any condition"_s;
  }
  return u"?"_s;
}

QString translateAction(Action action) {
  switch (action) {
    case Action::Discard: return u"Discard matched items"_s;
    case Action::Select: return u"Select matched items"_s;
    case Action::Prefer: return u"Prefer matched items to similar ones"_s;
  }
  return u"?"_s;
}

QString translateOption(Option option) {
  switch (option) {
    case Option::Default: return u"Default"_s;
    case Option::Deactivate: return u"Deactivate discarded items"_s;
    case Option::Hide: return u"Hide discarded items"_s;
  }
  return u"?"_s;
}

QString translateListStatus(int status) {
  switch (static_cast<anime::list::Status>(status)) {
    using enum anime::list::Status;
    case NotInList: return u"Not in list"_s;
    case Watching: return u"Currently watching"_s;
    case Completed: return u"Completed"_s;
    case OnHold: return u"On hold"_s;
    case Dropped: return u"Dropped"_s;
    case PlanToWatch: return u"Plan to watch"_s;
  }
  return {};
}

QString translateAiringStatus(int status) {
  switch (static_cast<anime::Status>(status)) {
    using enum anime::Status;
    case Unknown: return u"Unknown"_s;
    case Airing: return u"Currently airing"_s;
    case FinishedAiring: return u"Finished airing"_s;
    case NotYetAired: return u"Not yet aired"_s;
  }
  return {};
}

QString translateType(int type) {
  switch (static_cast<anime::Type>(type)) {
    using enum anime::Type;
    case Unknown: return u"Unknown"_s;
    case Tv: return u"TV"_s;
    case Ova: return u"OVA"_s;
    case Movie: return u"Movie"_s;
    case Special: return u"Special"_s;
    case Ona: return u"ONA"_s;
    case Music: return u"Music"_s;
  }
  return {};
}

}  // namespace track::feed
