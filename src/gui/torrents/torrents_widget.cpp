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

#include "torrents_widget.hpp"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QCursor>
#include <QDateTime>
#include <QDesktopServices>
#include <QHeaderView>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QToolBar>
#include <QTreeWidget>
#include <QUrl>
#include <algorithm>

#include "base/string.hpp"
#include "gui/main/main_window.hpp"
#include "gui/settings/settings_dialog.hpp"
#include "gui/utils/theme.hpp"
#include "media/anime.hpp"
#include "media/anime_db.hpp"
#include "media/anime_utils.hpp"
#include "track/feed_aggregator.hpp"
#include "track/feed_filter.hpp"

namespace gui {

using track::feed::aggregator;
using track::feed::Item;
using track::feed::ItemState;
using track::feed::TorrentCategory;

namespace {

constexpr int kLinkRole = Qt::UserRole;

const Item* findItem(const QString& link) {
  const auto& items = aggregator()->feed().items;
  const auto it = std::ranges::find_if(
      items, [&link](const Item& item) { return QString::fromStdString(item.link) == link; });
  return it != items.end() ? &*it : nullptr;
}

QString menuText(QString text) {
  return text.replace(u'&', u"&&"_s);
}

QString episodeText(const Item& item) {
  const auto range = item.episode.episodeNumberRange();
  if (!range) {
    return item.torrent_category == TorrentCategory::Batch ? QObject::tr("Batch") : QString{};
  }
  if (range->second > range->first) return u"%1-%2"_s.arg(range->first).arg(range->second);

  auto text = QString::number(range->first);
  if (const auto version = item.episode.element(anitomy::ElementKind::ReleaseVersion);
      !version.empty() && version != "1") {
    text += u"v%1"_s.arg(QString::fromStdString(version));
  }
  return text;
}

QString dateText(const std::string& pubDate) {
  const auto date = QDateTime::fromString(QString::fromStdString(pubDate), Qt::RFC2822Date);
  if (!date.isValid()) return QString::fromStdString(pubDate);
  return QLocale().toString(date.toLocalTime(), QLocale::ShortFormat);
}

QString stateText(const Item& item) {
  const auto decidedBy = QString::fromStdString(item.decided_by);
  switch (item.state) {
    case ItemState::Selected:
      return decidedBy.isEmpty() ? QObject::tr("Marked for download")
                                 : QObject::tr("Marked for download by: %1").arg(decidedBy);
    case ItemState::DiscardedNormal:
    case ItemState::DiscardedInactive:
    case ItemState::DiscardedHidden:
      return decidedBy.isEmpty() ? QObject::tr("Discarded")
                                 : QObject::tr("Discarded by: %1").arg(decidedBy);
    case ItemState::Blank:
      break;
  }
  return {};
}

}  // namespace

TorrentsWidget::TorrentsWidget(QWidget* parent)
    : PageWidget{parent}, m_label(new QLabel(this)), m_view(new QTreeWidget(this)) {
  m_label->setTextFormat(Qt::RichText);
  m_toolbarLayout->insertWidget(0, m_label);

  // Toolbar
  m_actionCheck = m_toolbar->addAction(theme.getIcon("sync"), tr("Check new torrents"), this,
                                       []() { aggregator()->check(false); });
  m_actionDownload =
      m_toolbar->addAction(theme.getIcon("cloud_download"), tr("Download marked torrents"), this,
                           []() { aggregator()->downloadSelected(); });
  m_actionShowDiscarded = m_toolbar->addAction(theme.getIcon("lists"), tr("Show discarded"));
  m_actionShowDiscarded->setCheckable(true);
  connect(m_actionShowDiscarded, &QAction::toggled, this, &TorrentsWidget::refresh);
  m_toolbar->addAction(theme.getIcon("settings"), tr("Torrent settings"), this,
                       []() { SettingsDialog::show(mainWindow()); });

  // View
  m_view->setObjectName("torrentsView");
  m_view->setFrameShape(QFrame::Shape::NoFrame);
  m_view->setRootIsDecorated(false);
  m_view->setUniformRowHeights(true);
  m_view->setAlternatingRowColors(true);
  m_view->setAllColumnsShowFocus(true);
  m_view->setSelectionMode(QAbstractItemView::ExtendedSelection);
  m_view->setContextMenuPolicy(Qt::CustomContextMenu);
  m_view->setSortingEnabled(false);  // keep marked items on top, then the order of the feed
  m_view->setHeaderLabels({
      tr("Anime"),
      tr("Episode"),
      tr("Group"),
      tr("Video"),
      tr("Size"),
      tr("S"),
      tr("L"),
      tr("Title"),
      tr("Released"),
  });
  m_view->headerItem()->setToolTip(COLUMN_SEEDERS, tr("Seeders"));
  m_view->headerItem()->setToolTip(COLUMN_LEECHERS, tr("Leechers"));

  auto header = m_view->header();
  header->setStretchLastSection(false);
  header->setSectionResizeMode(QHeaderView::ResizeToContents);
  header->setSectionResizeMode(COLUMN_ANIME, QHeaderView::Interactive);
  header->setSectionResizeMode(COLUMN_TITLE, QHeaderView::Stretch);
  m_view->setColumnWidth(COLUMN_ANIME, 280);

  layout()->addWidget(m_view);

  connect(m_view, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* row, int column) {
    if (m_refreshing || column != COLUMN_ANIME) return;
    const auto link = row->data(COLUMN_ANIME, kLinkRole).toString();
    const bool checked = row->checkState(COLUMN_ANIME) == Qt::Checked;
    aggregator()->setItemState(link, checked ? ItemState::Selected : ItemState::Blank);
    updateActions();
    updateLabel();
  });

  connect(m_view, &QTreeWidget::itemDoubleClicked, this, [](QTreeWidgetItem* row) {
    aggregator()->download({row->data(COLUMN_ANIME, kLinkRole).toString()});
  });

  connect(m_view, &QWidget::customContextMenuRequested, this, &TorrentsWidget::showContextMenu);

  connect(aggregator(), &track::feed::Aggregator::feedChanged, this, &TorrentsWidget::refresh);
  connect(aggregator(), &track::feed::Aggregator::busyChanged, this,
          &TorrentsWidget::updateActions);

  connect(mainWindow()->searchBox(), &QLineEdit::returnPressed, this, [this]() {
    if (!isVisible()) return;
    search(mainWindow()->searchBox()->text());
  });

  refresh();

  // Show what's new on the first visit
  if (aggregator()->feed().items.empty() && !aggregator()->isBusy()) {
    aggregator()->check(false);
  }
}

void TorrentsWidget::search(const QString& text) {
  if (text.trimmed().isEmpty()) {
    aggregator()->check(false);
  } else {
    aggregator()->search(text);
  }
  updateLabel();
}

void TorrentsWidget::refresh() {
  m_refreshing = true;
  m_view->setUpdatesEnabled(false);

  const auto selectedBefore = selectedLinks();
  m_view->clear();

  const bool showDiscarded = m_actionShowDiscarded->isChecked();
  const auto disabledColor = palette().color(QPalette::Disabled, QPalette::Text);
  const QLocale locale;

  for (const auto& item : aggregator()->feed().items) {
    if (item.state == ItemState::DiscardedHidden) continue;
    if (item.isDiscarded() && !showDiscarded) continue;

    auto row = new QTreeWidgetItem(m_view);
    const auto link = QString::fromStdString(item.link);
    row->setData(COLUMN_ANIME, kLinkRole, link);

    const auto* anime = anime::db.item(item.episode.animeId());
    const auto parsedTitle = QString::fromStdString(item.episode.element(anitomy::ElementKind::Title));

    row->setText(COLUMN_ANIME, anime ? QString::fromStdString(anime::preferredTitle(*anime))
                                     : parsedTitle);
    row->setText(COLUMN_EPISODE, episodeText(item));
    row->setText(COLUMN_GROUP,
                 QString::fromStdString(item.episode.element(anitomy::ElementKind::ReleaseGroup)));
    row->setText(COLUMN_VIDEO, QString::fromStdString(
                                   item.episode.element(anitomy::ElementKind::VideoResolution)));
    if (item.file_size > 0) {
      row->setText(COLUMN_SIZE, locale.formattedDataSize(static_cast<qint64>(item.file_size), 1,
                                                         QLocale::DataSizeIecFormat));
    }
    if (item.seeders) row->setText(COLUMN_SEEDERS, QString::number(*item.seeders));
    if (item.leechers) row->setText(COLUMN_LEECHERS, QString::number(*item.leechers));
    row->setText(COLUMN_TITLE, QString::fromStdString(item.title));
    row->setText(COLUMN_DATE, dateText(item.pub_date));

    for (const auto column : {COLUMN_SIZE, COLUMN_SEEDERS, COLUMN_LEECHERS}) {
      row->setTextAlignment(column, Qt::AlignRight | Qt::AlignVCenter);
    }

    // Tooltips
    auto tooltip = QString::fromStdString(item.title);
    if (!item.description.empty()) tooltip += u"\n"_s + QString::fromStdString(item.description);
    if (const auto state = stateText(item); !state.isEmpty()) tooltip += u"\n\n"_s + state;
    for (int column = 0; column < COLUMN_COUNT; ++column) row->setToolTip(column, tooltip);
    if (!anime) {
      row->setToolTip(COLUMN_ANIME, parsedTitle.isEmpty() ? tr("Not recognized")
                                                          : tr("Not in your anime list"));
    }

    // Check state
    auto flags = row->flags() | Qt::ItemIsUserCheckable;
    if (item.state == ItemState::DiscardedInactive) flags &= ~Qt::ItemIsUserCheckable;
    row->setFlags(flags);
    row->setCheckState(COLUMN_ANIME,
                       item.state == ItemState::Selected ? Qt::Checked : Qt::Unchecked);

    // Style
    if (!anime) {
      auto font = row->font(COLUMN_ANIME);
      font.setItalic(true);
      row->setFont(COLUMN_ANIME, font);
    }
    if (item.new_episode && anime) {
      auto font = row->font(COLUMN_EPISODE);
      font.setBold(true);
      row->setFont(COLUMN_EPISODE, font);
    }
    if (item.isDiscarded()) {
      for (int column = 0; column < COLUMN_COUNT; ++column) {
        row->setForeground(column, disabledColor);
      }
    }

    if (selectedBefore.contains(link)) row->setSelected(true);
  }

  m_view->setUpdatesEnabled(true);
  m_refreshing = false;

  updateActions();
  updateLabel();
}

void TorrentsWidget::showContextMenu() {
  const auto links = selectedLinks();
  if (links.isEmpty()) return;

  const auto* item = findItem(links.front());
  if (!item) return;

  auto menu = new QMenu(this);
  menu->setAttribute(Qt::WA_DeleteOnClose);

  menu->addAction(theme.getIcon("cloud_download"),
                  links.size() > 1 ? tr("Download %1 torrents").arg(links.size()) : tr("Download"),
                  this, [links]() { aggregator()->download(links); });

  if (links.size() == 1) {
    const auto link = QString::fromStdString(item->link);
    const auto infoLink = item->info_link.empty() ? link : QString::fromStdString(item->info_link);
    const auto magnetLink = QString::fromStdString(item->magnet_link);

    menu->addAction(theme.getIcon("open_in_new"), tr("Open info page"), this,
                    [infoLink]() { QDesktopServices::openUrl(QUrl{infoLink}); });
    menu->addAction(theme.getIcon("content_copy"), tr("Copy link"), this,
                    [link]() { QApplication::clipboard()->setText(link); });
    if (!magnetLink.isEmpty()) {
      menu->addAction(theme.getIcon("content_copy"), tr("Copy magnet link"), this,
                      [magnetLink]() { QApplication::clipboard()->setText(magnetLink); });
    }
  }

  menu->addSeparator();

  menu->addAction(theme.getIcon("check"), tr("Mark for download"), this, [this, links]() {
    for (const auto& link : links) aggregator()->setItemState(link, ItemState::Selected);
    refresh();
  });
  menu->addAction(theme.getIcon("delete"), tr("Discard"), this, [this, links]() {
    for (const auto& link : links) aggregator()->setItemState(link, ItemState::DiscardedNormal);
    refresh();
  });

  const int animeId = item->episode.animeId();
  if (const auto* anime = anime::db.item(animeId); anime && links.size() == 1) {
    const auto title = QString::fromStdString(anime::preferredTitle(*anime));
    const auto group = item->episode.element(anitomy::ElementKind::ReleaseGroup);
    const auto resolution = item->episode.element(anitomy::ElementKind::VideoResolution);

    menu->addSeparator();

    if (!group.empty()) {
      const auto text =
          resolution.empty()
              ? tr("Prefer %1 for %2").arg(QString::fromStdString(group), title)
              : tr("Prefer %1 (%2) for %3")
                    .arg(QString::fromStdString(group), QString::fromStdString(resolution), title);
      menu->addAction(menuText(text), this, [animeId, group, resolution]() {
        auto filters = aggregator()->filters();
        track::feed::setFansubFilter(filters, animeId, group, resolution);
        aggregator()->setFilters(filters);
        aggregator()->reexamine();
      });
    }

    menu->addAction(menuText(tr("Discard all torrents of %1").arg(title)), this, [animeId]() {
      auto filters = aggregator()->filters();
      if (track::feed::addDiscardFilter(filters, animeId)) {
        aggregator()->setFilters(filters);
        aggregator()->reexamine();
      }
    });

    menu->addAction(theme.getIcon("search"), menuText(tr("Search torrents for %1").arg(title)),
                    this, [this, title]() {
                      mainWindow()->searchBox()->setText(title);
                      search(title);
                    });
  }

  menu->popup(QCursor::pos());
}

void TorrentsWidget::updateActions() {
  const bool busy = aggregator()->isBusy();
  const bool anySelected = std::ranges::any_of(aggregator()->feed().items, [](const Item& item) {
    return item.state == ItemState::Selected;
  });

  m_actionCheck->setEnabled(!busy);
  m_actionDownload->setEnabled(!busy && anySelected);
}

void TorrentsWidget::updateLabel() {
  const auto& feed = aggregator()->feed();
  const auto query = aggregator()->query();
  const auto host = QUrl{feed.url}.host();

  QString text;
  if (!query.isEmpty()) {
    text = tr("Search results for <b>%1</b>").arg(query.toHtmlEscaped());
  } else if (!host.isEmpty()) {
    text = tr("New torrents from <b>%1</b>").arg(host.toHtmlEscaped());
  }

  if (!feed.items.empty()) {
    const auto marked = std::ranges::count_if(
        feed.items, [](const Item& item) { return item.state == ItemState::Selected; });
    text += tr(" &middot; %1 marked of %2").arg(marked).arg(feed.items.size());
  }

  m_label->setText(text);
}

QStringList TorrentsWidget::selectedLinks() const {
  QStringList links;
  for (const auto* row : m_view->selectedItems()) {
    links.append(row->data(COLUMN_ANIME, kLinkRole).toString());
  }
  return links;
}

}  // namespace gui
