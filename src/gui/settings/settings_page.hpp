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

#include <QObject>

class QDialog;
class QWidget;

namespace Ui {
class SettingsDialog;
}

namespace gui {

class SettingsPage : public QObject {
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(SettingsPage)

public:
  SettingsPage(Ui::SettingsDialog* ui, QDialog* dialog);
  ~SettingsPage() override = default;

  virtual void load() = 0;
  virtual void apply() const = 0;

protected:
  // Puts a page that is built in code into a scroll area, so that its contents keep their size
  // (and wrapped text isn't cut off) when the dialog is smaller than the page.
  static QWidget* scrollable(QWidget* content);

  Ui::SettingsDialog* ui_;
  QDialog* dialog_;
};

}  // namespace gui
