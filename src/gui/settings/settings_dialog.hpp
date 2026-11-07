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

#include <QDialog>
#include <QList>

namespace Ui {
class SettingsDialog;
}

namespace gui {

class SettingsPage;
class SettingsPageTorrents;

class SettingsDialog final : public QDialog {
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(SettingsDialog)

public:
  SettingsDialog(QWidget* parent);
  ~SettingsDialog() = default;

  // `section` is the name of an item in the tree, e.g. "Media server" or "media-server"
  static void show(QWidget* parent, const QString& section = {});

  void setCurrentSection(const QString& section);
  void accept() override;

private:
  Ui::SettingsDialog* ui_ = nullptr;
  QList<SettingsPage*> pages_;
  SettingsPageTorrents* torrentsPage_ = nullptr;
};

}  // namespace gui
