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

// The filter wizard and the condition dialog of Taiga 1.x (ui/dlg/dlg_feed_filter.cpp,
// ui/dlg/dlg_feed_condition.cpp), with the same pages, options and behavior.

#include <QDialog>
#include <QIcon>
#include <optional>

#include "track/feed_filter.hpp"

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QStackedWidget;
class QToolButton;
class QTreeWidget;

namespace gui {

// Funnel icons of Taiga 1.x: discard (cross), select (tick), prefer (plus), custom (pencil)
QIcon feedFilterIcon(track::feed::Action action);
QIcon feedFilterCustomIcon();

class FeedConditionDialog final : public QDialog {
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(FeedConditionDialog)

public:
  FeedConditionDialog(QWidget* parent, const track::feed::Condition& condition);

  // Returns the condition, or nothing if the dialog was canceled
  static std::optional<track::feed::Condition> edit(QWidget* parent,
                                                    const track::feed::Condition& condition = {});

  track::feed::Condition condition() const;

private:
  void chooseElement(track::feed::Element element);

  QComboBox* m_element = nullptr;
  QComboBox* m_operator = nullptr;
  QComboBox* m_value = nullptr;
};

class FeedFilterDialog final : public QDialog {
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(FeedFilterDialog)

public:
  FeedFilterDialog(QWidget* parent, const std::optional<track::feed::Filter>& filter);

  // Adds a new filter (starting with presets), or edits an existing one. Returns nothing if the
  // dialog was canceled.
  static std::optional<track::feed::Filter> edit(
      QWidget* parent, const std::optional<track::feed::Filter>& filter = std::nullopt);

  const track::feed::Filter& filter() const;

protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

private:
  QWidget* createPage0();
  QWidget* createPage1();
  QWidget* createPage2();

  void choosePage(int index);
  void buildFilterFromPreset();
  void buildFilterFromOptions();
  void buildFilterFromAnimeList();
  void refreshConditionList();
  void refreshAnimeList();
  void updateConditionButtons();
  void updateLimitText();
  void changeAction();
  void addCondition();
  void editCondition();
  void deleteCondition();
  void moveCondition(int offset);

  track::feed::Filter m_filter;
  std::vector<int> m_hiddenAnimeIds;  // limits for anime that are not in the list
  int m_currentPage = 0;

  QLabel* m_header = nullptr;
  QStackedWidget* m_pages = nullptr;
  QPushButton* m_backButton = nullptr;
  QPushButton* m_nextButton = nullptr;

  // Page 0
  QListWidget* m_presetList = nullptr;

  // Page 1
  QLineEdit* m_name = nullptr;
  QTreeWidget* m_conditionList = nullptr;
  QToolButton* m_deleteConditionButton = nullptr;
  QToolButton* m_moveUpButton = nullptr;
  QToolButton* m_moveDownButton = nullptr;
  QComboBox* m_match = nullptr;
  QComboBox* m_action = nullptr;
  QComboBox* m_option = nullptr;
  QLabel* m_optionLabel = nullptr;

  // Page 2
  QTreeWidget* m_animeList = nullptr;
  QLabel* m_limitLabel = nullptr;
};

}  // namespace gui
