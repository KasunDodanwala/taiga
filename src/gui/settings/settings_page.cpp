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
#include "settings_page.hpp"

#include <QDialog>
#include <QScrollArea>

namespace gui {

SettingsPage::SettingsPage(Ui::SettingsDialog* ui, QDialog* dialog)
    : QObject(dialog), ui_(ui), dialog_(dialog) {}

QWidget* SettingsPage::scrollable(QWidget* content) {
  auto area = new QScrollArea(content->parentWidget());
  area->setFrameShape(QFrame::NoFrame);
  area->setWidgetResizable(true);
  area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  area->setWidget(content);
  // Keep the dialog's background instead of the scroll area's
  area->viewport()->setAutoFillBackground(false);
  content->setAutoFillBackground(false);
  return area;
}

}  // namespace gui
