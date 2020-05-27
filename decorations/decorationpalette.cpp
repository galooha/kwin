/********************************************************************
 KWin - the KDE window manager
 This file is part of the KDE project.

Copyright 2014  Martin Gräßlin <mgraesslin@kde.org>
Copyright 2014  Hugo Pereira Da Costa <hugo.pereira@free.fr>
Copyright 2015  Mika Allan Rauhala <mika.allan.rauhala@gmail.com>

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

#include "decorationpalette.h"
#include "decorations_logging.h"

#include <KConfigGroup>
#include <KSharedConfig>
#include <KColorScheme>

#include <QPalette>
#include <QFileInfo>
#include <QStandardPaths>

namespace KWin
{
namespace Decoration
{

DecorationPalette::DecorationPalette(const QString &colorScheme)
    : m_colorScheme(QFileInfo(colorScheme).isAbsolute()
                    ? colorScheme
                    : QStandardPaths::locate(QStandardPaths::GenericConfigLocation, colorScheme))
{
    if (!m_colorScheme.startsWith(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)) && colorScheme == QStringLiteral("kdeglobals")) {
        // kdeglobals doesn't exist so create it. This is needed to monitor it using QFileSystemWatcher.
        auto config = KSharedConfig::openConfig(colorScheme, KConfig::SimpleConfig);
        KConfigGroup wmConfig(config, QStringLiteral("WM"));
        wmConfig.writeEntry("FakeEntryToKeepThisGroup", true);
        config->sync();

        m_colorScheme = QStandardPaths::locate(QStandardPaths::GenericConfigLocation, colorScheme);
    }
    m_watcher.addPath(m_colorScheme);
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, [this]() {
        m_watcher.addPath(m_colorScheme);
        update();
        emit changed();
    });

    update();
}

bool DecorationPalette::isValid() const
{
    return true;
}

QColor DecorationPalette::color(KDecoration2::ColorGroup group, KDecoration2::ColorRole role) const
{
    using KDecoration2::ColorRole;
    using KDecoration2::ColorGroup;

    switch (role) {
        case ColorRole::Frame:
            switch (group) {
                case ColorGroup::Active:
                    return KColorScheme(QPalette::Normal, KColorScheme::ColorSet::Header, m_colorSchemeConfig).shade(KColorScheme::ShadeRole::ShadowShade);
                case ColorGroup::Inactive:
                    return KColorScheme(QPalette::Inactive, KColorScheme::ColorSet::Header, m_colorSchemeConfig).shade(KColorScheme::ShadeRole::ShadowShade);
                default:
                    return QColor();
            }
        case ColorRole::TitleBar:
            switch (group) {
                case ColorGroup::Active:
                    return KColorScheme(QPalette::Normal, KColorScheme::ColorSet::Header, m_colorSchemeConfig).background().color();
                case ColorGroup::Inactive:
                    return KColorScheme(QPalette::Normal, KColorScheme::ColorSet::Header, m_colorSchemeConfig).background().color();
                default:
                    return QColor();
            }
        case ColorRole::Foreground:
            switch (group) {
                case ColorGroup::Active:
                    return KColorScheme(QPalette::Normal, KColorScheme::ColorSet::Header, m_colorSchemeConfig).foreground().color();
                case ColorGroup::Inactive:
                    return KColorScheme(QPalette::Inactive, KColorScheme::ColorSet::Header, m_colorSchemeConfig).foreground().color();
                case ColorGroup::Warning:
                    return KColorScheme(QPalette::Inactive, KColorScheme::ColorSet::Header, m_colorSchemeConfig).foreground(KColorScheme::ForegroundRole::NegativeText).color();
                default:
                    return QColor();
            }
        default:
            return QColor();
    }
}

QPalette DecorationPalette::palette() const
{
    return m_palette;
}

void DecorationPalette::update()
{
    m_colorSchemeConfig = KSharedConfig::openConfig(m_colorScheme, KConfig::SimpleConfig);
    m_palette = KColorScheme::createApplicationPalette(m_colorSchemeConfig);
}

}
}
