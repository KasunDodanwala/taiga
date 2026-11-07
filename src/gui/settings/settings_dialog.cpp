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

#include "settings_dialog.hpp"

#include <QScrollArea>
#include <QScrollBar>
#include <QTreeWidgetItemIterator>
#include <algorithm>

#include "base/string.hpp"
#include "gui/settings/settings_page_accounts.hpp"
#include "gui/settings/settings_page_advanced.hpp"
#include "gui/settings/settings_page_application.hpp"
#include "gui/settings/settings_page_discord.hpp"
#include "gui/settings/settings_page_kodi.hpp"
#include "gui/settings/settings_page_library.hpp"
#include "gui/settings/settings_page_media_server.hpp"
#include "gui/settings/settings_page_media_players.hpp"
#include "gui/settings/settings_page_recognition.hpp"
#include "gui/settings/settings_page_streaming.hpp"
#include "gui/settings/settings_page_torrents.hpp"
#include "gui/utils/theme.hpp"
#include "ui_settings_dialog.h"

#ifdef Q_OS_WINDOWS
#include "gui/platforms/windows.hpp"
#endif

namespace gui {

constexpr int kPageRole = Qt::UserRole;

SettingsDialog::SettingsDialog(QWidget* parent) : QDialog(parent), ui_(new Ui::SettingsDialog) {
  ui_->setupUi(this);

  // Large enough for the tallest pages (e.g. Media server) to fit without scrolling
  resize(std::max(width(), 940), std::max(height(), 720));

#ifdef Q_OS_WINDOWS
  enableMicaBackground(this);
#endif

  ui_->treeWidget->setIndentation(22);

  const auto add_item = [this](QString icon, QString text, QWidget* page = nullptr) {
    auto item = new QTreeWidgetItem(ui_->treeWidget, QStringList(text));
    item->setIcon(0, theme.getIcon(icon));
    item->setSizeHint(0, QSize{0, 24});
    item->setData(0, kPageRole, QVariant::fromValue(page));
    if (!page) item->setDisabled(true);
    return item;
  };

  const auto add_child = [this](QTreeWidgetItem* parent, QString text, QWidget* page = nullptr) {
    auto item = new QTreeWidgetItem(parent, QStringList(text));
    item->setData(0, kPageRole, QVariant::fromValue(page));
    if (!page) item->setDisabled(true);
  };

  // Pages that build their widgets in code
  auto discordPage = new SettingsPageDiscord(ui_, this);
  auto kodiPage = new SettingsPageKodi(ui_, this);
  auto mediaServerPage = new SettingsPageMediaServer(ui_, this);
  auto torrentsPage = new SettingsPageTorrents(ui_, this);
  torrentsPage_ = torrentsPage;
  for (auto widget : {discordPage->widget(), kodiPage->widget(), mediaServerPage->widget(),
                      torrentsPage->widget()}) {
    ui_->stackedWidget->addWidget(widget);
  }

  add_item("account_circle", "Accounts", ui_->accountsPage);
  add_item("web_asset", "Application", ui_->applicationPage);
  add_item("list_alt", "Anime List");
  add_item("folder", "Library", ui_->libraryPage);
  add_item("play_arrow", "Media server", mediaServerPage->widget());
  {
    auto item = add_item("check_circle", "Recognition", ui_->recognitionPage);
    add_child(item, "Media players", ui_->mediaPlayersPage);
    add_child(item, "Kodi", kodiPage->widget());
    add_child(item, "Streaming", ui_->streamingPage);
  }
  {
    auto item = add_item("share", "Sharing", discordPage->widget());
    add_child(item, "Discord", discordPage->widget());
    add_child(item, "HTTP");
    add_child(item, "mIRC");
  }
  // Discovery, Downloads and Filters are tabs of this page, as in Taiga 1.x
  add_item("rss_feed", "Torrents", torrentsPage->widget());
  {
    auto item = add_item("warning", "Advanced", ui_->advancedPage);
    add_child(item, "Cache");
  }

  ui_->treeWidget->expandAll();

  connect(ui_->treeWidget, &QTreeWidget::currentItemChanged, this,
          [this](QTreeWidgetItem* current, QTreeWidgetItem*) {
            if (current) {
              auto text = current->text(0);
              if (current->parent()) {
                text = u"%1 / %2"_s.arg(current->parent()->text(0), text);
              }
              ui_->titleLabel->setText(text);

              auto page = current->data(0, kPageRole).value<QWidget*>();
              ui_->stackedWidget->setCurrentWidget(page ? page : ui_->todoPage);
              if (auto area = qobject_cast<QScrollArea*>(page)) {
                area->verticalScrollBar()->setValue(0);
              }
            }
          });

  ui_->treeWidget->setCurrentItem(ui_->treeWidget->topLevelItem(0));

  pages_ = {
      // clang-format off
      new SettingsPageAccounts(ui_, this),
      new SettingsPageAdvanced(ui_, this),
      new SettingsPageApplication(ui_, this),
      discordPage,
      kodiPage,
      new SettingsPageLibrary(ui_, this),
      mediaServerPage,
      new SettingsPageMediaPlayers(ui_, this),
      new SettingsPageRecognition(ui_, this),
      new SettingsPageStreaming(ui_, this),
      torrentsPage,
      // clang-format on
  };
  for (auto page : pages_) {
    page->load();
  }
}

void SettingsDialog::show(QWidget* parent, const QString& section) {
  auto dlg = new SettingsDialog(parent);
  dlg->setAttribute(Qt::WA_DeleteOnClose);
  dlg->setModal(true);
  if (!section.isEmpty()) dlg->setCurrentSection(section);
  dlg->QDialog::show();
}

void SettingsDialog::setCurrentSection(const QString& section) {
  const auto normalize = [](QString text) { return text.replace(u'-', u' ').simplified().toLower(); };
  const auto name = normalize(section);

  for (QTreeWidgetItemIterator it{ui_->treeWidget}; *it; ++it) {
    if (normalize((*it)->text(0)) == name && !(*it)->isDisabled()) {
      ui_->treeWidget->setCurrentItem(*it);
      return;
    }
  }

  // Tabs of the Torrents page (e.g. "filters")
  if (torrentsPage_ && torrentsPage_->selectTab(name)) {
    setCurrentSection(u"torrents"_s);
  }
}

void SettingsDialog::accept() {
  for (const auto page : pages_) {
    page->apply();
  }
  QDialog::accept();
}

}  // namespace gui
