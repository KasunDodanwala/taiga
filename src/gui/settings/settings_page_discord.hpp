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

#include "gui/settings/settings_page.hpp"

class QCheckBox;
class QWidget;

namespace gui {

class SettingsPageDiscord final : public SettingsPage {
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(SettingsPageDiscord)

public:
  SettingsPageDiscord(Ui::SettingsDialog* ui, QDialog* dialog);
  ~SettingsPageDiscord() override = default;

  QWidget* widget() const;

  void load() override;
  void apply() const override;

private:
  QWidget* m_widget = nullptr;
  mutable QWidget* m_page = nullptr;  // m_widget in a scroll area
  QCheckBox* m_enabled = nullptr;
  QCheckBox* m_showGroup = nullptr;
  QCheckBox* m_showTime = nullptr;
  QCheckBox* m_showUsername = nullptr;
  QCheckBox* m_showButton = nullptr;
};

}  // namespace gui
