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

#include <QStringList>

#include "gui/common/page_widget.hpp"

class QAction;
class QLabel;
class QTreeWidget;
class QTreeWidgetItem;

namespace gui {

class TorrentsWidget final : public PageWidget {
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(TorrentsWidget)

public:
  enum Column {
    COLUMN_ANIME,
    COLUMN_EPISODE,
    COLUMN_GROUP,
    COLUMN_VIDEO,
    COLUMN_SIZE,
    COLUMN_SEEDERS,
    COLUMN_LEECHERS,
    COLUMN_TITLE,
    COLUMN_DATE,
    COLUMN_COUNT,
  };

  explicit TorrentsWidget(QWidget* parent);
  ~TorrentsWidget() override = default;

  void search(const QString& text);

private:
  void refresh();
  void showContextMenu();
  void updateActions();
  void updateLabel();
  QStringList selectedLinks() const;

  QLabel* m_label = nullptr;
  QTreeWidget* m_view = nullptr;
  QAction* m_actionCheck = nullptr;
  QAction* m_actionDownload = nullptr;
  QAction* m_actionShowDiscarded = nullptr;
  bool m_refreshing = false;
};

}  // namespace gui
