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

// Torrent filters, ported from Taiga 1.x (track/feed_filter*, track/feed_filter_manager*). The
// elements, operators, presets, evaluation rules and the import/export format are the same, so that
// filters behave identically and can be shared between both versions.

#include <QJsonArray>
#include <QString>
#include <array>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace track::feed {

struct Feed;
struct Item;

enum class Element {
  MetaId,
  MetaStatus,
  MetaType,
  MetaEpisodes,
  MetaDateStart,
  MetaDateEnd,
  UserNotes,
  UserStatus,
  LocalEpisodeAvailable,
  EpisodeTitle,
  EpisodeNumber,
  EpisodeVersion,
  EpisodeGroup,
  EpisodeVideoResolution,
  EpisodeVideoType,
  FileTitle,
  FileCategory,
  FileDescription,
  FileLink,
  FileSize,
};

enum class Operator {
  Equals,
  NotEquals,
  IsGreaterThan,
  IsGreaterThanOrEqualTo,
  IsLessThan,
  IsLessThanOrEqualTo,
  BeginsWith,
  EndsWith,
  Contains,
  NotContains,
};

enum class Match {
  All,
  Any,
};

enum class Action {
  Discard,
  Select,
  Prefer,
};

enum class Option {
  Default,
  Deactivate,
  Hide,
};

constexpr std::array kElements{
    Element::MetaId,          Element::MetaStatus,
    Element::MetaType,        Element::MetaEpisodes,
    Element::MetaDateStart,   Element::MetaDateEnd,
    Element::UserNotes,       Element::UserStatus,
    Element::LocalEpisodeAvailable, Element::EpisodeTitle,
    Element::EpisodeNumber,   Element::EpisodeVersion,
    Element::EpisodeGroup,    Element::EpisodeVideoResolution,
    Element::EpisodeVideoType, Element::FileTitle,
    Element::FileCategory,    Element::FileDescription,
    Element::FileLink,        Element::FileSize,
};

constexpr std::array kOperators{
    Operator::Equals,     Operator::NotEquals,  Operator::IsGreaterThan,
    Operator::IsGreaterThanOrEqualTo, Operator::IsLessThan, Operator::IsLessThanOrEqualTo,
    Operator::BeginsWith, Operator::EndsWith,   Operator::Contains,
    Operator::NotContains,
};

struct Condition {
  Element element = Element::FileTitle;
  Operator op = Operator::Equals;
  std::string value;
};

struct Filter {
  std::string name;
  bool enabled = true;
  Action action = Action::Discard;
  Match match = Match::All;
  Option option = Option::Default;
  std::vector<int> anime_ids;  // limits the filter to these anime, if not empty
  std::vector<Condition> conditions;
};

struct Preset {
  std::string description;
  Filter filter;
  bool is_default = false;
};

// Tells whether an episode exists in library folders.
using episode_available_t = std::function<bool(int anime_id, int episode_number)>;

////////////////////////////////////////////////////////////////////////////////
// FeedFilterManager

const std::vector<Preset>& presets();
std::vector<Filter> defaultFilters();  // AddPresets

// Filter(feed, false) and Filter(feed, true): preferences have lower priority, so they are applied
// after other filters, in order to avoid discarding items that we actually want.
void applyFilters(Feed& feed, const std::vector<Filter>& filters,
                  const episode_available_t& isEpisodeAvailable);
void markNewEpisodes(Feed& feed);

bool getFansubFilter(const std::vector<Filter>& filters, int animeId,
                     std::vector<std::string>& groups);
bool setFansubFilter(std::vector<Filter>& filters, int animeId, const std::string& group,
                     const std::string& videoResolution);
bool addDiscardFilter(std::vector<Filter>& filters, int animeId);

////////////////////////////////////////////////////////////////////////////////
// Import and export

// The encoded text that Taiga 1.x shows in "Export filters" and accepts in "Import filters"
QString exportFilters(const std::vector<Filter>& filters);
std::optional<std::vector<Filter>> importFilters(const QString& text);

// Taiga 1.x's XML representation (items with shortcodes)
QString filtersToXml(const std::vector<Filter>& filters);
std::optional<std::vector<Filter>> filtersFromXml(const QString& xml);

// Storage in settings (same shortcodes, as JSON)
QJsonArray toJson(const std::vector<Filter>& filters);
std::vector<Filter> filtersFromJson(const QJsonArray& array);

////////////////////////////////////////////////////////////////////////////////
// Translations (feed_filter_util in Taiga 1.x)

QString createNameFromConditions(const Filter& filter);
QString translateCondition(const Condition& condition);
QString translateConditions(const Filter& filter, size_t index = 0);
QString translateElement(Element element);
QString translateOperator(Operator op);
QString translateValue(const Condition& condition);
QString translateMatching(Match match);
QString translateAction(Action action);
QString translateOption(Option option);

// Taiga 1.x's UI strings for statuses and types, used for values of conditions
QString translateListStatus(int status);
QString translateAiringStatus(int status);
QString translateType(int type);

}  // namespace track::feed
