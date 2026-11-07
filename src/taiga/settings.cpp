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

#include "settings.hpp"

#include <QFile>
#include <QJsonArray>
#include <QSysInfo>
#include <QUuid>
#include <algorithm>
#include <ranges>

#include "base/string.hpp"
#include "compat/settings.hpp"
#include "sync/service.hpp"
#include "taiga/accounts.hpp"
#include "taiga/path.hpp"
#include "taiga/version.hpp"

namespace taiga {

void Settings::init() const {
  const auto appVersion = taiga::version().to_string();

  // v1 to v2
  if (!QFile::exists(fileName())) {
    compat::v1::readSettings(std::format("{}/v1/settings.xml", get_data_path()), *this, accounts);
    setValue("meta.version", appVersion);
    return;
  }

  // v2.x
  const auto fileVersion = value("meta.version").toString().toStdString();
  if (fileVersion != appVersion) {
    setValue("meta.version", appVersion);
  }
}

QString Settings::fileName() const {
  return u"%1/settings.json"_s.arg(get_data_path());
}

////////////////////////////////////////////////////////////////////////////////

Qt::ColorScheme Settings::appColorScheme() const {
  return value("app.colorScheme", static_cast<int>(Qt::ColorScheme::Unknown))
      .value<Qt::ColorScheme>();
}

std::string Settings::appStyle() const {
#ifdef Q_OS_WINDOWS
  // Fusion is more consistent than the Windows 11 style.
  const auto defaultStyle = u"fusion"_s;
#else
  const auto defaultStyle = QString{kAppStyleSystem};
#endif
  return value("app.style", defaultStyle).toString().toStdString();
}

bool Settings::detectionEnabled() const {
  return value("track.detection.enabled", true).toBool();
}

std::vector<std::string> Settings::disabledMediaPlayers() const {
  return value("recognition.mediaPlayers.disabled").toJsonArray().toVariantList() |
         std::views::transform([](const QVariant& v) { return v.toString().toStdString(); }) |
         std::ranges::to<std::vector>();
}

std::string Settings::service() const {
  return value("sync.service", sync::serviceSlug(sync::ServiceId::AniList))
      .toString()
      .toStdString();
}

std::vector<std::string> Settings::libraryFolders() const {
  return value("library.folders").toJsonArray().toVariantList() |
         std::views::transform([](const QVariant& v) { return v.toString().toStdString(); }) |
         std::ranges::to<std::vector>();
}

std::chrono::milliseconds Settings::mediaDetectionInterval() const {
  const auto interval = value("track.detection.interval", 3000).toInt();
  return std::chrono::milliseconds{interval};
}

QNetworkProxy::ProxyType Settings::proxyType() const {
  const auto type = value("network.proxy.type", u"http"_s).toString();
  if (type == u"socks5") return QNetworkProxy::Socks5Proxy;
  return QNetworkProxy::HttpProxy;
}

std::string Settings::proxyHost() const {
  return value("network.proxy.host").toString().toStdString();
}

int Settings::proxyPort() const {
  return value("network.proxy.port", -1).toInt();
}

std::string Settings::proxyUsername() const {
  return value("network.proxy.username").toString().toStdString();
}

std::string Settings::proxyPassword() const {
  return value("network.proxy.password").toString().toStdString();
}

bool Settings::streamingMediaEnabled() const {
  return value("recognition.streaming.enabled", false).toBool();
}

bool Settings::syncEnabled() const {
  return value("sync.enabled", true).toBool();
}

anime::TitleLanguage Settings::titleLanguage() const {
  if (!titleLanguageCache_) {
    const auto language = value("library.titleLanguage", u"romaji"_s).toString();
    if (language == u"english") {
      titleLanguageCache_ = anime::TitleLanguage::English;
    } else if (language == u"native") {
      titleLanguageCache_ = anime::TitleLanguage::Native;
    } else {
      titleLanguageCache_ = anime::TitleLanguage::Romaji;
    }
  }
  return *titleLanguageCache_;
}

std::chrono::seconds Settings::updateDelay() const {
  const std::chrono::seconds delay{value("track.update.delay", 120).toInt()};
  return std::clamp(delay, kUpdateDelayMin, kUpdateDelayMax);
}

bool Settings::updateLibraryOnly() const {
  return value("track.update.libraryOnly", false).toBool();
}

bool Settings::updatePauseWhenUnfocused() const {
  return value("track.update.pauseWhenUnfocused", false).toBool();
}

track::UpdateTrigger Settings::updateTrigger() const {
  const auto trigger = value("track.update.trigger", u"afterDelay"_s).toString();
  if (trigger == u"onPlayerClose") return track::UpdateTrigger::OnPlayerClose;
  return track::UpdateTrigger::AfterDelay;
}

////////////////////////////////////////////////////////////////////////////////

void Settings::setAppColorScheme(const Qt::ColorScheme scheme) const {
  setValue("app.colorScheme", static_cast<int>(scheme));
}

void Settings::setAppStyle(const std::string& style) const {
  setValue("app.style", style);
}

void Settings::setDetectionEnabled(const bool enabled) const {
  setValue("track.detection.enabled", enabled);
}

void Settings::setDisabledMediaPlayers(std::vector<std::string> players) const {
  const auto list =
      players |
      std::views::transform([](const std::string& s) { return QString::fromStdString(s); }) |
      std::ranges::to<QList>();
  setValue("recognition.mediaPlayers.disabled", QJsonArray::fromStringList(list));
}

void Settings::setService(const std::string& service) const {
  setValue("sync.service", service);
}

void Settings::setLibraryFolders(std::vector<std::string> folders) const {
  const auto list =
      folders |
      std::views::transform([](const std::string& s) { return QString::fromStdString(s); }) |
      std::ranges::to<QList>();
  setValue("library.folders", QJsonArray::fromStringList(list));
}

void Settings::setMediaDetectionInterval(const std::chrono::milliseconds interval) const {
  setValue("track.detection.interval", static_cast<qint64>(interval.count()));
}

void Settings::setProxyType(const QNetworkProxy::ProxyType type) const {
  const std::string slug = type == QNetworkProxy::Socks5Proxy ? "socks5" : "http";
  setValue("network.proxy.type", slug);
}

void Settings::setProxyHost(const std::string& host) const {
  setValue("network.proxy.host", host);
}

void Settings::setProxyPort(const int port) const {
  setValue("network.proxy.port", port);
}

void Settings::setProxyUsername(const std::string& username) const {
  setValue("network.proxy.username", username);
}

void Settings::setProxyPassword(const std::string& password) const {
  setValue("network.proxy.password", password);
}

void Settings::setStreamingMediaEnabled(const bool enabled) const {
  setValue("recognition.streaming.enabled", enabled);
}

void Settings::setSyncEnabled(const bool enabled) const {
  setValue("sync.enabled", enabled);
}

void Settings::setTitleLanguage(const anime::TitleLanguage language) const {
  const auto slug = [language]() -> std::string {
    switch (language) {
      default:
      case anime::TitleLanguage::Romaji:
        return "romaji";
      case anime::TitleLanguage::English:
        return "english";
      case anime::TitleLanguage::Native:
        return "native";
    }
  }();
  setValue("library.titleLanguage", slug);
  titleLanguageCache_ = language;
}

void Settings::setUpdateDelay(const std::chrono::seconds delay) const {
  setValue("track.update.delay", static_cast<int>(delay.count()));
}

void Settings::setUpdateLibraryOnly(const bool enabled) const {
  setValue("track.update.libraryOnly", enabled);
}

void Settings::setUpdatePauseWhenUnfocused(const bool enabled) const {
  setValue("track.update.pauseWhenUnfocused", enabled);
}

void Settings::setUpdateTrigger(const track::UpdateTrigger trigger) const {
  const std::string slug =
      trigger == track::UpdateTrigger::OnPlayerClose ? "onPlayerClose" : "afterDelay";
  setValue("track.update.trigger", slug);
}

////////////////////////////////////////////////////////////////////////////////
// Application

bool Settings::appCloseToTray() const {
#ifdef Q_OS_WINDOWS
  constexpr bool defaultValue = false;
#else
  // On Linux, Taiga is typically left running in the background to detect media (e.g. on a media
  // server), so closing the window should not stop detection.
  constexpr bool defaultValue = true;
#endif
  return value("app.closeToTray", defaultValue).toBool();
}

void Settings::setAppCloseToTray(const bool enabled) const {
  setValue("app.closeToTray", enabled);
}

////////////////////////////////////////////////////////////////////////////////
// Kodi

bool Settings::kodiEnabled() const {
  return value("track.kodi.enabled", true).toBool();
}

bool Settings::kodiDetection() const {
  // Off by default, so that Kodi is detected via the files it has open, like in Taiga 1.x
  return value("track.kodi.detection", false).toBool();
}

std::string Settings::kodiHost() const {
  return value("track.kodi.host", u"127.0.0.1"_s).toString().toStdString();
}

int Settings::kodiPort() const {
  return value("track.kodi.port", 8080).toInt();
}

std::string Settings::kodiUsername() const {
  return value("track.kodi.username", u"kodi"_s).toString().toStdString();
}

std::string Settings::kodiPassword() const {
  return value("track.kodi.password").toString().toStdString();
}

bool Settings::kodiPlayEpisodes() const {
  return value("track.kodi.playEpisodes", false).toBool();
}

void Settings::setKodiEnabled(const bool enabled) const {
  setValue("track.kodi.enabled", enabled);
}

void Settings::setKodiDetection(const bool enabled) const {
  setValue("track.kodi.detection", enabled);
}

void Settings::setKodiHost(const std::string& host) const {
  setValue("track.kodi.host", host);
}

void Settings::setKodiPort(const int port) const {
  setValue("track.kodi.port", port);
}

void Settings::setKodiUsername(const std::string& username) const {
  setValue("track.kodi.username", username);
}

void Settings::setKodiPassword(const std::string& password) const {
  setValue("track.kodi.password", password);
}

void Settings::setKodiPlayEpisodes(const bool enabled) const {
  setValue("track.kodi.playEpisodes", enabled);
}

////////////////////////////////////////////////////////////////////////////////
// Torrents

std::string Settings::torrentFeedUrl() const {
  return value("torrent.discovery.source", u"https://nyaa.si/?page=rss&c=1_2&f=0"_s)
      .toString()
      .toStdString();
}

std::string Settings::torrentSearchUrl() const {
  return value("torrent.discovery.searchUrl", u"https://nyaa.si/?page=rss&q=%title%&c=1_2&f=0"_s)
      .toString()
      .toStdString();
}

bool Settings::torrentAutoCheckEnabled() const {
  return value("torrent.discovery.autoCheck", true).toBool();
}

std::chrono::minutes Settings::torrentAutoCheckInterval() const {
  const std::chrono::minutes interval{value("torrent.discovery.autoCheckInterval", 60).toInt()};
  return std::clamp(interval, std::chrono::minutes{10}, std::chrono::minutes{24 * 60});
}

std::string Settings::torrentNewAction() const {
  return value("torrent.discovery.newAction", u"notify"_s).toString().toStdString();
}

std::string Settings::torrentClientUrl() const {
  return value("torrent.client.url", u"http://127.0.0.1:8080"_s).toString().toStdString();
}

std::string Settings::torrentClientUsername() const {
  return value("torrent.client.username").toString().toStdString();
}

std::string Settings::torrentClientPassword() const {
  return value("torrent.client.password").toString().toStdString();
}

std::string Settings::torrentDownloadPath() const {
  return value("torrent.download.path").toString().toStdString();
}

bool Settings::torrentCreateSubfolder() const {
  return value("torrent.download.createSubfolder", true).toBool();
}

std::string Settings::torrentCategory() const {
  return value("torrent.download.category", u"anime"_s).toString().toStdString();
}

bool Settings::torrentUseMagnet() const {
  return value("torrent.download.useMagnet", false).toBool();
}

bool Settings::torrentFilterEnabled() const {
  return value("torrent.filter.enabled", true).toBool();
}

std::optional<QJsonArray> Settings::torrentFilters() const {
  const auto filters = value("torrent.filter.items");
  if (!filters.isValid()) return std::nullopt;
  return QJsonValue::fromVariant(filters).toArray();
}

void Settings::setTorrentFeedUrl(const std::string& url) const {
  setValue("torrent.discovery.source", url);
}

void Settings::setTorrentSearchUrl(const std::string& url) const {
  setValue("torrent.discovery.searchUrl", url);
}

void Settings::setTorrentAutoCheckEnabled(const bool enabled) const {
  setValue("torrent.discovery.autoCheck", enabled);
}

void Settings::setTorrentAutoCheckInterval(const std::chrono::minutes interval) const {
  setValue("torrent.discovery.autoCheckInterval", static_cast<int>(interval.count()));
}

void Settings::setTorrentNewAction(const std::string& action) const {
  setValue("torrent.discovery.newAction", action);
}

void Settings::setTorrentClientUrl(const std::string& url) const {
  setValue("torrent.client.url", url);
}

void Settings::setTorrentClientUsername(const std::string& username) const {
  setValue("torrent.client.username", username);
}

void Settings::setTorrentClientPassword(const std::string& password) const {
  setValue("torrent.client.password", password);
}

void Settings::setTorrentDownloadPath(const std::string& path) const {
  setValue("torrent.download.path", path);
}

void Settings::setTorrentCreateSubfolder(const bool enabled) const {
  setValue("torrent.download.createSubfolder", enabled);
}

void Settings::setTorrentCategory(const std::string& category) const {
  setValue("torrent.download.category", category);
}

void Settings::setTorrentUseMagnet(const bool enabled) const {
  setValue("torrent.download.useMagnet", enabled);
}

void Settings::setTorrentFilterEnabled(const bool enabled) const {
  setValue("torrent.filter.enabled", enabled);
}

void Settings::setTorrentFilters(const QJsonArray& filters) const {
  setValue("torrent.filter.items", filters);
}

////////////////////////////////////////////////////////////////////////////////
// Media server (DLNA)

bool Settings::mediaServerEnabled() const {
  return value("mediaServer.enabled", false).toBool();
}

std::string Settings::mediaServerName() const {
  const auto defaultName = u"Taiga (%1)"_s.arg(QSysInfo::machineHostName());
  const auto name = value("mediaServer.name", defaultName).toString().trimmed();
  return (name.isEmpty() ? defaultName : name).toStdString();
}

int Settings::mediaServerPort() const {
  return std::clamp(value("mediaServer.port", 8200).toInt(), 1024, 65535);
}

std::string Settings::mediaServerTranscoding() const {
  const auto mode = value("mediaServer.transcoding", u"none"_s).toString();
  if (mode == u"software" || mode == u"hardware") return mode.toStdString();
  return "none";
}

std::string Settings::mediaServerVaapiDevice() const {
  return value("mediaServer.vaapiDevice", u"/dev/dri/renderD128"_s).toString().toStdString();
}

int Settings::mediaServerBitrate() const {
  return std::clamp(value("mediaServer.bitrate", 8000).toInt(), 1000, 50000);
}

bool Settings::mediaServerBurnSubtitles() const {
  return value("mediaServer.burnSubtitles", true).toBool();
}

bool Settings::mediaServerVirtualFolders() const {
  return value("mediaServer.virtualFolders", true).toBool();
}

bool Settings::mediaServerLocalOnly() const {
  return value("mediaServer.localOnly", true).toBool();
}

bool Settings::mediaServerDetection() const {
  return value("mediaServer.detection", true).toBool();
}

std::string Settings::mediaServerUuid() const {
  // The UDN must stay the same, so that clients recognize the server after restarts.
  auto uuid = value("mediaServer.uuid").toString();
  if (uuid.isEmpty()) {
    uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    setValue("mediaServer.uuid", uuid);
  }
  return uuid.toStdString();
}

void Settings::setMediaServerEnabled(const bool enabled) const {
  setValue("mediaServer.enabled", enabled);
}

void Settings::setMediaServerName(const std::string& name) const {
  setValue("mediaServer.name", name);
}

void Settings::setMediaServerPort(const int port) const {
  setValue("mediaServer.port", port);
}

void Settings::setMediaServerTranscoding(const std::string& mode) const {
  setValue("mediaServer.transcoding", mode);
}

void Settings::setMediaServerVaapiDevice(const std::string& device) const {
  setValue("mediaServer.vaapiDevice", device);
}

void Settings::setMediaServerBitrate(const int bitrate) const {
  setValue("mediaServer.bitrate", bitrate);
}

void Settings::setMediaServerBurnSubtitles(const bool enabled) const {
  setValue("mediaServer.burnSubtitles", enabled);
}

void Settings::setMediaServerVirtualFolders(const bool enabled) const {
  setValue("mediaServer.virtualFolders", enabled);
}

void Settings::setMediaServerLocalOnly(const bool enabled) const {
  setValue("mediaServer.localOnly", enabled);
}

void Settings::setMediaServerDetection(const bool enabled) const {
  setValue("mediaServer.detection", enabled);
}

////////////////////////////////////////////////////////////////////////////////
// Discord

bool Settings::discordEnabled() const {
  return value("share.discord.enabled", false).toBool();
}

std::string Settings::discordApplicationId() const {
  // Taiga's application, which has the service logos as assets
  return value("share.discord.applicationId", u"379871385176244224"_s).toString().toStdString();
}

bool Settings::discordShowGroup() const {
  return value("share.discord.showGroup", true).toBool();
}

bool Settings::discordShowTime() const {
  return value("share.discord.showTime", true).toBool();
}

bool Settings::discordShowUsername() const {
  return value("share.discord.showUsername", true).toBool();
}

bool Settings::discordShowButton() const {
  return value("share.discord.showButton", true).toBool();
}

void Settings::setDiscordEnabled(const bool enabled) const {
  setValue("share.discord.enabled", enabled);
}

void Settings::setDiscordApplicationId(const std::string& id) const {
  setValue("share.discord.applicationId", id);
}

void Settings::setDiscordShowGroup(const bool enabled) const {
  setValue("share.discord.showGroup", enabled);
}

void Settings::setDiscordShowTime(const bool enabled) const {
  setValue("share.discord.showTime", enabled);
}

void Settings::setDiscordShowUsername(const bool enabled) const {
  setValue("share.discord.showUsername", enabled);
}

void Settings::setDiscordShowButton(const bool enabled) const {
  setValue("share.discord.showButton", enabled);
}

}  // namespace taiga
