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

#include "application.hpp"

#include <QDir>
#include <QFileInfo>
#include <QLocalSocket>
#include <QMouseEvent>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QTranslator>
#include <chrono>
#include <format>

#include "base/log.hpp"
#include "base/string.hpp"
#include "gui/main/main_window.hpp"
#include "gui/settings/settings_dialog.hpp"
#include "gui/utils/image_provider.hpp"
#include "gui/utils/theme.hpp"
#include "media/anime_db.hpp"
#include "media/anime_history.hpp"
#include "media_server/server.hpp"
#include "share/discord.hpp"
#include "sync/queue.hpp"
#include "taiga/config.h"
#include "taiga/path.hpp"
#include "taiga/settings.hpp"
#include "taiga/version.hpp"
#include "track/feed_aggregator.hpp"
#include "track/media.hpp"
#include "track/update_session.hpp"

namespace taiga {

Application::Application(int argc, char* argv[]) : QApplication(argc, argv) {
  setApplicationName("taiga");
  setApplicationDisplayName("Taiga");
  setApplicationVersion(QString::fromStdString(taiga::version().to_string()));
  setOrganizationDomain("taiga.moe");
  setOrganizationName("erengy");
}

Application::~Application() {
  if (window_) {
    window_->hide();
  }
}

int Application::run() {
  parseCommandLine();

  initLogger();

  const auto version = taiga::version().to_string();
  const auto fileInfo = QFileInfo{QCoreApplication::applicationFilePath()};
  const auto lastModified = fileInfo.lastModified().toString(Qt::DateFormat::ISODate);
  qDebug() << u"Version %1 (%2)"_s.arg(version).arg(lastModified);
  if (!parser_.optionNames().isEmpty()) {
    qDebug() << "Options:" << parser_.optionNames().join(", ");
  }

  if (activatePreviousInstance()) {
    qDebug() << "Another instance of Taiga is running.";
    return 0;
  }

  // Nobody answered, so a leftover socket file can only belong to an instance that didn't exit
  // cleanly (e.g. killed on logout).
  QLocalServer::removeServer(TAIGA_APP_NAME);
  connect(&local_server_, &QLocalServer::newConnection, this, &Application::onNewConnection);
  if (!local_server_.listen(TAIGA_APP_NAME)) {
    qWarning() << "Could not listen for other instances:" << local_server_.errorString();
  }

  taiga::settings.init();
  anime::db.init();
  anime::history.init();
  sync::queue.init();
  track::media::detection()->init();
  track::updateSession()->init();
  track::feed::aggregator()->init();
  media_server::server()->init();
  share::discord::presence()->init();
  gui::imageProvider.init();

  gui::theme.initStyle();
  setWindowIcon(gui::theme.getIcon("taiga", "png"));

  QTranslator translator;
  if (translator.load(QLocale::system(), "taiga", "_", ":/i18n")) {
    installTranslator(&translator);
  }

  window_ = new gui::MainWindow();
  window_->init();

  // While the main window is in the system tray, closing a dialog shouldn't quit the application.
  if (taiga::settings.appCloseToTray() && QSystemTrayIcon::isSystemTrayAvailable()) {
    setQuitOnLastWindowClosed(false);
  }

#ifdef Q_OS_WINDOWS
  // Delay showing the window to avoid a white flash.
  window_->setWindowOpacity(0.0);
  window_->show();
  QTimer::singleShot(std::chrono::milliseconds(50), window_, [this]() {
    if (window_) {
      window_->setWindowOpacity(1.0);
    }
  });
#else
  // Starting minimized (e.g. on login) keeps Taiga in the system tray, detecting media.
  if (!options_.minimized || !QSystemTrayIcon::isSystemTrayAvailable()) {
    window_->show();
  }
#endif

  if (!options_.page.isEmpty()) {
    openPage(options_.page);
  }

  // Taiga usually starts with the session and runs in the background, so the anime list has to be
  // up to date (e.g. after a reboot) before episodes are detected and progress is compared.
  if (taiga::settings.syncEnabled()) {
    QTimer::singleShot(std::chrono::seconds(3), window_, [this]() {
      if (window_) QMetaObject::invokeMethod(window_, "synchronize");
    });
  }

  return QApplication::exec();
}

bool Application::isDebug() const {
  return options_.debug;
}

bool Application::isVerbose() const {
  return options_.verbose;
}

gui::MainWindow* Application::mainWindow() const {
  return window_.get();
}

bool Application::notify(QObject* receiver, QEvent* event) {
  // Restrict double-click events to left mouse button.
  if (event->type() == QEvent::MouseButtonDblClick &&
      static_cast<QMouseEvent*>(event)->button() != Qt::LeftButton) {
    return true;
  }

  return QApplication::notify(receiver, event);
}

// A running instance listens on a local socket, so connecting to it is how we detect it. (A shared
// memory segment, which Taiga used before, can outlive a process that was killed on Linux, and then
// Taiga would silently refuse to start.)
bool Application::activatePreviousInstance() {
  QLocalSocket socket;
  socket.connectToServer(TAIGA_APP_NAME);
  if (!socket.waitForConnected(std::chrono::milliseconds(1000).count())) return false;

  // Forward the requested page (e.g. `taiga --page torrents`) to the running instance.
  if (!options_.page.isEmpty()) {
    socket.write(u"page:%1\n"_s.arg(options_.page).toUtf8());
    socket.waitForBytesWritten(std::chrono::milliseconds(1000).count());
  }
  socket.disconnectFromServer();
  return true;
}

void Application::initLogger() const {
  const auto directory = u"%1/logs"_s.arg(get_data_path());
  QDir().mkpath(directory);

  const auto date = QDate::currentDate().toString(Qt::DateFormat::ISODate);
  const auto path = u"%1/%2_%3.log"_s.arg(directory).arg(TAIGA_APP_NAME).arg(date);

  base::initLogging(path, options_.debug ? QtDebugMsg : QtWarningMsg);
}

void Application::onNewConnection() {
  while (auto socket = local_server_.nextPendingConnection()) {
    connect(socket, &QLocalSocket::readyRead, this, [this, socket]() {
      while (socket->canReadLine()) {
        const auto message = QString::fromUtf8(socket->readLine()).trimmed();
        if (message.startsWith(u"page:"_s)) openPage(message.mid(5));
      }
    });
    connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
  }
  if (window_) {
    window_->displayWindow();
  }
}

void Application::openPage(const QString& name) {
  using gui::MainWindowPage;

  // "settings" or "settings/<section>", e.g. "settings/media-server"
  if (name.section(u'/', 0, 0).compare(u"settings", Qt::CaseInsensitive) == 0) {
    if (!window_) return;
    window_->displayWindow();
    gui::SettingsDialog::show(window_, name.section(u'/', 1));
    return;
  }

  static const QMap<QString, MainWindowPage> pages{
      {u"search"_s, MainWindowPage::Search},     {u"list"_s, MainWindowPage::List},
      {u"history"_s, MainWindowPage::History},   {u"library"_s, MainWindowPage::Library},
      {u"torrents"_s, MainWindowPage::Torrents},
  };

  const auto it = pages.find(name.toLower());
  if (it == pages.end() || !window_) {
    qWarning() << "Unknown page:" << name;
    return;
  }

  window_->displayWindow();
  window_->navigateTo(*it);
}

void Application::parseCommandLine() {
  parser_.addOptions({
      {"debug", QCoreApplication::translate("main", "Enable debug mode")},
      {"minimized", QCoreApplication::translate("main", "Start minimized to the system tray")},
      {"page",
       QCoreApplication::translate(
           "main", "Open a page (search, list, history, library, torrents)"),
       QCoreApplication::translate("main", "name")},
      {"verbose", QCoreApplication::translate("main", "Enable verbose output")},
  });

  // This stops the current process in case of an error (e.g. an unknown option was passed).
  parser_.process(QApplication::arguments());

#ifdef _DEBUG
  options_.debug = true;
#else
  options_.debug = parser_.isSet("debug");
#endif
  options_.minimized = parser_.isSet("minimized");
  options_.page = parser_.value("page");
  options_.verbose = parser_.isSet("verbose");
}

}  // namespace taiga
