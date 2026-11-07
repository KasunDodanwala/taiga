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

class QButtonGroup;
class QCheckBox;
class QLabel;
class QLineEdit;
class QRadioButton;
class QSpinBox;
class QWidget;

namespace gui {

class SettingsPageMediaServer final : public SettingsPage {
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(SettingsPageMediaServer)

public:
  SettingsPageMediaServer(Ui::SettingsDialog* ui, QDialog* dialog);
  ~SettingsPageMediaServer() override = default;

  QWidget* widget() const;

  void load() override;
  void apply() const override;

private:
  void updateStates();
  void updateStatus();

  QWidget* m_widget = nullptr;
  mutable QWidget* m_page = nullptr;  // m_widget in a scroll area
  QCheckBox* m_enabled = nullptr;
  QLineEdit* m_name = nullptr;
  QSpinBox* m_port = nullptr;
  QCheckBox* m_virtualFolders = nullptr;
  QCheckBox* m_detection = nullptr;
  QCheckBox* m_localOnly = nullptr;
  QButtonGroup* m_transcoding = nullptr;
  QRadioButton* m_transcodeNone = nullptr;
  QRadioButton* m_transcodeSoftware = nullptr;
  QRadioButton* m_transcodeHardware = nullptr;
  QLineEdit* m_vaapiDevice = nullptr;
  QSpinBox* m_bitrate = nullptr;
  QCheckBox* m_burnSubtitles = nullptr;
  QLabel* m_status = nullptr;
};

}  // namespace gui
