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

#include "settings_page_discord.hpp"

#include <QCheckBox>
#include <QDialog>
#include <QGroupBox>
#include <QLabel>
#include <QVBoxLayout>

#include "share/discord.hpp"
#include "taiga/settings.hpp"

namespace gui {

SettingsPageDiscord::SettingsPageDiscord(Ui::SettingsDialog* ui, QDialog* dialog)
    : SettingsPage(ui, dialog), m_widget(new QWidget(dialog)) {
  auto layout = new QVBoxLayout(m_widget);

  m_enabled = new QCheckBox(tr("Show what I'm watching on my Discord profile (Rich Presence)"),
                            m_widget);
  layout->addWidget(m_enabled);

  auto group = new QGroupBox(tr("Details"), m_widget);
  auto groupLayout = new QVBoxLayout(group);
  m_showGroup = new QCheckBox(tr("Fansub group"), group);
  m_showTime = new QCheckBox(tr("Elapsed time"), group);
  m_showUsername = new QCheckBox(tr("Username"), group);
  m_showButton = new QCheckBox(tr("\"View Anime\" button"), group);
  for (auto checkBox : {m_showGroup, m_showTime, m_showUsername, m_showButton}) {
    groupLayout->addWidget(checkBox);
  }
  layout->addWidget(group);

  auto hint = new QLabel(
      tr("Requires the Discord app to be running on this computer. Anime that are marked as "
         "private in your list are never shared."),
      m_widget);
  hint->setWordWrap(true);
  hint->setForegroundRole(QPalette::PlaceholderText);
  layout->addWidget(hint);

  layout->addStretch();

  connect(m_enabled, &QCheckBox::toggled, group, &QWidget::setEnabled);
}

QWidget* SettingsPageDiscord::widget() const {
  if (!m_page) m_page = scrollable(m_widget);
  return m_page;
}

void SettingsPageDiscord::load() {
  m_enabled->setChecked(taiga::settings.discordEnabled());
  m_showGroup->setChecked(taiga::settings.discordShowGroup());
  m_showTime->setChecked(taiga::settings.discordShowTime());
  m_showUsername->setChecked(taiga::settings.discordShowUsername());
  m_showButton->setChecked(taiga::settings.discordShowButton());
}

void SettingsPageDiscord::apply() const {
  taiga::settings.setDiscordEnabled(m_enabled->isChecked());
  taiga::settings.setDiscordShowGroup(m_showGroup->isChecked());
  taiga::settings.setDiscordShowTime(m_showTime->isChecked());
  taiga::settings.setDiscordShowUsername(m_showUsername->isChecked());
  taiga::settings.setDiscordShowButton(m_showButton->isChecked());
  share::discord::presence()->refresh();
}

}  // namespace gui
