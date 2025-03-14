/*
 * Copyright (C) by Cédric Bellegarde <gnumdk@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 */

#include "accountmanager.h"
#include "systray.h"
#include "theme.h"
#include "config.h"
#include "common/utility.h"
#include "tray/svgimageprovider.h"
#include "tray/usermodel.h"
#include "wheelhandler.h"
#include "tray/trayimageprovider.h"
#include "configfile.h"
#include "accessmanager.h"

#include <QCursor>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QVariantMap>
#include <QScreen>
#include <QMenu>
#include <QGuiApplication>

#ifdef USE_FDO_NOTIFICATIONS
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusPendingCall>
#define NOTIFICATIONS_SERVICE "org.freedesktop.Notifications"
#define NOTIFICATIONS_PATH "/org/freedesktop/Notifications"
#define NOTIFICATIONS_IFACE "org.freedesktop.Notifications"
#endif

namespace OCC {

Q_LOGGING_CATEGORY(lcSystray, "nextcloud.gui.systray")

Systray *Systray::_instance = nullptr;

Systray *Systray::instance()
{
    if (!_instance) {
        _instance = new Systray();
    }
    return _instance;
}

void Systray::setTrayEngine(QQmlApplicationEngine *trayEngine)
{
    _trayEngine = trayEngine;

    _trayEngine->setNetworkAccessManagerFactory(&_accessManagerFactory);

    _trayEngine->addImportPath("qrc:/qml/theme");
    _trayEngine->addImageProvider("avatars", new ImageProvider);
    _trayEngine->addImageProvider(QLatin1String("svgimage-custom-color"), new OCC::Ui::SvgImageProvider);
    _trayEngine->addImageProvider(QLatin1String("tray-image-provider"), new TrayImageProvider);
}

Systray::Systray()
    : QSystemTrayIcon(nullptr)
{
    qmlRegisterSingletonType<UserModel>("com.nextcloud.desktopclient", 1, 0, "UserModel",
        [](QQmlEngine *, QJSEngine *) -> QObject * {
            return UserModel::instance();
        }
    );

    qmlRegisterSingletonType<UserAppsModel>("com.nextcloud.desktopclient", 1, 0, "UserAppsModel",
        [](QQmlEngine *, QJSEngine *) -> QObject * {
            return UserAppsModel::instance();
        }
    );

    qmlRegisterSingletonType<Systray>("com.nextcloud.desktopclient", 1, 0, "Theme",
        [](QQmlEngine *, QJSEngine *) -> QObject * {
            return Theme::instance();
        }
    );

    qmlRegisterSingletonType<Systray>("com.nextcloud.desktopclient", 1, 0, "Systray",
        [](QQmlEngine *, QJSEngine *) -> QObject * {
            return Systray::instance();
        }
    );

    qmlRegisterType<WheelHandler>("com.nextcloud.desktopclient", 1, 0, "WheelHandler");

#ifndef Q_OS_MAC
    auto contextMenu = new QMenu();
    if (AccountManager::instance()->accounts().isEmpty()) {
        contextMenu->addAction(tr("Add account"), this, &Systray::openAccountWizard);
    } else {
        contextMenu->addAction(tr("Open main dialog"), this, &Systray::openMainDialog);
    }

    auto pauseAction = contextMenu->addAction(tr("Pause sync"), this, &Systray::slotPauseAllFolders);
    auto resumeAction = contextMenu->addAction(tr("Resume sync"), this, &Systray::slotUnpauseAllFolders);
    contextMenu->addAction(tr("Settings"), this, &Systray::openSettings);
    contextMenu->addAction(tr("Help"), this, &Systray::openHelp);
    contextMenu->addAction(tr("Exit %1").arg(Theme::instance()->appNameGUI()), this, &Systray::shutdown);
    setContextMenu(contextMenu);

    connect(contextMenu, &QMenu::aboutToShow, [=] {
        const auto folders = FolderMan::instance()->map();

        const auto allPaused = std::all_of(std::cbegin(folders), std::cend(folders), [](Folder *f) { return f->syncPaused(); });
        const auto pauseText = folders.size() > 1 ? tr("Pause sync for all") : tr("Pause sync");
        pauseAction->setText(pauseText);
        pauseAction->setVisible(!allPaused);
        pauseAction->setEnabled(!allPaused);

        const auto anyPaused = std::any_of(std::cbegin(folders), std::cend(folders), [](Folder *f) { return f->syncPaused(); });
        const auto resumeText = folders.size() > 1 ? tr("Resume sync for all") : tr("Resume sync");
        resumeAction->setText(resumeText);
        resumeAction->setVisible(anyPaused);
        resumeAction->setEnabled(anyPaused);
    });
#endif

    connect(UserModel::instance(), &UserModel::newUserSelected,
        this, &Systray::slotNewUserSelected);
    connect(UserModel::instance(), &UserModel::addAccount,
            this, &Systray::openAccountWizard);

    connect(AccountManager::instance(), &AccountManager::accountAdded,
        this, &Systray::showWindow);
}

void Systray::create()
{
    if (_trayEngine) {
        if (!AccountManager::instance()->accounts().isEmpty()) {
            _trayEngine->rootContext()->setContextProperty("activityModel", UserModel::instance()->currentActivityModel());
        }
        _trayEngine->load(QStringLiteral("qrc:/qml/src/gui/tray/Window.qml"));
    }
    hideWindow();
    emit activated(QSystemTrayIcon::ActivationReason::Unknown);

    const auto folderMap = FolderMan::instance()->map();
    for (const auto *folder : folderMap) {
        if (!folder->syncPaused()) {
            _syncIsPaused = false;
            break;
        }
    }
}

void Systray::createCallDialog(const Activity &callNotification)
{
    qCDebug(lcSystray) << "Starting a new call dialog for notification with id: " << callNotification._id << "with text: " << callNotification._subject;

    if (_trayEngine && !_callsAlreadyNotified.contains(callNotification._id)) {
        const QVariantMap talkNotificationData{
            {"conversationToken", callNotification._talkNotificationData.conversationToken},
            {"messageId", callNotification._talkNotificationData.messageId},
            {"messageSent", callNotification._talkNotificationData.messageSent},
            {"userAvatar", callNotification._talkNotificationData.userAvatar},
        };

        QVariantList links;
        for(const auto &link : callNotification._links) {
            links.append(QVariantMap{
                {"imageSource", link._imageSource},
                {"imageSourceHovered", link._imageSourceHovered},
                {"label", link._label},
                {"link", link._link},
                {"primary", link._primary},
                {"verb", link._verb},
            });
        }

        const QVariantMap initialProperties{
            {"talkNotificationData", talkNotificationData},
            {"links", links},
            {"subject", callNotification._subject},
            {"link", callNotification._link},
        };

        const auto callDialog = new QQmlComponent(_trayEngine, QStringLiteral("qrc:/qml/src/gui/tray/CallNotificationDialog.qml"));
        callDialog->createWithInitialProperties(initialProperties);

        _callsAlreadyNotified.insert(callNotification._id);
    }
}

void Systray::slotNewUserSelected()
{
    if (_trayEngine) {
        // Change ActivityModel
        _trayEngine->rootContext()->setContextProperty("activityModel", UserModel::instance()->currentActivityModel());
    }

    // Rebuild App list
    UserAppsModel::instance()->buildAppList();
}

void Systray::slotUnpauseAllFolders()
{
    setPauseOnAllFoldersHelper(false);
}

void Systray::slotPauseAllFolders()
{
    setPauseOnAllFoldersHelper(true);
}

void Systray::setPauseOnAllFoldersHelper(bool pause)
{
    // For some reason we get the raw pointer from Folder::accountState()
    // that's why we need a list of raw pointers for the call to contains
    // later on...
    const auto accounts = [=] {
        const auto ptrList = AccountManager::instance()->accounts();
        auto result = QList<AccountState *>();
        result.reserve(ptrList.size());
        std::transform(std::cbegin(ptrList), std::cend(ptrList), std::back_inserter(result), [](const AccountStatePtr &account) {
            return account.data();
        });
        return result;
    }();
    const auto folders = FolderMan::instance()->map();
    for (auto f : folders) {
        if (accounts.contains(f->accountState())) {
            f->setSyncPaused(pause);
            if (pause) {
                f->slotTerminateSync();
            }
        }
    }
}

bool Systray::isOpen()
{
    return _isOpen;
}

QString Systray::windowTitle() const
{
    return Theme::instance()->appNameGUI();
}

bool Systray::useNormalWindow() const
{
    if (!isSystemTrayAvailable()) {
        return true;
    }

    ConfigFile cfg;
    return cfg.showMainDialogAsNormalWindow();
}

Q_INVOKABLE void Systray::setOpened()
{
    _isOpen = true;
}

Q_INVOKABLE void Systray::setClosed()
{
    _isOpen = false;
}

void Systray::showMessage(const QString &title, const QString &message, MessageIcon icon)
{
#ifdef USE_FDO_NOTIFICATIONS
    if (QDBusInterface(NOTIFICATIONS_SERVICE, NOTIFICATIONS_PATH, NOTIFICATIONS_IFACE).isValid()) {
        const QVariantMap hints = {{QStringLiteral("desktop-entry"), LINUX_APPLICATION_ID}};
        QList<QVariant> args = QList<QVariant>() << APPLICATION_NAME << quint32(0) << APPLICATION_ICON_NAME
                                                 << title << message << QStringList() << hints << qint32(-1);
        QDBusMessage method = QDBusMessage::createMethodCall(NOTIFICATIONS_SERVICE, NOTIFICATIONS_PATH, NOTIFICATIONS_IFACE, "Notify");
        method.setArguments(args);
        QDBusConnection::sessionBus().asyncCall(method);
    } else
#endif
#ifdef Q_OS_OSX
        if (canOsXSendUserNotification()) {
        sendOsXUserNotification(title, message);
    } else
#endif
    {
        QSystemTrayIcon::showMessage(title, message, icon);
    }
}

void Systray::setToolTip(const QString &tip)
{
    QSystemTrayIcon::setToolTip(tr("%1: %2").arg(Theme::instance()->appNameGUI(), tip));
}

bool Systray::syncIsPaused()
{
    return _syncIsPaused;
}

void Systray::pauseResumeSync()
{
    if (_syncIsPaused) {
        _syncIsPaused = false;
        slotUnpauseAllFolders();
    } else {
        _syncIsPaused = true;
        slotPauseAllFolders();
    }
}

/********************************************************************************************/
/* Helper functions for cross-platform tray icon position and taskbar orientation detection */
/********************************************************************************************/

void Systray::positionWindow(QQuickWindow *window) const
{
    if (!useNormalWindow()) {
        window->setScreen(currentScreen());
        const auto position = computeWindowPosition(window->width(), window->height());
        window->setPosition(position);
    }
}

void Systray::forceWindowInit(QQuickWindow *window) const
{
    // HACK: At least on Windows, if the systray window is not shown at least once
    // it can prevent session handling to carry on properly, so we show/hide it here
    // this shouldn't flicker
    window->show();
    window->hide();
    
#ifdef Q_OS_MAC
    // On macOS we need to designate the tray window as visible on all spaces and
    // at the menu bar level, otherwise showing it can cause the current spaces to
    // change, or the window could be obscured by another window that shouldn't
    // normally cover a menu.
    OCC::setTrayWindowLevelAndVisibleOnAllSpaces(window);
#endif
}

void Systray::positionNotificationWindow(QQuickWindow *window) const
{
    if (!useNormalWindow()) {
        window->setScreen(currentScreen());
        if(geometry().isValid()) {
            // On OSes where the QSystemTrayIcon geometry method isn't borked, we can actually figure out where the system tray is located
            // We can therefore use our normal routines
            const auto position = computeNotificationPosition(window->width(), window->height());
            window->setPosition(position);
        } else if (QProcessEnvironment::systemEnvironment().contains(QStringLiteral("XDG_CURRENT_DESKTOP")) &&
                   (QProcessEnvironment::systemEnvironment().value(QStringLiteral("XDG_CURRENT_DESKTOP")).contains(QStringLiteral("GNOME")))) {
            // We can safely hardcode the top-right position for the notification when running GNOME
            const auto position = computeNotificationPosition(window->width(), window->height(), 0, NotificationPosition::TopRight);
            window->setPosition(position);
        } else {
            // For other DEs we play it safe and place the notification in the centre of the screen
            const QPoint windowAdjustment(window->geometry().width() / 2, window->geometry().height() / 2);
            const auto position = currentScreen()->geometry().center();// - windowAdjustment;
            window->setPosition(position);
        }
        // TODO: Get actual notification positions for the DEs
    }
}

QScreen *Systray::currentScreen() const
{
    const auto screen = QGuiApplication::screenAt(QCursor::pos());

    if(screen) {
        return screen;
    }
    // Didn't find anything matching the cursor position,
    // falling back to the primary screen
    return QGuiApplication::primaryScreen();
}

Systray::TaskBarPosition Systray::taskbarOrientation() const
{
// macOS: Always on top
#if defined(Q_OS_MACOS)
    return TaskBarPosition::Top;
// Windows: Check registry for actual taskbar orientation
#elif defined(Q_OS_WIN)
    auto taskbarPositionSubkey = QStringLiteral("SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StuckRects3");
    if (!Utility::registryKeyExists(HKEY_CURRENT_USER, taskbarPositionSubkey)) {
        // Windows 7
        taskbarPositionSubkey = QStringLiteral("SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StuckRects2");
    }
    if (!Utility::registryKeyExists(HKEY_CURRENT_USER, taskbarPositionSubkey)) {
        return TaskBarPosition::Bottom;
    }
    auto taskbarPosition = Utility::registryGetKeyValue(HKEY_CURRENT_USER, taskbarPositionSubkey, "Settings");
    switch (taskbarPosition.toInt()) {
    // Mapping windows binary value (0 = left, 1 = top, 2 = right, 3 = bottom) to qml logic (0 = bottom, 1 = left...)
    case 0:
        return TaskBarPosition::Left;
    case 1:
        return TaskBarPosition::Top;
    case 2:
        return TaskBarPosition::Right;
    case 3:
        return TaskBarPosition::Bottom;
    default:
        return TaskBarPosition::Bottom;
    }
// Probably Linux
#else
    const auto screenRect = currentScreenRect();
    const auto trayIconCenter = calcTrayIconCenter();

    const auto distBottom = screenRect.bottom() - trayIconCenter.y();
    const auto distRight = screenRect.right() - trayIconCenter.x();
    const auto distLeft = trayIconCenter.x() - screenRect.left();
    const auto distTop = trayIconCenter.y() - screenRect.top();

    const auto minDist = std::min({distRight, distTop, distBottom});

    if (minDist == distBottom) {
        return TaskBarPosition::Bottom;
    } else if (minDist == distLeft) {
        return TaskBarPosition::Left;
    } else if (minDist == distTop) {
        return TaskBarPosition::Top;
    } else {
        return TaskBarPosition::Right;
    }
#endif
}

// TODO: Get real taskbar dimensions Linux as well
QRect Systray::taskbarGeometry() const
{
#if defined(Q_OS_WIN)
    QRect tbRect = Utility::getTaskbarDimensions();
    //QML side expects effective pixels, convert taskbar dimensions if necessary
    auto pixelRatio = currentScreen()->devicePixelRatio();
    if (pixelRatio != 1) {
        tbRect.setHeight(tbRect.height() / pixelRatio);
        tbRect.setWidth(tbRect.width() / pixelRatio);
    }
    return tbRect;
#elif defined(Q_OS_MACOS)
    // Finder bar is always 22px height on macOS (when treating as effective pixels)
    const auto screenWidth = currentScreenRect().width();
    const auto statusBarHeight = static_cast<int>(OCC::statusBarThickness());
    return {0, 0, screenWidth, statusBarHeight};
#else
    if (taskbarOrientation() == TaskBarPosition::Bottom || taskbarOrientation() == TaskBarPosition::Top) {
        auto screenWidth = currentScreenRect().width();
        return {0, 0, screenWidth, 32};
    } else {
        auto screenHeight = currentScreenRect().height();
        return {0, 0, 32, screenHeight};
    }
#endif
}

QRect Systray::currentScreenRect() const
{
    const auto screen = currentScreen();
    Q_ASSERT(screen);
    return screen->geometry();
}

QPoint Systray::computeWindowReferencePoint() const
{
    constexpr auto spacing = 4;
    const auto trayIconCenter = calcTrayIconCenter();
    const auto taskbarRect = taskbarGeometry();
    const auto taskbarScreenEdge = taskbarOrientation();
    const auto screenRect = currentScreenRect();

    qCDebug(lcSystray) << "screenRect:" << screenRect;
    qCDebug(lcSystray) << "taskbarRect:" << taskbarRect;
    qCDebug(lcSystray) << "taskbarScreenEdge:" << taskbarScreenEdge;
    qCDebug(lcSystray) << "trayIconCenter:" << trayIconCenter;

    switch(taskbarScreenEdge) {
    case TaskBarPosition::Bottom:
        return {
            trayIconCenter.x(),
            screenRect.bottom() - taskbarRect.height() - spacing
        };
    case TaskBarPosition::Left:
        return {
            screenRect.left() + taskbarRect.width() + spacing,
            trayIconCenter.y()
        };
    case TaskBarPosition::Top:
        return {
            trayIconCenter.x(),
            screenRect.top() + taskbarRect.height() + spacing
        };
    case TaskBarPosition::Right:
        return {
            screenRect.right() - taskbarRect.width() - spacing,
            trayIconCenter.y()
        };
    }
    Q_UNREACHABLE();
}

QPoint Systray::computeNotificationReferencePoint(int spacing, NotificationPosition position) const
{
    auto trayIconCenter = calcTrayIconCenter();
    auto taskbarScreenEdge = taskbarOrientation();
    auto taskbarRect = taskbarGeometry();
    const auto screenRect = currentScreenRect();
    
    if(position == NotificationPosition::TopLeft) {
        taskbarScreenEdge = TaskBarPosition::Top;
        trayIconCenter = QPoint(0, 0);
        taskbarRect = QRect(0, 0, screenRect.width(), 32);
    } else if(position == NotificationPosition::TopRight) {
        taskbarScreenEdge = TaskBarPosition::Top;
        trayIconCenter = QPoint(screenRect.width(), 0);
        taskbarRect = QRect(0, 0, screenRect.width(), 32);
    } else if(position == NotificationPosition::BottomLeft) {
        taskbarScreenEdge = TaskBarPosition::Bottom;
        trayIconCenter = QPoint(0, screenRect.height());
        taskbarRect = QRect(0, 0, screenRect.width(), 32);
    } else if(position == NotificationPosition::BottomRight) {
        taskbarScreenEdge = TaskBarPosition::Bottom;
        trayIconCenter = QPoint(screenRect.width(), screenRect.height());
        taskbarRect = QRect(0, 0, screenRect.width(), 32);
    }

    qCDebug(lcSystray) << "screenRect:" << screenRect;
    qCDebug(lcSystray) << "taskbarRect:" << taskbarRect;
    qCDebug(lcSystray) << "taskbarScreenEdge:" << taskbarScreenEdge;
    qCDebug(lcSystray) << "trayIconCenter:" << trayIconCenter;

    switch(taskbarScreenEdge) {
    case TaskBarPosition::Bottom:
        return {
            trayIconCenter.x() < screenRect.center().x() ? screenRect.left() + spacing :  screenRect.right() - spacing,
            screenRect.bottom() - taskbarRect.height() - spacing
        };
    case TaskBarPosition::Left:
        return {
            screenRect.left() + taskbarRect.width() + spacing,
            trayIconCenter.y() < screenRect.center().y() ? screenRect.top() + spacing : screenRect.bottom() - spacing
        };
    case TaskBarPosition::Top:
        return {
            trayIconCenter.x() < screenRect.center().x() ? screenRect.left() + spacing :  screenRect.right() - spacing,
            screenRect.top() + taskbarRect.height() + spacing
        };
    case TaskBarPosition::Right:
        return {
            screenRect.right() - taskbarRect.width() - spacing,
            trayIconCenter.y() < screenRect.center().y() ? screenRect.top() + spacing : screenRect.bottom() - spacing
        };
    }
    Q_UNREACHABLE();
}

QRect Systray::computeWindowRect(int spacing, const QPoint &topLeft, const QPoint &bottomRight) const
{
    const auto screenRect = currentScreenRect();
    const auto rect = QRect(topLeft, bottomRight);
    auto offset = QPoint();

    if (rect.left() < screenRect.left()) {
        offset.setX(screenRect.left() - rect.left() + spacing);
    } else if (rect.right() > screenRect.right()) {
        offset.setX(screenRect.right() - rect.right() - spacing);
    }

    if (rect.top() < screenRect.top()) {
        offset.setY(screenRect.top() - rect.top() + spacing);
    } else if (rect.bottom() > screenRect.bottom()) {
        offset.setY(screenRect.bottom() - rect.bottom() - spacing);
    }

    return rect.translated(offset);
}

QPoint Systray::computeWindowPosition(int width, int height) const
{
    constexpr auto spacing = 4;
    const auto referencePoint = computeWindowReferencePoint();

    const auto taskbarScreenEdge = taskbarOrientation();
    const auto screenRect = currentScreenRect();

    const auto topLeft = [=]() {
        switch(taskbarScreenEdge) {
        case TaskBarPosition::Bottom:
            return referencePoint - QPoint(width / 2, height);
        case TaskBarPosition::Left:
            return referencePoint;
        case TaskBarPosition::Top:
            return referencePoint - QPoint(width / 2, 0);
        case TaskBarPosition::Right:
            return referencePoint - QPoint(width, 0);
        }
        Q_UNREACHABLE();
    }();
    const auto bottomRight = topLeft + QPoint(width, height);
    const auto windowRect = computeWindowRect(spacing, topLeft, bottomRight);

    qCDebug(lcSystray) << "taskbarScreenEdge:" << taskbarScreenEdge;
    qCDebug(lcSystray) << "screenRect:" << screenRect;
    qCDebug(lcSystray) << "windowRect (reference)" << QRect(topLeft, bottomRight);
    qCDebug(lcSystray) << "windowRect (adjusted)" << windowRect;

    return windowRect.topLeft();
}

QPoint Systray::computeNotificationPosition(int width, int height, int spacing, NotificationPosition position) const
{
    const auto referencePoint = computeNotificationReferencePoint(spacing, position);

    auto trayIconCenter = calcTrayIconCenter();
    auto taskbarScreenEdge = taskbarOrientation();
    const auto screenRect = currentScreenRect();
        
    if(position == NotificationPosition::TopLeft) {
        taskbarScreenEdge = TaskBarPosition::Top;
        trayIconCenter = QPoint(0, 0);
    } else if(position == NotificationPosition::TopRight) {
        taskbarScreenEdge = TaskBarPosition::Top;
        trayIconCenter = QPoint(screenRect.width(), 0);
    } else if(position == NotificationPosition::BottomLeft) {
        taskbarScreenEdge = TaskBarPosition::Bottom;
        trayIconCenter = QPoint(0, screenRect.height());
    } else if(position == NotificationPosition::BottomRight) {
        taskbarScreenEdge = TaskBarPosition::Bottom;
        trayIconCenter = QPoint(screenRect.width(), screenRect.height());
    }
        
    const auto topLeft = [=]() {
        switch(taskbarScreenEdge) {
        case TaskBarPosition::Bottom:
            return trayIconCenter.x() < screenRect.center().x() ? referencePoint - QPoint(0, height) : referencePoint - QPoint(width, height);
        case TaskBarPosition::Left:
            return trayIconCenter.y() < screenRect.center().y() ? referencePoint : referencePoint - QPoint(0, height);
        case TaskBarPosition::Top:
            return trayIconCenter.x() < screenRect.center().x() ? referencePoint : referencePoint - QPoint(width, 0);
        case TaskBarPosition::Right:
            return trayIconCenter.y() < screenRect.center().y() ? referencePoint - QPoint(width, 0) : QPoint(width, height);
        }
        Q_UNREACHABLE();
    }();
    const auto bottomRight = topLeft + QPoint(width, height);
    const auto windowRect = computeWindowRect(spacing, topLeft, bottomRight);

    qCDebug(lcSystray) << "taskbarScreenEdge:" << taskbarScreenEdge;
    qCDebug(lcSystray) << "screenRect:" << screenRect;
    qCDebug(lcSystray) << "windowRect (reference)" << QRect(topLeft, bottomRight);
    qCDebug(lcSystray) << "windowRect (adjusted)" << windowRect;
    qCDebug(lcSystray) << "referencePoint" << referencePoint;

    return windowRect.topLeft();
}

QPoint Systray::calcTrayIconCenter() const
{
    if(geometry().isValid()) {
        // QSystemTrayIcon::geometry() is broken for ages on most Linux DEs (invalid geometry returned)
        // thus we can use this only for Windows and macOS
        auto trayIconCenter = geometry().center();
        return trayIconCenter;
    }

    // On Linux, fall back to mouse position (assuming tray icon is activated by mouse click)
    return QCursor::pos(currentScreen());
}

AccessManagerFactory::AccessManagerFactory()
    : QQmlNetworkAccessManagerFactory()
{
}

QNetworkAccessManager* AccessManagerFactory::create(QObject *parent)
{
    const auto am = new AccessManager(parent);
    const auto diskCache = new QNetworkDiskCache(am);
    diskCache->setCacheDirectory("cacheDir");
    am->setCache(diskCache);
    return am;
}

} // namespace OCC
