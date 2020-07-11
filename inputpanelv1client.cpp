/********************************************************************
 KWin - the KDE window manager
 This file is part of the KDE project.

Copyright (C) 2020 Aleix Pol Gonzalez <aleixpol@kde.org>

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

#include "inputpanelv1client.h"
#include "deleted.h"
#include "wayland_server.h"
#include "workspace.h"
#include "abstract_wayland_output.h"
#include "platform.h"
#include <KWaylandServer/output_interface.h>
#include <KWaylandServer/seat_interface.h>
#include <KWaylandServer/surface_interface.h>

using namespace KWin;
using namespace KWaylandServer;

InputPanelV1Client::InputPanelV1Client(KWaylandServer::InputPanelSurfaceV1Interface *panelSurface)
    : WaylandClient(panelSurface->surface())
    , m_panelSurface(panelSurface)
{
    setSkipPager(true);
    setSkipTaskbar(true);
    setKeepAbove(true);
    setupCompositing();

    connect(surface(), &SurfaceInterface::aboutToBeDestroyed, this, &InputPanelV1Client::destroyClient);
    connect(surface(), &SurfaceInterface::sizeChanged, this, &InputPanelV1Client::reposition);
    connect(surface(), &SurfaceInterface::mapped, this, &InputPanelV1Client::updateDepth);

    connect(panelSurface, &KWaylandServer::InputPanelSurfaceV1Interface::topLevel, this, &InputPanelV1Client::showTopLevel);
    connect(panelSurface, &KWaylandServer::InputPanelSurfaceV1Interface::overlayPanel, this, &InputPanelV1Client::showOverlayPanel);
    connect(panelSurface, &KWaylandServer::InputPanelSurfaceV1Interface::destroyed, this, &InputPanelV1Client::destroyClient);
}

void InputPanelV1Client::showOverlayPanel()
{
    setOutput(nullptr);
    m_mode = Overlay;
    reposition();
}

void InputPanelV1Client::showTopLevel(KWaylandServer::OutputInterface *output, KWaylandServer::InputPanelSurfaceV1Interface::Position position)
{
    Q_UNUSED(position);
    m_mode = Toplevel;
    setOutput(output);
    reposition();
}

void KWin::InputPanelV1Client::reposition()
{
    switch (m_mode) {
        case Toplevel: {
            if (m_output) {
                const QSize panelSize = surface()->size();
                if (!panelSize.isValid() || panelSize.isEmpty())
                    return;

                const auto outputGeometry = m_output->geometry();
                QRect geo(outputGeometry.topLeft(), panelSize);
                geo.translate((outputGeometry.width() - panelSize.width())/2, outputGeometry.height() - panelSize.height());
                setFrameGeometry(geo);
            }
        }   break;
        case Overlay: {
            auto focusedField = waylandServer()->findClient(waylandServer()->seat()->focusedTextInputSurface());
            if (focusedField) {
                setFrameGeometry({focusedField->pos(), surface()->size()});
            }
        }   break;
    }
}

void InputPanelV1Client::setFrameGeometry(const QRect &geometry, ForceGeometry_t force)
{
    Q_UNUSED(force);
    if (m_frameGeometry != geometry) {
        m_frameGeometry = geometry;
        m_clientGeometry = geometry;

        emit frameGeometryChanged(this, m_frameGeometry);
        emit clientGeometryChanged(this, m_frameGeometry);
        emit bufferGeometryChanged(this, m_frameGeometry);

        setReadyForPainting();

        autoRaise();
    }
}

void InputPanelV1Client::destroyClient()
{
    markAsZombie();

    Deleted *deleted = Deleted::create(this);
    emit windowClosed(this, deleted);
    StackingUpdatesBlocker blocker(workspace());
    waylandServer()->removeClient(this);
    deleted->unrefWindow();

    delete this;
}

void InputPanelV1Client::debug(QDebug &stream) const
{
    stream << "InputPanelClient(" << static_cast<const void*>(this) << "," << resourceClass() << m_frameGeometry << ')';
}

NET::WindowType InputPanelV1Client::windowType(bool, int) const
{
    return NET::Utility;
}

void InputPanelV1Client::hideClient(bool hide)
{
    m_visible = !hide;
    if (hide) {
        workspace()->clientHidden(this);
        addWorkspaceRepaint(visibleRect());
        Q_EMIT windowHidden(this);
    } else {
        reposition();
        addRepaintFull();
        Q_EMIT windowShown(this);
    }
}

void InputPanelV1Client::setOutput(KWaylandServer::OutputInterface* outputIface)
{
    if (m_output) {
        disconnect(m_output->waylandOutput(), nullptr, this, nullptr);
    }

    m_output = waylandServer()->findOutput(outputIface);

    if (m_output) {
        connect(outputIface, &OutputInterface::physicalSizeChanged, this, &InputPanelV1Client::reposition);
        connect(outputIface, &OutputInterface::globalPositionChanged, this, &InputPanelV1Client::reposition);
        connect(outputIface, &OutputInterface::pixelSizeChanged, this, &InputPanelV1Client::reposition);
        connect(outputIface, &OutputInterface::scaleChanged, this, &InputPanelV1Client::reposition);
        connect(outputIface, &OutputInterface::currentModeChanged, this, &InputPanelV1Client::reposition);
        connect(outputIface, &OutputInterface::transformChanged, this, &InputPanelV1Client::reposition);
    }
}
