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

#include "settings_page_media_server.hpp"

#include <QButtonGroup>
#include <QCheckBox>
#include <QDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QRadioButton>
#include <QSpinBox>
#include <QSysInfo>
#include <QVBoxLayout>

#include "base/string.hpp"
#include "media_server/server.hpp"
#include "taiga/settings.hpp"

namespace gui {

namespace {

enum TranscodingMode { None, Software, Hardware };

QLabel* createHint(const QString& text, QWidget* parent) {
  auto label = new QLabel(text, parent);
  label->setWordWrap(true);
  label->setTextFormat(Qt::RichText);
  label->setForegroundRole(QPalette::PlaceholderText);
  return label;
}

}  // namespace

SettingsPageMediaServer::SettingsPageMediaServer(Ui::SettingsDialog* ui, QDialog* dialog)
    : SettingsPage(ui, dialog), m_widget(new QWidget(dialog)) {
  auto layout = new QVBoxLayout(m_widget);

  m_enabled = new QCheckBox(tr("Share anime folders on the network (DLNA)"), m_widget);
  layout->addWidget(m_enabled);
  layout->addWidget(createHint(
      tr("Shares the folders in <i>Library</i> settings with DLNA/UPnP players on your network, "
         "e.g. VLC, Infuse or a smart TV. What you watch through it is detected, so your list is "
         "updated even when you watch on another device."),
      m_widget));

  // Server
  {
    auto group = new QGroupBox(tr("Server"), m_widget);
    auto form = new QFormLayout(group);

    m_name = new QLineEdit(group);
    m_name->setPlaceholderText(u"Taiga (%1)"_s.arg(QSysInfo::machineHostName()));
    form->addRow(tr("Name:"), m_name);

    m_port = new QSpinBox(group);
    m_port->setRange(1024, 65535);
    form->addRow(tr("Port:"), m_port);

    m_virtualFolders = new QCheckBox(tr("Add \"Currently watching\" and \"Recently added\" folders"),
                                     group);
    form->addRow(m_virtualFolders);

    m_detection = new QCheckBox(tr("Update anime list for what is watched through the server"),
                                group);
    form->addRow(m_detection);

    m_localOnly = new QCheckBox(tr("Only allow devices on the local network and Tailscale"), group);
    form->addRow(m_localOnly);

    // A word-wrapped label in the field column would keep its narrow preferred width, wrap, and
    // get clipped (the form gives the row the height of a single line). Letting it expand to the
    // full width keeps the status on one line.
    m_status = new QLabel(group);
    m_status->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(tr("Status:"), m_status);

    layout->addWidget(group);
  }

  // Transcoding
  {
    auto group = new QGroupBox(tr("Transcoding"), m_widget);
    auto form = new QFormLayout(group);

    m_transcoding = new QButtonGroup(group);
    m_transcodeNone = new QRadioButton(tr("Off: stream files as they are (recommended)"), group);
    m_transcodeSoftware = new QRadioButton(tr("Software (CPU, x264)"), group);
    m_transcodeHardware = new QRadioButton(tr("Hardware (GPU, VA-API)"), group);
    m_transcoding->addButton(m_transcodeNone, None);
    m_transcoding->addButton(m_transcodeSoftware, Software);
    m_transcoding->addButton(m_transcodeHardware, Hardware);
    form->addRow(m_transcodeNone);
    form->addRow(m_transcodeSoftware);
    form->addRow(m_transcodeHardware);

    m_vaapiDevice = new QLineEdit(group);
    m_vaapiDevice->setPlaceholderText(u"/dev/dri/renderD128"_s);
    form->addRow(tr("VA-API device:"), m_vaapiDevice);

    m_bitrate = new QSpinBox(group);
    m_bitrate->setRange(1, 50);
    m_bitrate->setSuffix(tr(" Mbit/s"));
    form->addRow(tr("Maximum bitrate:"), m_bitrate);

    m_burnSubtitles = new QCheckBox(tr("Burn in subtitles"), group);
    form->addRow(m_burnSubtitles);

    form->addRow(createHint(
        tr("Only needed for players that can't handle a file's format. With transcoding on, "
           "players get an H.264/AAC stream first, and the original file second. Image-based "
           "subtitles (e.g. PGS) can't be burned in."),
        group));

    layout->addWidget(group);

    connect(m_transcoding, &QButtonGroup::idToggled, this, &SettingsPageMediaServer::updateStates);
  }

  layout->addStretch();

  connect(m_enabled, &QCheckBox::toggled, this, &SettingsPageMediaServer::updateStates);
  connect(media_server::server(), &media_server::Server::statusChanged, this,
          &SettingsPageMediaServer::updateStatus);
}

QWidget* SettingsPageMediaServer::widget() const {
  if (!m_page) m_page = scrollable(m_widget);
  return m_page;
}

void SettingsPageMediaServer::load() {
  using taiga::settings;

  m_enabled->setChecked(settings.mediaServerEnabled());
  const auto name = QString::fromStdString(settings.mediaServerName());
  m_name->setText(name == m_name->placeholderText() ? QString{} : name);
  m_port->setValue(settings.mediaServerPort());
  m_virtualFolders->setChecked(settings.mediaServerVirtualFolders());
  m_detection->setChecked(settings.mediaServerDetection());
  m_localOnly->setChecked(settings.mediaServerLocalOnly());

  const auto mode = settings.mediaServerTranscoding();
  m_transcoding->button(mode == "software" ? Software : mode == "hardware" ? Hardware : None)
      ->setChecked(true);
  m_vaapiDevice->setText(QString::fromStdString(settings.mediaServerVaapiDevice()));
  m_bitrate->setValue(settings.mediaServerBitrate() / 1000);
  m_burnSubtitles->setChecked(settings.mediaServerBurnSubtitles());

  updateStates();
  updateStatus();
}

void SettingsPageMediaServer::apply() const {
  using taiga::settings;

  settings.setMediaServerEnabled(m_enabled->isChecked());
  settings.setMediaServerName(m_name->text().trimmed().toStdString());
  settings.setMediaServerPort(m_port->value());
  settings.setMediaServerVirtualFolders(m_virtualFolders->isChecked());
  settings.setMediaServerDetection(m_detection->isChecked());
  settings.setMediaServerLocalOnly(m_localOnly->isChecked());

  const auto mode = m_transcoding->checkedId();
  settings.setMediaServerTranscoding(mode == Software   ? "software"
                                     : mode == Hardware ? "hardware"
                                                        : "none");
  const auto device = m_vaapiDevice->text().trimmed();
  settings.setMediaServerVaapiDevice(device.isEmpty() ? "/dev/dri/renderD128" : device.toStdString());
  settings.setMediaServerBitrate(m_bitrate->value() * 1000);
  settings.setMediaServerBurnSubtitles(m_burnSubtitles->isChecked());

  media_server::server()->restart();
}

void SettingsPageMediaServer::updateStates() {
  const bool enabled = m_enabled->isChecked();
  const auto mode = m_transcoding->checkedId();

  for (QWidget* widget : std::initializer_list<QWidget*>{
           m_name, m_port, m_virtualFolders, m_detection, m_localOnly, m_transcodeNone,
           m_transcodeSoftware, m_transcodeHardware}) {
    widget->setEnabled(enabled);
  }
  m_vaapiDevice->setEnabled(enabled && mode == Hardware);
  m_bitrate->setEnabled(enabled && mode != None);
  m_burnSubtitles->setEnabled(enabled && mode != None);
}

void SettingsPageMediaServer::updateStatus() {
  m_status->setText(media_server::server()->statusText());
}

}  // namespace gui
