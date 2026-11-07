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

#include <vector>

#include "gui/settings/settings_page.hpp"
#include "track/feed_filter.hpp"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QSpinBox;
class QTabWidget;
class QToolButton;
class QTreeWidget;
class QWidget;

namespace gui {

// "Torrents" page with Discovery, Downloads and Filters tabs, like in Taiga 1.x
class SettingsPageTorrents final : public SettingsPage {
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(SettingsPageTorrents)

public:
  SettingsPageTorrents(Ui::SettingsDialog* ui, QDialog* dialog);
  ~SettingsPageTorrents() override = default;

  QWidget* widget() const;
  bool selectTab(const QString& name);

  void load() override;
  void apply() const override;

protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

private:
  QWidget* createDiscoveryTab();
  QWidget* createDownloadsTab();
  QWidget* createFiltersTab();

  void refreshFilterList();
  void syncFilterStates() const;
  void updateFilterButtons();
  void addFilter();
  void editFilter();
  void deleteFilter();
  void moveFilter(int offset);
  void importFilters();
  void exportFilters();
  void resetFilters();
  void testConnection();

  QTabWidget* m_tabs = nullptr;

  // Discovery
  QLineEdit* m_feedUrl = nullptr;
  QLineEdit* m_searchUrl = nullptr;
  QCheckBox* m_autoCheck = nullptr;
  QSpinBox* m_autoCheckInterval = nullptr;
  QComboBox* m_newAction = nullptr;

  // Downloads
  QLineEdit* m_clientUrl = nullptr;
  QLineEdit* m_clientUsername = nullptr;
  QLineEdit* m_clientPassword = nullptr;
  QLabel* m_clientStatus = nullptr;
  QLineEdit* m_downloadPath = nullptr;
  QCheckBox* m_createSubfolder = nullptr;
  QLineEdit* m_category = nullptr;
  QCheckBox* m_useMagnet = nullptr;

  // Filters
  QCheckBox* m_filterEnabled = nullptr;
  QTreeWidget* m_filterList = nullptr;
  QWidget* m_filterToolbar = nullptr;
  QToolButton* m_deleteFilterButton = nullptr;
  QToolButton* m_moveUpButton = nullptr;
  QToolButton* m_moveDownButton = nullptr;
  mutable std::vector<track::feed::Filter> m_filters;
};

}  // namespace gui
