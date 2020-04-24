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
#include "tray/UserModel.h"

#include <QCursor>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QScreen>

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

Systray *Systray::_instance = nullptr;

Systray *Systray::instance()
{
    if (_instance == nullptr) {
        _instance = new Systray();
    }
    return _instance;
}

Systray::Systray()
    : _isOpen(false)
    , _syncIsPaused(false)
    , _trayEngine(new QQmlApplicationEngine(this))
{
    _trayEngine->addImportPath("qrc:/qml/theme");
    _trayEngine->addImageProvider("avatars", new ImageProvider);
    _trayEngine->rootContext()->setContextProperty("userModelBackend", UserModel::instance());
    _trayEngine->rootContext()->setContextProperty("appsMenuModelBackend", UserAppsModel::instance());
    _trayEngine->rootContext()->setContextProperty("systrayBackend", this);

    connect(UserModel::instance(), &UserModel::newUserSelected,
        this, &Systray::slotNewUserSelected);

    connect(AccountManager::instance(), &AccountManager::accountAdded,
        this, &Systray::showWindow);

}

void Systray::create()
{
    if (!AccountManager::instance()->accounts().isEmpty()) {
        _trayEngine->rootContext()->setContextProperty("activityModel", UserModel::instance()->currentActivityModel());
    }
    _trayEngine->load(QStringLiteral("qrc:/qml/src/gui/tray/Window.qml"));
    hideWindow();
    emit this->activated(QSystemTrayIcon::ActivationReason::Unknown);
}

void Systray::slotNewUserSelected()
{
    // Change ActivityModel
    _trayEngine->rootContext()->setContextProperty("activityModel", UserModel::instance()->currentActivityModel());

    // Rebuild App list
    UserAppsModel::instance()->buildAppList();
}

bool Systray::isOpen()
{
    return _isOpen;
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
        emit resumeSync();
    } else {
        _syncIsPaused = true;
        emit pauseSync();
    }
}

/********************************************************************************************/
/* Helper functions for cross-platform tray icon position and taskbar orientation detection */
/********************************************************************************************/

/// Return the current screen index based on curser position
int Systray::screenIndex()
{
    auto qPos = QCursor::pos();
    for (int i = 0; i < QGuiApplication::screens().count(); i++) {
        if (QGuiApplication::screens().at(i)->geometry().contains(qPos)) {
            return i;
        }
    }
    return 0;
}

// 0 = bottom, 1 = left, 2 = top, 3 = right
int Systray::taskbarOrientation()
{
// macOS: Always on top
#if defined(Q_OS_MACOS)
    return 2;
// Windows: Check registry for actual taskbar orientation
#elif defined(Q_OS_WIN)
    auto taskbarGeometry = Utility::registryGetKeyValue(HKEY_CURRENT_USER,
        "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StuckRects3",
        "Settings");
    switch (taskbarGeometry.toInt()) {
    // Yes this is somehow ugly, but as long as this is such a specific function, an extra enum may be too much
    // Mapping windows binary value (0 = left, 1 = top, 2 = right, 3 = bottom) to qml logic (0 = bottom, 1 = left...)
    case 0:
        return 1;
    case 1:
        return 2;
    case 2:
        return 3;
    case 3:
        return 0;
    default:
        return 0;
    }
// Else (generally linux DEs): fallback to cursor position nearest edge logic
#else
    return 0;
#endif
}

QRect Systray::taskbarRect()
{
#if defined(Q_OS_WIN)
    return Utility::getTaskbarDimensions();
#else
    return QRect(0,0,0,32);
#endif
}

/// Returns a QPoint that constitutes the center coordinate of the tray icon
QPoint Systray::calcTrayIconCenter()
{
// QSystemTrayIcon::geometry() is broken for ages on most Linux DEs (invalid geometry returned)
// thus we can use this only for Windows and macOS
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    auto trayIconCenter = this->geometry().center();
    return trayIconCenter;
#else
// On Linux, fall back to mouse position (assuming tray icon is activated by mouse click)
    return QCursor::pos();
    /*QScreen* trayScreen = nullptr;
    if (QGuiApplication::screens().count() > 1) {
        trayScreen = QGuiApplication::screens().at(screenIndex());
    } else {
        trayScreen = QGuiApplication::primaryScreen();
    }

    int screenWidth = trayScreen->geometry().width();
    int screenHeight = trayScreen->geometry().height();
    int availableWidth = trayScreen->availableGeometry().width();
    int availableHeight = trayScreen->availableGeometry().height();

    QPoint topRightDpiAware = QPoint();
    QPoint topLeftDpiAware = QPoint();
    if (this->geometry().left() == 0 || this->geometry().top() == 0) {
        // tray geometry is invalid - QT bug on some linux desktop environments
        // Use mouse position instead. Cringy, but should work for now
        auto test = (QCursor::pos() - QGuiApplication::screens().at(screenIndex())->geometry().topLeft());
        topRightDpiAware = (QCursor::pos() - QGuiApplication::screens().at(screenIndex())->geometry().topLeft()) / trayScreen->devicePixelRatio();
        topLeftDpiAware = (QCursor::pos() - QGuiApplication::screens().at(screenIndex())->geometry().topLeft()) / trayScreen->devicePixelRatio();
    } else {
        topRightDpiAware = this->geometry().topRight() / trayScreen->devicePixelRatio();
        topLeftDpiAware = this->geometry().topLeft() / trayScreen->devicePixelRatio();
    }

    // get x coordinate from top center point of tray icon
    int trayIconTopCenterX = (topRightDpiAware - ((topRightDpiAware - topLeftDpiAware) * 0.5)).x();

    if (availableHeight < screenHeight) {
        // taskbar is on top or bottom
        if (trayIconTopCenterX + (400 * 0.5) > availableWidth) {
            return availableWidth - 400 - 12;
        } else {
            return trayIconTopCenterX - (400 * 0.5);
        }
    } else {
        if (trayScreen->availableGeometry().x() > trayScreen->geometry().x()) {
            // on the left
            return (screenWidth - availableWidth) + 6;
        } else {
            // on the right
            return screenWidth - 400 - (screenWidth - availableWidth) - 6;
        }
    }*/
#endif
}
/*int Systray::calcTrayWindowY()
{
    QScreen* trayScreen = nullptr;
    if (QGuiApplication::screens().count() > 1) {
        trayScreen = QGuiApplication::screens().at(screenIndex());
    } else {
        trayScreen = QGuiApplication::primaryScreen();
    }
#ifdef Q_OS_OSX
    // macOS menu bar is always 22 (effective) pixels
    // don't use availableGeometry() here, because this also excludes the dock
    return trayScreen->geometry().topLeft().y()+22+6;
#else
    int screenHeight = trayScreen->geometry().height();
    int availableHeight = trayScreen->availableGeometry().height();

    QPoint topRightDpiAware = QPoint();
    QPoint topLeftDpiAware = QPoint();
    if (this->geometry().left() == 0 || this->geometry().top() == 0) {
        // tray geometry is invalid - QT bug on some linux desktop environments
        // Use mouse position instead. Cringy, but should work for now
        topRightDpiAware = QCursor::pos() / trayScreen->devicePixelRatio();
        topLeftDpiAware = QCursor::pos() / trayScreen->devicePixelRatio();
    } else {
        topRightDpiAware = this->geometry().topRight() / trayScreen->devicePixelRatio();
        topLeftDpiAware = this->geometry().topLeft() / trayScreen->devicePixelRatio();
    }
    // get y coordinate from top center point of tray icon
    int trayIconTopCenterY = (topRightDpiAware - ((topRightDpiAware - topLeftDpiAware) * 0.5)).y();

    if (availableHeight < screenHeight) {
        // taskbar is on top or bottom
        if (QCursor::pos().y() < (screenHeight / 2)) {
            // on top
            return (screenHeight - availableHeight) + 6;
        } else {
            // on bottom
            return screenHeight - 510 - (screenHeight - availableHeight) - 6;
        }
    } else {
        // on the left or right
        return (trayIconTopCenterY - 510 + 12);
    }
#endif
}*/

} // namespace OCC
