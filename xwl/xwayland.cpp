/********************************************************************
 KWin - the KDE window manager
 This file is part of the KDE project.

Copyright 2014 Martin Gräßlin <mgraesslin@kde.org>
Copyright 2019 Roman Gilg <subdiff@gmail.com>
Copyright (C) 2020 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*********************************************************************/
#include "xwayland.h"
#include "databridge.h"

#include "main_wayland.h"
#include "utils.h"
#include "wayland_server.h"
#include "xcbutils.h"
#include "xwayland_logging.h"

#include <KLocalizedString>
#include <KSelectionOwner>

#include <QAbstractEventDispatcher>
#include <QFile>
#include <QFutureWatcher>
#include <QtConcurrentRun>

// system
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#if HAVE_SYS_PROCCTL_H
#include <unistd.h>
#endif

#include <sys/socket.h>
#include <iostream>

static void readDisplay(int pipe)
{
    QFile readPipe;
    if (!readPipe.open(pipe, QIODevice::ReadOnly)) {
        std::cerr << "FATAL ERROR failed to open pipe to start X Server" << std::endl;
        exit(1);
    }
    QByteArray displayNumber = readPipe.readLine();

    displayNumber.prepend(QByteArray(":"));
    displayNumber.remove(displayNumber.size() -1, 1);
    std::cout << "X-Server started on display " << displayNumber.constData() << std::endl;

    setenv("DISPLAY", displayNumber.constData(), true);

    // close our pipe
    close(pipe);
}

namespace KWin
{
namespace Xwl
{

Xwayland *s_self = nullptr;

Xwayland *Xwayland::self()
{
    return s_self;
}

Xwayland::Xwayland(ApplicationWaylandAbstract *app, QObject *parent)
    : XwaylandInterface(parent)
    , m_app(app)
{
    s_self = this;
}

Xwayland::~Xwayland()
{
    stop();
    s_self = nullptr;
}

QProcess *Xwayland::process() const
{
    return m_xwaylandProcess;
}

void Xwayland::start()
{
    int pipeFds[2];
    if (pipe(pipeFds) != 0) {
        std::cerr << "FATAL ERROR failed to create pipe to start Xwayland " << std::endl;
        Q_EMIT criticalError(1);
        return;
    }
    int sx[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sx) < 0) {
        std::cerr << "FATAL ERROR: failed to open socket to open XCB connection" << std::endl;
        Q_EMIT criticalError(1);
        return;
    }
    int fd = dup(sx[1]);
    if (fd < 0) {
        std::cerr << "FATAL ERROR: failed to open socket to open XCB connection" << std::endl;
        Q_EMIT criticalError(20);
        return;
    }

    const int waylandSocket = waylandServer()->createXWaylandConnection();
    if (waylandSocket == -1) {
        std::cerr << "FATAL ERROR: failed to open socket for Xwayland" << std::endl;
        Q_EMIT criticalError(1);
        return;
    }
    const int wlfd = dup(waylandSocket);
    if (wlfd < 0) {
        std::cerr << "FATAL ERROR: failed to open socket for Xwayland" << std::endl;
        Q_EMIT criticalError(20);
        return;
    }

    m_xcbConnectionFd = sx[0];
    m_displayFileDescriptor = pipeFds[0];

    m_xwaylandProcess = new Process(this);
    m_xwaylandProcess->setProcessChannelMode(QProcess::ForwardedErrorChannel);
    m_xwaylandProcess->setProgram(QStringLiteral("Xwayland"));
    QProcessEnvironment env = m_app->processStartupEnvironment();
    env.insert("WAYLAND_SOCKET", QByteArray::number(wlfd));
    env.insert("EGL_PLATFORM", QByteArrayLiteral("DRM"));
    m_xwaylandProcess->setProcessEnvironment(env);
    m_xwaylandProcess->setArguments({QStringLiteral("-displayfd"),
                           QString::number(pipeFds[1]),
                           QStringLiteral("-rootless"),
                           QStringLiteral("-wm"),
                           QString::number(fd)});
    connect(m_xwaylandProcess, &QProcess::errorOccurred, this, &Xwayland::handleXwaylandError);
    connect(m_xwaylandProcess, &QProcess::started, this, &Xwayland::handleXwaylandStarted);
    connect(m_xwaylandProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &Xwayland::handleXwaylandFinished);
    m_xwaylandProcess->start();
    close(pipeFds[1]);
}

void Xwayland::stop()
{
    if (!m_xwaylandProcess) {
        return;
    }

    // If Xwayland has crashed, we must deactivate the socket notifier and ensure that no X11
    // events will be dispatched before blocking; otherwise we will simply hang...
    uninstallSocketNotifier();

    delete m_dataBridge;
    m_dataBridge = nullptr;

    destroyX11Connection();

    // When the Xwayland process is finally terminated, the finished() signal will be emitted,
    // however we don't actually want to process it anymore. Furthermore, we also don't really
    // want to handle any errors that may occur during the teardown.
    if (m_xwaylandProcess->state() != QProcess::NotRunning) {
        disconnect(m_xwaylandProcess, nullptr, this, nullptr);
        m_xwaylandProcess->terminate();
        m_xwaylandProcess->waitForFinished(5000);
    }
    delete m_xwaylandProcess;
    m_xwaylandProcess = nullptr;

    waylandServer()->destroyXWaylandConnection(); // This one must be destroyed last!
}

void Xwayland::dispatchEvents()
{
    xcb_connection_t *connection = kwinApp()->x11Connection();
    if (!connection) {
        qCWarning(KWIN_XWL, "Attempting to dispatch X11 events with no connection");
        return;
    }

    const int connectionError = xcb_connection_has_error(connection);
    if (connectionError) {
        qCWarning(KWIN_XWL, "The X11 connection broke (error %d)", connectionError);
        stop();
        return;
    }

    while (xcb_generic_event_t *event = xcb_poll_for_event(connection)) {
        long result = 0;
        QAbstractEventDispatcher *dispatcher = QCoreApplication::eventDispatcher();
        dispatcher->filterNativeEvent(QByteArrayLiteral("xcb_generic_event_t"), event, &result);
        free(event);
    }

    xcb_flush(connection);
}

void Xwayland::installSocketNotifier()
{
    const int fileDescriptor = xcb_get_file_descriptor(kwinApp()->x11Connection());

    m_socketNotifier = new QSocketNotifier(fileDescriptor, QSocketNotifier::Read, this);
    connect(m_socketNotifier, &QSocketNotifier::activated, this, &Xwayland::dispatchEvents);

    QAbstractEventDispatcher *dispatcher = QCoreApplication::eventDispatcher();
    connect(dispatcher, &QAbstractEventDispatcher::aboutToBlock, this, &Xwayland::dispatchEvents);
    connect(dispatcher, &QAbstractEventDispatcher::awake, this, &Xwayland::dispatchEvents);
}

void Xwayland::uninstallSocketNotifier()
{
    QAbstractEventDispatcher *dispatcher = QCoreApplication::eventDispatcher();
    disconnect(dispatcher, &QAbstractEventDispatcher::aboutToBlock, this, &Xwayland::dispatchEvents);
    disconnect(dispatcher, &QAbstractEventDispatcher::awake, this, &Xwayland::dispatchEvents);

    delete m_socketNotifier;
    m_socketNotifier = nullptr;
}

void Xwayland::handleXwaylandStarted()
{
    QFutureWatcher<void> *watcher = new QFutureWatcher<void>(this);
    connect(watcher, &QFutureWatcher<void>::finished, this, &Xwayland::continueStartupWithX);
    connect(watcher, &QFutureWatcher<void>::finished, watcher, &QFutureWatcher<void>::deleteLater);
    watcher->setFuture(QtConcurrent::run(readDisplay, m_displayFileDescriptor));
}

void Xwayland::handleXwaylandFinished(int exitCode)
{
    qCDebug(KWIN_XWL) << "Xwayland process has quit with exit code" << exitCode;

    // The Xwayland server has crashed... At this moment we have two choices either restart
    // Xwayland or shut down all X11 related components. For now, we do the latter, we simply
    // tear down everything that has any connection to X11.
    stop();
}

void Xwayland::handleXwaylandError(QProcess::ProcessError error)
{
    switch (error) {
    case QProcess::FailedToStart:
        qCWarning(KWIN_XWL) << "Xwayland process failed to start";
        emit criticalError(1);
        return;
    case QProcess::Crashed:
        qCWarning(KWIN_XWL) << "Xwayland process crashed. Shutting down X11 components";
        break;
    case QProcess::Timedout:
        qCWarning(KWIN_XWL) << "Xwayland operation timed out";
        break;
    case QProcess::WriteError:
    case QProcess::ReadError:
        qCWarning(KWIN_XWL) << "An error occurred while communicating with Xwayland";
        break;
    case QProcess::UnknownError:
        qCWarning(KWIN_XWL) << "An unknown error has occurred in Xwayland";
        break;
    }
}

void Xwayland::createX11Connection()
{
    xcb_connection_t *connection = xcb_connect_to_fd(m_xcbConnectionFd, nullptr);
    if (!connection) {
        return;
    }

    xcb_screen_t *screen = xcb_setup_roots_iterator(xcb_get_setup(connection)).data;
    Q_ASSERT(screen);

    m_app->setX11Connection(connection);
    m_app->setX11DefaultScreen(screen);
    m_app->setX11ScreenNumber(0);
    m_app->setX11RootWindow(screen->root);

    m_app->createAtoms();
    m_app->installNativeX11EventFilter();

    installSocketNotifier();

    // Note that it's very important to have valid x11RootWindow(), x11ScreenNumber(), and
    // atoms when the rest of kwin is notified about the new X11 connection.
    emit m_app->x11ConnectionChanged();
}

void Xwayland::destroyX11Connection()
{
    if (!m_app->x11Connection()) {
        return;
    }

    emit m_app->x11ConnectionAboutToBeDestroyed();

    Xcb::setInputFocus(XCB_INPUT_FOCUS_POINTER_ROOT);
    m_app->destroyAtoms();
    m_app->removeNativeX11EventFilter();

    xcb_disconnect(m_app->x11Connection());
    m_xcbConnectionFd = -1;

    m_app->setX11Connection(nullptr);
    m_app->setX11DefaultScreen(nullptr);
    m_app->setX11ScreenNumber(-1);
    m_app->setX11RootWindow(XCB_WINDOW_NONE);

    emit m_app->x11ConnectionChanged();
}

void Xwayland::continueStartupWithX()
{
    createX11Connection();
    xcb_connection_t *xcbConn = m_app->x11Connection();
    if (!xcbConn) {
        // about to quit
        Q_EMIT criticalError(1);
        return;
    }

    // create selection owner for WM_S0 - magic X display number expected by XWayland
    KSelectionOwner owner("WM_S0", xcbConn, m_app->x11RootWindow());
    owner.claim(true);

    m_dataBridge = new DataBridge;

    auto env = m_app->processStartupEnvironment();
    env.insert(QStringLiteral("DISPLAY"), QString::fromUtf8(qgetenv("DISPLAY")));
    m_app->setProcessStartupEnvironment(env);

    emit started();

    Xcb::sync(); // Trigger possible errors, there's still a chance to abort
}

DragEventReply Xwayland::dragMoveFilter(Toplevel *target, const QPoint &pos)
{
    if (!m_dataBridge) {
        return DragEventReply::Wayland;
    }
    return m_dataBridge->dragMoveFilter(target, pos);
}

} // namespace Xwl
} // namespace KWin
