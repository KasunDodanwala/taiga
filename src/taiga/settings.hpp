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

#include <QJsonArray>
#include <QNetworkProxy>
#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include "base/settings.hpp"
#include "media/anime.hpp"
#include "track/update_trigger.hpp"

namespace taiga {

class Settings final : public base::Settings {
public:
  static constexpr QLatin1StringView kAppStyleSystem{"system"};
  static constexpr std::chrono::seconds kUpdateDelayMin{10};
  static constexpr std::chrono::seconds kUpdateDelayMax{3600};

  void init() const;

  Qt::ColorScheme appColorScheme() const;
  std::string appStyle() const;
  bool detectionEnabled() const;
  std::vector<std::string> disabledMediaPlayers() const;
  std::string service() const;
  std::vector<std::string> libraryFolders() const;
  std::chrono::milliseconds mediaDetectionInterval() const;
  QNetworkProxy::ProxyType proxyType() const;
  std::string proxyHost() const;
  int proxyPort() const;
  std::string proxyUsername() const;
  std::string proxyPassword() const;
  bool streamingMediaEnabled() const;
  bool syncEnabled() const;
  anime::TitleLanguage titleLanguage() const;
  std::chrono::seconds updateDelay() const;
  bool updateLibraryOnly() const;
  bool updatePauseWhenUnfocused() const;
  track::UpdateTrigger updateTrigger() const;

  bool appCloseToTray() const;

  bool kodiEnabled() const;
  bool kodiDetection() const;
  std::string kodiHost() const;
  int kodiPort() const;
  std::string kodiUsername() const;
  std::string kodiPassword() const;
  bool kodiPlayEpisodes() const;

  std::string torrentFeedUrl() const;
  std::string torrentSearchUrl() const;
  bool torrentAutoCheckEnabled() const;
  std::chrono::minutes torrentAutoCheckInterval() const;
  std::string torrentNewAction() const;  // "notify", "download" or "none"
  std::string torrentClientUrl() const;
  std::string torrentClientUsername() const;
  std::string torrentClientPassword() const;
  std::string torrentDownloadPath() const;
  bool torrentCreateSubfolder() const;
  std::string torrentCategory() const;
  bool torrentUseMagnet() const;
  bool torrentFilterEnabled() const;
  std::optional<QJsonArray> torrentFilters() const;

  bool mediaServerEnabled() const;
  std::string mediaServerName() const;
  int mediaServerPort() const;
  std::string mediaServerTranscoding() const;  // "none", "software" or "hardware"
  std::string mediaServerVaapiDevice() const;
  int mediaServerBitrate() const;  // kbit/s, for transcoding
  bool mediaServerBurnSubtitles() const;
  bool mediaServerVirtualFolders() const;
  bool mediaServerLocalOnly() const;
  bool mediaServerDetection() const;
  std::string mediaServerUuid() const;

  bool discordEnabled() const;
  std::string discordApplicationId() const;
  bool discordShowGroup() const;
  bool discordShowTime() const;
  bool discordShowUsername() const;
  bool discordShowButton() const;

  void setAppColorScheme(const Qt::ColorScheme scheme) const;
  void setAppStyle(const std::string& style) const;
  void setDetectionEnabled(const bool enabled) const;
  void setDisabledMediaPlayers(std::vector<std::string> players) const;
  void setService(const std::string& service) const;
  void setLibraryFolders(std::vector<std::string> folders) const;
  void setMediaDetectionInterval(const std::chrono::milliseconds interval) const;
  void setProxyType(const QNetworkProxy::ProxyType type) const;
  void setProxyHost(const std::string& host) const;
  void setProxyPort(const int port) const;
  void setProxyUsername(const std::string& username) const;
  void setProxyPassword(const std::string& password) const;
  void setStreamingMediaEnabled(const bool enabled) const;
  void setSyncEnabled(const bool enabled) const;
  void setTitleLanguage(const anime::TitleLanguage language) const;
  void setUpdateDelay(const std::chrono::seconds delay) const;
  void setUpdateLibraryOnly(const bool enabled) const;
  void setUpdatePauseWhenUnfocused(const bool enabled) const;
  void setUpdateTrigger(const track::UpdateTrigger trigger) const;

  void setAppCloseToTray(const bool enabled) const;

  void setKodiEnabled(const bool enabled) const;
  void setKodiDetection(const bool enabled) const;
  void setKodiHost(const std::string& host) const;
  void setKodiPort(const int port) const;
  void setKodiUsername(const std::string& username) const;
  void setKodiPassword(const std::string& password) const;
  void setKodiPlayEpisodes(const bool enabled) const;

  void setTorrentFeedUrl(const std::string& url) const;
  void setTorrentSearchUrl(const std::string& url) const;
  void setTorrentAutoCheckEnabled(const bool enabled) const;
  void setTorrentAutoCheckInterval(const std::chrono::minutes interval) const;
  void setTorrentNewAction(const std::string& action) const;
  void setTorrentClientUrl(const std::string& url) const;
  void setTorrentClientUsername(const std::string& username) const;
  void setTorrentClientPassword(const std::string& password) const;
  void setTorrentDownloadPath(const std::string& path) const;
  void setTorrentCreateSubfolder(const bool enabled) const;
  void setTorrentCategory(const std::string& category) const;
  void setTorrentUseMagnet(const bool enabled) const;
  void setTorrentFilterEnabled(const bool enabled) const;
  void setTorrentFilters(const QJsonArray& filters) const;

  void setMediaServerEnabled(const bool enabled) const;
  void setMediaServerName(const std::string& name) const;
  void setMediaServerPort(const int port) const;
  void setMediaServerTranscoding(const std::string& mode) const;
  void setMediaServerVaapiDevice(const std::string& device) const;
  void setMediaServerBitrate(const int bitrate) const;
  void setMediaServerBurnSubtitles(const bool enabled) const;
  void setMediaServerVirtualFolders(const bool enabled) const;
  void setMediaServerLocalOnly(const bool enabled) const;
  void setMediaServerDetection(const bool enabled) const;

  void setDiscordEnabled(const bool enabled) const;
  void setDiscordApplicationId(const std::string& id) const;
  void setDiscordShowGroup(const bool enabled) const;
  void setDiscordShowTime(const bool enabled) const;
  void setDiscordShowUsername(const bool enabled) const;
  void setDiscordShowButton(const bool enabled) const;

private:
  QString fileName() const override;

  mutable std::optional<anime::TitleLanguage> titleLanguageCache_;
};

inline Settings settings;

}  // namespace taiga
