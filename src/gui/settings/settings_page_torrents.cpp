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

#include "settings_page_torrents.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTabWidget>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <chrono>
#include <utility>

#include "base/string.hpp"
#include "gui/torrents/feed_filter_dialog.hpp"
#include "gui/utils/theme.hpp"
#include "taiga/settings.hpp"
#include "track/feed_aggregator.hpp"
#include "track/torrent_client.hpp"

namespace gui {

using namespace track::feed;

namespace {

enum FilterColumn { COLUMN_NAME, COLUMN_LIMITS };

QLabel* createHint(const QString& text, QWidget* parent) {
  auto label = new QLabel(text, parent);
  label->setWordWrap(true);
  label->setTextFormat(Qt::RichText);
  label->setForegroundRole(QPalette::PlaceholderText);
  return label;
}

QToolButton* createToolButton(const QString& icon, const QString& tooltip, QWidget* parent) {
  auto button = new QToolButton(parent);
  button->setIcon(theme.getIcon(icon));
  button->setToolTip(tooltip);
  button->setAutoRaise(true);
  return button;
}

QFrame* createSeparator(QWidget* parent) {
  auto separator = new QFrame(parent);
  separator->setFrameShape(QFrame::VLine);
  separator->setFrameShadow(QFrame::Sunken);
  return separator;
}

}  // namespace

SettingsPageTorrents::SettingsPageTorrents(Ui::SettingsDialog* ui, QDialog* dialog)
    : SettingsPage(ui, dialog), m_tabs(new QTabWidget(dialog)) {
  m_tabs->addTab(scrollable(createDiscoveryTab()), tr("Discovery"));
  m_tabs->addTab(scrollable(createDownloadsTab()), tr("Downloads"));
  m_tabs->addTab(createFiltersTab(), tr("Filters"));
}

QWidget* SettingsPageTorrents::widget() const {
  return m_tabs;
}

bool SettingsPageTorrents::selectTab(const QString& name) {
  for (int i = 0; i < m_tabs->count(); ++i) {
    if (m_tabs->tabText(i).compare(name, Qt::CaseInsensitive) == 0) {
      m_tabs->setCurrentIndex(i);
      return true;
    }
  }
  return false;
}

QWidget* SettingsPageTorrents::createDiscoveryTab() {
  auto page = new QWidget(m_tabs);
  auto layout = new QVBoxLayout(page);

  // Source
  {
    auto group = new QGroupBox(tr("Source"), page);
    auto form = new QFormLayout(group);
    m_feedUrl = new QLineEdit(group);
    form->addRow(tr("Feed URL:"), m_feedUrl);
    m_searchUrl = new QLineEdit(group);
    form->addRow(tr("Search URL:"), m_searchUrl);
    form->addRow(createHint(tr("<code>%title%</code> is replaced with the anime title."), group));
    layout->addWidget(group);
  }

  // New torrents
  {
    auto group = new QGroupBox(tr("New torrents"), page);
    auto form = new QFormLayout(group);

    m_autoCheck = new QCheckBox(tr("Check for new torrents every"), group);
    m_autoCheckInterval = new QSpinBox(group);
    m_autoCheckInterval->setRange(10, 24 * 60);
    m_autoCheckInterval->setSuffix(tr(" minutes"));
    auto row = new QHBoxLayout();
    row->addWidget(m_autoCheck);
    row->addWidget(m_autoCheckInterval);
    row->addStretch();
    form->addRow(row);
    connect(m_autoCheck, &QCheckBox::toggled, m_autoCheckInterval, &QWidget::setEnabled);

    m_newAction = new QComboBox(group);
    m_newAction->addItem(tr("Notify me"), u"notify"_s);
    m_newAction->addItem(tr("Download them"), u"download"_s);
    m_newAction->addItem(tr("Do nothing"), u"none"_s);
    form->addRow(tr("When new torrents are found:"), m_newAction);

    layout->addWidget(group);
  }

  layout->addStretch();
  return page;
}

QWidget* SettingsPageTorrents::createDownloadsTab() {
  auto page = new QWidget(m_tabs);
  auto layout = new QVBoxLayout(page);

  // Client
  {
    auto group = new QGroupBox(tr("BitTorrent client (qBittorrent Web UI)"), page);
    auto form = new QFormLayout(group);

    m_clientUrl = new QLineEdit(group);
    m_clientUrl->setPlaceholderText(u"http://127.0.0.1:8080"_s);
    form->addRow(tr("Web UI address:"), m_clientUrl);
    m_clientUsername = new QLineEdit(group);
    form->addRow(tr("Username:"), m_clientUsername);
    m_clientPassword = new QLineEdit(group);
    m_clientPassword->setEchoMode(QLineEdit::Password);
    form->addRow(tr("Password:"), m_clientPassword);

    auto testButton = new QPushButton(tr("Test connection"), group);
    m_clientStatus = new QLabel(group);
    m_clientStatus->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto row = new QHBoxLayout();
    row->addWidget(testButton);
    row->addWidget(m_clientStatus, 1);
    form->addRow(row);
    connect(testButton, &QPushButton::clicked, this, &SettingsPageTorrents::testConnection);

    form->addRow(createHint(
        tr("Works with <code>qbittorrent-nox</code> and the desktop version (Tools &gt; Options "
           "&gt; Web UI). Username and password are not needed if <i>Bypass authentication for "
           "clients on localhost</i> is enabled."),
        group));

    layout->addWidget(group);
  }

  // Download location
  {
    auto group = new QGroupBox(tr("Download location"), page);
    auto form = new QFormLayout(group);

    m_downloadPath = new QLineEdit(group);
    m_downloadPath->setPlaceholderText(tr("First anime folder in Library settings"));
    auto browseButton = new QPushButton(tr("Browse..."), group);
    auto row = new QHBoxLayout();
    row->addWidget(m_downloadPath, 1);
    row->addWidget(browseButton);
    form->addRow(tr("Folder:"), row);
    connect(browseButton, &QPushButton::clicked, this, [this]() {
      const auto folder = QFileDialog::getExistingDirectory(dialog_, tr("Select download folder"),
                                                            m_downloadPath->text());
      if (!folder.isEmpty()) m_downloadPath->setText(QDir::cleanPath(folder));
    });

    m_createSubfolder = new QCheckBox(tr("Create a subfolder for each anime"), group);
    form->addRow(m_createSubfolder);
    form->addRow(createHint(tr("New episodes are saved next to the ones that are already in your "
                               "library folders, if there are any."),
                            group));

    m_category = new QLineEdit(group);
    form->addRow(tr("Category:"), m_category);
    m_useMagnet = new QCheckBox(tr("Use magnet links if available"), group);
    form->addRow(m_useMagnet);

    layout->addWidget(group);
  }

  layout->addStretch();
  return page;
}

QWidget* SettingsPageTorrents::createFiltersTab() {
  auto page = new QWidget(m_tabs);
  auto layout = new QVBoxLayout(page);

  m_filterEnabled = new QCheckBox(tr("Enable torrent filters"), page);
  layout->addWidget(m_filterEnabled);
  layout->addWidget(
      createHint(tr("Filters allow you to download the files you want and ignore the others."),
                 page));

  m_filterList = new QTreeWidget(page);
  m_filterList->setRootIsDecorated(false);
  m_filterList->setUniformRowHeights(true);
  m_filterList->setAllColumnsShowFocus(true);
  m_filterList->setHeaderLabels({tr("Name"), tr("Applies to")});
  m_filterList->headerItem()->setTextAlignment(COLUMN_LIMITS, Qt::AlignRight | Qt::AlignVCenter);
  m_filterList->header()->setStretchLastSection(false);
  m_filterList->header()->setSectionResizeMode(COLUMN_NAME, QHeaderView::Stretch);
  m_filterList->header()->setSectionResizeMode(COLUMN_LIMITS, QHeaderView::ResizeToContents);
  m_filterList->installEventFilter(this);
  layout->addWidget(m_filterList, 1);

  // Toolbar (same buttons and tooltips as Taiga 1.x)
  m_filterToolbar = new QWidget(page);
  auto toolbar = new QHBoxLayout(m_filterToolbar);
  toolbar->setContentsMargins(0, 0, 0, 0);
  toolbar->setSpacing(2);
  toolbar->addStretch();

  auto addButton = createToolButton(u"add"_s, tr("Add new filter..."), m_filterToolbar);
  m_deleteFilterButton = createToolButton(u"remove"_s, tr("Delete filter"), m_filterToolbar);
  m_moveUpButton = createToolButton(u"arrow_upward"_s, tr("Move up"), m_filterToolbar);
  m_moveDownButton = createToolButton(u"arrow_downward"_s, tr("Move down"), m_filterToolbar);
  auto importButton = createToolButton(u"download"_s, tr("Import filters..."), m_filterToolbar);
  auto exportButton = createToolButton(u"upload"_s, tr("Export filters..."), m_filterToolbar);
  auto resetButton = createToolButton(u"close"_s, tr("Reset filters"), m_filterToolbar);

  toolbar->addWidget(addButton);
  toolbar->addWidget(m_deleteFilterButton);
  toolbar->addWidget(createSeparator(m_filterToolbar));
  toolbar->addWidget(m_moveUpButton);
  toolbar->addWidget(m_moveDownButton);
  toolbar->addWidget(createSeparator(m_filterToolbar));
  toolbar->addWidget(importButton);
  toolbar->addWidget(exportButton);
  toolbar->addWidget(createSeparator(m_filterToolbar));
  toolbar->addWidget(resetButton);
  layout->addWidget(m_filterToolbar);

  connect(addButton, &QToolButton::clicked, this, &SettingsPageTorrents::addFilter);
  connect(m_deleteFilterButton, &QToolButton::clicked, this, &SettingsPageTorrents::deleteFilter);
  connect(m_moveUpButton, &QToolButton::clicked, this, [this]() { moveFilter(-1); });
  connect(m_moveDownButton, &QToolButton::clicked, this, [this]() { moveFilter(1); });
  connect(importButton, &QToolButton::clicked, this, &SettingsPageTorrents::importFilters);
  connect(exportButton, &QToolButton::clicked, this, &SettingsPageTorrents::exportFilters);
  connect(resetButton, &QToolButton::clicked, this, &SettingsPageTorrents::resetFilters);

  connect(m_filterList, &QTreeWidget::itemSelectionChanged, this,
          &SettingsPageTorrents::updateFilterButtons);
  connect(m_filterList, &QTreeWidget::currentItemChanged, this,
          &SettingsPageTorrents::updateFilterButtons);
  connect(m_filterList, &QTreeWidget::itemDoubleClicked, this, &SettingsPageTorrents::editFilter);
  connect(m_filterList, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int) {
    const auto index = m_filterList->indexOfTopLevelItem(item);
    if (index >= 0 && index < static_cast<int>(m_filters.size())) {
      m_filters[index].enabled = item->checkState(COLUMN_NAME) == Qt::Checked;
    }
  });

  connect(m_filterEnabled, &QCheckBox::toggled, this, [this](bool checked) {
    m_filterList->setEnabled(checked);
    m_filterToolbar->setEnabled(checked);
  });

  return page;
}

bool SettingsPageTorrents::eventFilter(QObject* watched, QEvent* event) {
  if (watched == m_filterList && event->type() == QEvent::KeyPress) {
    switch (static_cast<QKeyEvent*>(event)->key()) {
      case Qt::Key_Delete:
        deleteFilter();
        return true;
      case Qt::Key_Return:
      case Qt::Key_Enter:
        editFilter();
        return true;
    }
  }
  return SettingsPage::eventFilter(watched, event);
}

void SettingsPageTorrents::refreshFilterList() {
  const QSignalBlocker blocker{m_filterList};
  m_filterList->clear();

  for (const auto& filter : m_filters) {
    auto item = new QTreeWidgetItem(m_filterList);
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(COLUMN_NAME, filter.enabled ? Qt::Checked : Qt::Unchecked);
    item->setIcon(COLUMN_NAME, feedFilterIcon(filter.action));
    item->setText(COLUMN_NAME, QString::fromStdString(filter.name));
    item->setText(COLUMN_LIMITS, filter.anime_ids.empty()
                                     ? tr("All")
                                     : tr("%1 anime").arg(filter.anime_ids.size()));
    item->setTextAlignment(COLUMN_LIMITS, Qt::AlignRight | Qt::AlignVCenter);
    item->setToolTip(COLUMN_NAME, u"%1\n%2: %3"_s.arg(translateAction(filter.action))
                                      .arg(translateMatching(filter.match))
                                      .arg(translateConditions(filter)));
  }

  updateFilterButtons();
}

void SettingsPageTorrents::syncFilterStates() const {
  for (int i = 0; i < m_filterList->topLevelItemCount() && i < static_cast<int>(m_filters.size());
       ++i) {
    m_filters[i].enabled = m_filterList->topLevelItem(i)->checkState(COLUMN_NAME) == Qt::Checked;
  }
}

void SettingsPageTorrents::updateFilterButtons() {
  const auto index = m_filterList->currentIndex().row();
  const bool selected = !m_filterList->selectedItems().isEmpty();
  const auto count = m_filterList->topLevelItemCount();
  m_deleteFilterButton->setEnabled(selected);
  m_moveUpButton->setEnabled(selected && index > 0);
  m_moveDownButton->setEnabled(selected && index > -1 && index < count - 1);
}

void SettingsPageTorrents::addFilter() {
  auto filter = FeedFilterDialog::edit(dialog_);
  if (!filter || filter->conditions.empty()) return;
  if (filter->name.empty()) filter->name = createNameFromConditions(*filter).toStdString();
  syncFilterStates();
  m_filters.push_back(std::move(*filter));
  refreshFilterList();
  m_filterList->setCurrentItem(m_filterList->topLevelItem(m_filterList->topLevelItemCount() - 1));
}

void SettingsPageTorrents::editFilter() {
  const auto index = m_filterList->currentIndex().row();
  if (index < 0 || index >= static_cast<int>(m_filters.size())) return;
  syncFilterStates();
  const auto filter = FeedFilterDialog::edit(dialog_, m_filters[index]);
  if (!filter || filter->conditions.empty()) return;
  m_filters[index] = *filter;
  refreshFilterList();
  m_filterList->setCurrentItem(m_filterList->topLevelItem(index));
}

void SettingsPageTorrents::deleteFilter() {
  const auto index = m_filterList->currentIndex().row();
  if (index < 0 || index >= static_cast<int>(m_filters.size())) return;
  syncFilterStates();
  m_filters.erase(m_filters.begin() + index);
  refreshFilterList();
}

void SettingsPageTorrents::moveFilter(int offset) {
  const auto index = m_filterList->currentIndex().row();
  const auto target = index + offset;
  if (index < 0 || target < 0 || target >= static_cast<int>(m_filters.size())) return;
  syncFilterStates();
  std::iter_swap(m_filters.begin() + index, m_filters.begin() + target);
  refreshFilterList();
  m_filterList->setCurrentItem(m_filterList->topLevelItem(target));
}

void SettingsPageTorrents::importFilters() {
  bool ok = false;
  const auto text = QInputDialog::getText(dialog_, tr("Import Filters"),
                                          tr("Enter encoded text below:"), QLineEdit::Normal, {},
                                          &ok);
  if (!ok || text.trimmed().isEmpty()) return;

  const auto filters = track::feed::importFilters(text.trimmed());
  if (!filters) {
    QMessageBox::critical(dialog_, tr("Import Filters"),
                          tr("Could not parse the filter string. It may be missing characters, "
                             "or encoded with an incompatible version of the application."));
    return;
  }

  // As in Taiga 1.x, imported filters replace the current ones.
  m_filters = *filters;
  refreshFilterList();
}

void SettingsPageTorrents::exportFilters() {
  syncFilterStates();
  const auto text = track::feed::exportFilters(m_filters);
  if (text.isEmpty()) return;
  QInputDialog::getText(dialog_, tr("Export Filters"),
                        tr("Copy the text below and share it with other people:"),
                        QLineEdit::Normal, text);
}

void SettingsPageTorrents::resetFilters() {
  QMessageBox box(QMessageBox::Information, tr("Reset Torrent Filters"),
                  tr("Are you sure you want to reset the filters?"),
                  QMessageBox::Yes | QMessageBox::No, dialog_);
  box.setInformativeText(tr("All custom filters will be lost."));
  if (box.exec() != QMessageBox::Yes) return;

  m_filters = defaultFilters();
  refreshFilterList();
}

void SettingsPageTorrents::load() {
  using namespace taiga;

  m_feedUrl->setText(QString::fromStdString(settings.torrentFeedUrl()));
  m_searchUrl->setText(QString::fromStdString(settings.torrentSearchUrl()));
  m_autoCheck->setChecked(settings.torrentAutoCheckEnabled());
  m_autoCheckInterval->setValue(static_cast<int>(settings.torrentAutoCheckInterval().count()));
  m_autoCheckInterval->setEnabled(m_autoCheck->isChecked());
  m_newAction->setCurrentIndex(
      std::max(0, m_newAction->findData(QString::fromStdString(settings.torrentNewAction()))));

  m_clientUrl->setText(QString::fromStdString(settings.torrentClientUrl()));
  m_clientUsername->setText(QString::fromStdString(settings.torrentClientUsername()));
  m_clientPassword->setText(QString::fromStdString(settings.torrentClientPassword()));
  m_downloadPath->setText(QString::fromStdString(settings.torrentDownloadPath()));
  m_createSubfolder->setChecked(settings.torrentCreateSubfolder());
  m_category->setText(QString::fromStdString(settings.torrentCategory()));
  m_useMagnet->setChecked(settings.torrentUseMagnet());

  m_filterEnabled->setChecked(settings.torrentFilterEnabled());
  m_filterList->setEnabled(m_filterEnabled->isChecked());
  m_filterToolbar->setEnabled(m_filterEnabled->isChecked());
  m_filters = track::feed::aggregator()->filters();
  refreshFilterList();
}

void SettingsPageTorrents::apply() const {
  using namespace taiga;

  settings.setTorrentFeedUrl(m_feedUrl->text().trimmed().toStdString());
  settings.setTorrentSearchUrl(m_searchUrl->text().trimmed().toStdString());
  settings.setTorrentAutoCheckEnabled(m_autoCheck->isChecked());
  settings.setTorrentAutoCheckInterval(std::chrono::minutes{m_autoCheckInterval->value()});
  settings.setTorrentNewAction(m_newAction->currentData().toString().toStdString());

  settings.setTorrentClientUrl(m_clientUrl->text().trimmed().toStdString());
  settings.setTorrentClientUsername(m_clientUsername->text().toStdString());
  settings.setTorrentClientPassword(m_clientPassword->text().toStdString());
  settings.setTorrentDownloadPath(m_downloadPath->text().trimmed().toStdString());
  settings.setTorrentCreateSubfolder(m_createSubfolder->isChecked());
  settings.setTorrentCategory(m_category->text().trimmed().toStdString());
  settings.setTorrentUseMagnet(m_useMagnet->isChecked());

  settings.setTorrentFilterEnabled(m_filterEnabled->isChecked());
  syncFilterStates();

  auto* aggregator = track::feed::aggregator();
  aggregator->setFilters(m_filters);
  aggregator->updateTimer();
  if (!aggregator->feed().items.empty()) aggregator->reexamine();
}

void SettingsPageTorrents::testConnection() {
  const track::torrent::QBittorrent::Endpoint endpoint{
      .url = m_clientUrl->text().trimmed().isEmpty() ? u"http://127.0.0.1:8080"_s
                                                     : m_clientUrl->text().trimmed(),
      .username = m_clientUsername->text(),
      .password = m_clientPassword->text(),
  };

  m_clientStatus->setText(tr("Connecting..."));

  QPointer<QLabel> status = m_clientStatus;
  track::torrent::qbittorrent()->testConnection(
      endpoint, [status](bool success, const QString& message) {
        if (!status) return;  // the dialog was closed
        status->setText(success ? message : tr("Failed: %1").arg(message));
      });
}

}  // namespace gui
