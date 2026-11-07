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

#include "settings_page_kodi.hpp"

#include <QCheckBox>
#include <QDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include "base/string.hpp"
#include "taiga/settings.hpp"
#include "track/kodi.hpp"

namespace gui {

SettingsPageKodi::SettingsPageKodi(Ui::SettingsDialog* ui, QDialog* dialog)
    : SettingsPage(ui, dialog), m_widget(new QWidget(dialog)) {
  auto layout = new QVBoxLayout(m_widget);

  m_enabled = new QCheckBox(tr("Connect to Kodi via JSON-RPC"), m_widget);
  layout->addWidget(m_enabled);

  // Connection
  {
    auto group = new QGroupBox(tr("Connection"), m_widget);
    auto form = new QFormLayout(group);

    m_host = new QLineEdit(group);
    m_host->setPlaceholderText(u"127.0.0.1"_s);
    form->addRow(tr("Host:"), m_host);

    m_port = new QSpinBox(group);
    m_port->setRange(1, 65535);
    form->addRow(tr("Port:"), m_port);

    m_username = new QLineEdit(group);
    form->addRow(tr("Username:"), m_username);

    m_password = new QLineEdit(group);
    m_password->setEchoMode(QLineEdit::Password);
    form->addRow(tr("Password:"), m_password);

    auto testButton = new QPushButton(tr("Test connection"), group);
    m_status = new QLabel(group);
    m_status->setWordWrap(true);
    auto row = new QHBoxLayout();
    row->addWidget(testButton);
    row->addWidget(m_status, 1);
    form->addRow(row);
    connect(testButton, &QPushButton::clicked, this, &SettingsPageKodi::testConnection);

    layout->addWidget(group);
  }

  // Detection
  {
    auto group = new QGroupBox(tr("Detection"), m_widget);
    auto groupLayout = new QVBoxLayout(group);
    m_detection = new QCheckBox(tr("Ask Kodi what it is playing"), group);
    groupLayout->addWidget(m_detection);
    auto label = new QLabel(
        tr("Like in Taiga 1.x, Kodi is detected as a media player by the files it has open, which "
           "also covers what it streams to other devices through its UPnP/DLNA server. When this "
           "is enabled, Kodi's own report takes precedence for local playback, which tells "
           "whether playback is paused and works for files on network shares."),
        group);
    label->setWordWrap(true);
    groupLayout->addWidget(label);
    layout->addWidget(group);
  }

  // Playback
  {
    auto group = new QGroupBox(tr("Playback"), m_widget);
    auto groupLayout = new QVBoxLayout(group);
    m_playEpisodes = new QCheckBox(tr("Play episodes in Kodi"), group);
    m_playEpisodes->setToolTip(
        tr("Used by \"Play next episode\", \"Play random episode\" and the library."));
    groupLayout->addWidget(m_playEpisodes);
    layout->addWidget(group);
  }

  auto info = new QLabel(
      tr("Requires <i>Allow remote control via HTTP</i> in Kodi's <i>Settings &gt; Services &gt; "
         "Control</i>."),
      m_widget);
  info->setWordWrap(true);
  info->setTextFormat(Qt::RichText);
  layout->addWidget(info);

  layout->addStretch();

  connect(m_enabled, &QCheckBox::toggled, this, [this](bool checked) {
    for (QWidget* widget :
         std::initializer_list<QWidget*>{m_host, m_port, m_username, m_password, m_detection,
                                         m_playEpisodes}) {
      widget->setEnabled(checked);
    }
  });
}

QWidget* SettingsPageKodi::widget() const {
  if (!m_page) m_page = scrollable(m_widget);
  return m_page;
}

void SettingsPageKodi::load() {
  m_enabled->setChecked(taiga::settings.kodiEnabled());
  m_host->setText(QString::fromStdString(taiga::settings.kodiHost()));
  m_port->setValue(taiga::settings.kodiPort());
  m_username->setText(QString::fromStdString(taiga::settings.kodiUsername()));
  m_password->setText(QString::fromStdString(taiga::settings.kodiPassword()));
  m_detection->setChecked(taiga::settings.kodiDetection());
  m_playEpisodes->setChecked(taiga::settings.kodiPlayEpisodes());
}

void SettingsPageKodi::apply() const {
  const auto host = m_host->text().trimmed();
  taiga::settings.setKodiEnabled(m_enabled->isChecked());
  taiga::settings.setKodiHost(host.isEmpty() ? "127.0.0.1" : host.toStdString());
  taiga::settings.setKodiPort(m_port->value());
  taiga::settings.setKodiUsername(m_username->text().toStdString());
  taiga::settings.setKodiPassword(m_password->text().toStdString());
  taiga::settings.setKodiDetection(m_detection->isChecked());
  taiga::settings.setKodiPlayEpisodes(m_playEpisodes->isChecked());
}

void SettingsPageKodi::testConnection() {
  const track::kodi::Endpoint endpoint{
      .host = m_host->text().trimmed().isEmpty() ? u"127.0.0.1"_s : m_host->text().trimmed(),
      .port = m_port->value(),
      .username = m_username->text(),
      .password = m_password->text(),
  };

  m_status->setText(tr("Connecting..."));

  QPointer<QLabel> status = m_status;
  track::kodi::client()->testConnection(endpoint, [status](bool success, const QString& message) {
    if (!status) return;  // the dialog was closed
    status->setText(success ? message : tr("Failed: %1").arg(message));
  });
}

}  // namespace gui
