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
class QLabel;
class QLineEdit;
class QSpinBox;
class QWidget;

namespace gui {

class SettingsPageKodi final : public SettingsPage {
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(SettingsPageKodi)

public:
  SettingsPageKodi(Ui::SettingsDialog* ui, QDialog* dialog);
  ~SettingsPageKodi() override = default;

  QWidget* widget() const;

  void load() override;
  void apply() const override;

private:
  void testConnection();

  QWidget* m_widget = nullptr;
  mutable QWidget* m_page = nullptr;  // m_widget in a scroll area
  QCheckBox* m_enabled = nullptr;
  QLineEdit* m_host = nullptr;
  QSpinBox* m_port = nullptr;
  QLineEdit* m_username = nullptr;
  QLineEdit* m_password = nullptr;
  QCheckBox* m_detection = nullptr;
  QCheckBox* m_playEpisodes = nullptr;
  QLabel* m_status = nullptr;
};

}  // namespace gui
