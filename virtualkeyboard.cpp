/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2016 Martin Gräßlin <mgraesslin@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "virtualkeyboard.h"
#include "virtualkeyboard_dbus.h"
#include "input.h"
#include "keyboard_input.h"
#include "utils.h"
#include "screens.h"
#include "wayland_server.h"
#include "workspace.h"
#include "xkb.h"
#include "screenlockerwatcher.h"

#include <KWaylandServer/display.h>
#include <KWaylandServer/seat_interface.h>
#include <KWaylandServer/textinput_interface.h>
#include <KWaylandServer/surface_interface.h>
#include <KWaylandServer/inputmethod_v1_interface.h>

#include <KStatusNotifierItem>
#include <KLocalizedString>

#include <QDBusConnection>
#include <QDBusPendingCall>
#include <QDBusMessage>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickView>
#include <QQuickWindow>
#include <QTimer>
// xkbcommon
#include <xkbcommon/xkbcommon.h>

using namespace KWaylandServer;

namespace KWin
{

KWIN_SINGLETON_FACTORY(VirtualKeyboard)

VirtualKeyboard::VirtualKeyboard(QObject *parent)
    : QObject(parent)
{
    m_floodTimer = new QTimer(this);
    m_floodTimer->setSingleShot(true);
    m_floodTimer->setInterval(250);
    // this is actually too late. Other processes are started before init,
    // so might miss the availability of text input
    // but without Workspace we don't have the window listed at all
    connect(kwinApp(), &Application::workspaceCreated, this, &VirtualKeyboard::init);
}

VirtualKeyboard::~VirtualKeyboard() = default;

void VirtualKeyboard::init()
{
    connect(ScreenLockerWatcher::self(), &ScreenLockerWatcher::aboutToLock, this, &VirtualKeyboard::hide);

    if (waylandServer()) {
        m_enabled = !input()->hasAlphaNumericKeyboard();
        qCDebug(KWIN_VIRTUALKEYBOARD) << "enabled by default: " << m_enabled;
        connect(input(), &InputRedirection::hasAlphaNumericKeyboardChanged, this,
            [this] (bool set) {
                qCDebug(KWIN_VIRTUALKEYBOARD) << "AlphaNumeric Keyboard changed:" << set << "toggling VirtualKeyboard.";
                setEnabled(!set);
            }
        );
    }

    qCDebug(KWIN_VIRTUALKEYBOARD) << "Registering the SNI";
    m_sni = new KStatusNotifierItem(QStringLiteral("kwin-virtual-keyboard"), this);
    m_sni->setStandardActionsEnabled(false);
    m_sni->setCategory(KStatusNotifierItem::Hardware);
    m_sni->setStatus(KStatusNotifierItem::Passive);
    m_sni->setTitle(i18n("Virtual Keyboard"));
    updateSni();
    connect(m_sni, &KStatusNotifierItem::activateRequested, this,
        [this] {
            setEnabled(!m_enabled);
        }
    );
    connect(this, &VirtualKeyboard::enabledChanged, this, &VirtualKeyboard::updateSni);

    auto dbus = new VirtualKeyboardDBus(this);
    qCDebug(KWIN_VIRTUALKEYBOARD) << "Registering the DBus interface";
    dbus->setEnabled(m_enabled);
    connect(dbus, &VirtualKeyboardDBus::activateRequested, this, &VirtualKeyboard::setEnabled);
    connect(this, &VirtualKeyboard::enabledChanged, dbus, &VirtualKeyboardDBus::setEnabled);

    if (waylandServer()) {
        // we can announce support for the text input interface
        auto t = waylandServer()->display()->createTextInputManager(TextInputInterfaceVersion::UnstableV0, waylandServer()->display());
        t->create();
        auto t2 = waylandServer()->display()->createTextInputManager(TextInputInterfaceVersion::UnstableV2, waylandServer()->display());
        t2->create();

        auto inputPanel = waylandServer()->display()->createInputPanelInterface(this);
        connect(inputPanel, &InputPanelV1Interface::inputPanelSurfaceAdded, this, [this, inputPanel] (InputPanelSurfaceV1Interface *surface) {
            m_inputClient = waylandServer()->createInputPanelClient(surface);
            auto refreshFrame = [this] {
                const QRect inputGeometry = m_inputClient->surface()->input().boundingRect();
                if (!m_trackedClient || inputGeometry.isEmpty()) {
                    return;
                }
                m_trackedClient->setVirtualKeyboardGeometry(inputGeometry);
            };
            connect(surface->surface(), &SurfaceInterface::inputChanged, this, refreshFrame);
            connect(this, &VirtualKeyboard::hide, m_inputClient, [this] {
                m_inputClient->hideClient(true);
            });
            connect(this, &VirtualKeyboard::show, m_inputClient, [this] {
                m_inputClient->hideClient(false);
            });
            connect(surface->surface(), &SurfaceInterface::unmapped, this, [this, inputPanel, surface] {
                m_inputClient->destroyClient();
                inputPanel->inputPanelSurfaceAdded(surface);
            });
            refreshFrame();
        });

        connect(waylandServer()->seat(), &SeatInterface::focusedTextInputChanged, this,
            [this] {
                disconnect(m_waylandShowConnection);
                disconnect(m_waylandHideConnection);
                disconnect(m_waylandHintsConnection);
                disconnect(m_waylandSurroundingTextConnection);
                disconnect(m_waylandResetConnection);
                disconnect(m_waylandEnabledConnection);
                qApp->inputMethod()->reset();
                if (auto t = waylandServer()->seat()->focusedTextInput()) {
                    m_waylandShowConnection = connect(t, &TextInputInterface::requestShowInputPanel, this, &VirtualKeyboard::show);
                    m_waylandHideConnection = connect(t, &TextInputInterface::requestHideInputPanel, this, &VirtualKeyboard::hide);
                    m_waylandSurroundingTextConnection = connect(t, &TextInputInterface::surroundingTextChanged, this,
                        [] {
                            qApp->inputMethod()->update(Qt::ImSurroundingText | Qt::ImCursorPosition | Qt::ImAnchorPosition);
                        }
                    );
                    m_waylandHintsConnection = connect(t, &TextInputInterface::contentTypeChanged, this,
                        [] {
                            qApp->inputMethod()->update(Qt::ImHints);
                        }
                    );
                    m_waylandResetConnection = connect(t, &TextInputInterface::requestReset, this, [t] {
                        auto inputContext = waylandServer()->inputMethod()->context();
                        if (!inputContext) {
                            return;
                        }

                        inputContext->sendReset();
                        inputContext->sendSurroundingText(QString::fromUtf8(t->surroundingText()), t->surroundingTextCursorPosition(), t->surroundingTextSelectionAnchor());
                        inputContext->sendPreferredLanguage(QString::fromUtf8(t->preferredLanguage()));
                    });
                    m_waylandEnabledConnection = connect(t, &TextInputInterface::enabledChanged, this, [t, this] {
                        if (t->isEnabled()) {
                            waylandServer()->inputMethod()->sendDeactivate();
                            waylandServer()->inputMethod()->sendActivate();
                            adoptInputMethodContext();
                        } else {
                            waylandServer()->inputMethod()->sendDeactivate();
                            Q_EMIT hide();
                        }
                        qApp->inputMethod()->update(Qt::ImQueryAll);
                    });

                    auto newClient = waylandServer()->findClient(waylandServer()->seat()->focusedTextInputSurface());
                    // Reset the old client virtual keybaord geom if necessary
                    // Old and new clients could be the same if focus moves between subsurfaces
                    if (newClient != m_trackedClient) {
                        if (m_trackedClient) {
                            m_trackedClient->setVirtualKeyboardGeometry(QRect());
                        }
                        m_trackedClient = newClient;
                    }


                    updateInputPanelState();
                } else {
                    m_waylandShowConnection = QMetaObject::Connection();
                    m_waylandHideConnection = QMetaObject::Connection();
                    m_waylandHintsConnection = QMetaObject::Connection();
                    m_waylandSurroundingTextConnection = QMetaObject::Connection();
                    m_waylandResetConnection = QMetaObject::Connection();
                    m_waylandEnabledConnection = QMetaObject::Connection();
                }
                qApp->inputMethod()->update(Qt::ImQueryAll);
            }
        );
    }
    connect(qApp->inputMethod(), &QInputMethod::visibleChanged, this, &VirtualKeyboard::updateInputPanelState);
}

void VirtualKeyboard::setEnabled(bool enabled)
{
    if (m_enabled == enabled) {
        return;
    }
    m_enabled = enabled;
    qApp->inputMethod()->update(Qt::ImQueryAll);
    emit enabledChanged(m_enabled);

    // send OSD message
    QDBusMessage msg = QDBusMessage::createMethodCall(
        QStringLiteral("org.kde.plasmashell"),
        QStringLiteral("/org/kde/osdService"),
        QStringLiteral("org.kde.osdService"),
        QStringLiteral("virtualKeyboardEnabledChanged")
    );
    msg.setArguments({enabled});
    QDBusConnection::sessionBus().asyncCall(msg);
}

void VirtualKeyboard::adoptInputMethodContext()
{
    auto inputContext = waylandServer()->inputMethod()->context();
    TextInputInterface *ti = waylandServer()->seat()->focusedTextInput();

    inputContext->sendSurroundingText(QString::fromUtf8(ti->surroundingText()), ti->surroundingTextCursorPosition(), ti->surroundingTextSelectionAnchor());
    inputContext->sendPreferredLanguage(QString::fromUtf8(ti->preferredLanguage()));

    connect(inputContext, &KWaylandServer::InputMethodContextV1Interface::keysym, waylandServer(), [](quint32 serial, quint32 time, quint32 sym, bool pressed, Qt::KeyboardModifiers modifiers) {
        Q_UNUSED(serial)
        Q_UNUSED(time)
        auto t = waylandServer()->seat()->focusedTextInput();
        if (t && t->isEnabled()) {
            if (pressed) {
                t->keysymPressed(sym, modifiers);
            } else {
                t->keysymReleased(sym, modifiers);
            }
        }
    });

    connect(inputContext, &KWaylandServer::InputMethodContextV1Interface::commitString, waylandServer(), [](qint32 serial, const QString &text) {
        Q_UNUSED(serial)
        auto t = waylandServer()->seat()->focusedTextInput();
        if (t && t->isEnabled()) {
            t->commit(text.toUtf8());
        }
    });
    connect(inputContext, &KWaylandServer::InputMethodContextV1Interface::preeditCursor, waylandServer(), [](qint32 index) {
        auto t = waylandServer()->seat()->focusedTextInput();
        if (t && t->isEnabled()) {
            t->setPreEditCursor(index);
        }
    });
    connect(inputContext, &KWaylandServer::InputMethodContextV1Interface::preeditString, waylandServer(), [](uint32_t serial, const QString &text, const QString &commit) {
        Q_UNUSED(serial)
        auto t = waylandServer()->seat()->focusedTextInput();
        if (t && t->isEnabled()) {
            t->preEdit(text.toUtf8(), commit.toUtf8());
        }
    });
    connect(inputContext, &KWaylandServer::InputMethodContextV1Interface::deleteSurroundingText, waylandServer(), [](int32_t index, uint32_t length) {
        auto t = waylandServer()->seat()->focusedTextInput();
        if (t && t->isEnabled()) {
            t->deleteSurroundingText(index, length);
        }
    });
    connect(inputContext, &KWaylandServer::InputMethodContextV1Interface::cursorPosition, waylandServer(), [](qint32 index, qint32 anchor) {
        auto t = waylandServer()->seat()->focusedTextInput();
        if (t && t->isEnabled()) {
            t->setCursorPosition(index, anchor);
        }
    });
    connect(inputContext, &KWaylandServer::InputMethodContextV1Interface::language, waylandServer(), [](uint32_t serial, const QString &language) {
        Q_UNUSED(serial)
        auto t = waylandServer()->seat()->focusedTextInput();
        if (t && t->isEnabled()) {
            t->setLanguage(language.toUtf8());
        }
    });
    connect(inputContext, &KWaylandServer::InputMethodContextV1Interface::textDirection, waylandServer(), [](uint32_t serial, Qt::LayoutDirection direction) {
        Q_UNUSED(serial)
        auto t = waylandServer()->seat()->focusedTextInput();
        if (t && t->isEnabled()) {
            t->setTextDirection(direction);
        }
    });
}

void VirtualKeyboard::updateSni()
{
    if (!m_sni) {
        return;
    }
    if (m_enabled) {
        m_sni->setIconByName(QStringLiteral("input-keyboard-virtual-on"));
        m_sni->setTitle(i18n("Virtual Keyboard: enabled"));
    } else {
        m_sni->setIconByName(QStringLiteral("input-keyboard-virtual-off"));
        m_sni->setTitle(i18n("Virtual Keyboard: disabled"));
    }
    m_sni->setToolTipTitle(i18n("Whether to show the virtual keyboard on demand."));
}

void VirtualKeyboard::updateInputPanelState()
{
    if (!waylandServer()) {
        return;
    }

    auto t = waylandServer()->seat()->focusedTextInput();

    if (!t) {
        return;
    }

    if (m_inputClient) {
        m_trackedClient->setVirtualKeyboardGeometry(m_inputClient->frameGeometry());
        t->setInputPanelState(true, QRect(0, 0, 0, 0));
    }
}

bool VirtualKeyboard::event(QEvent *e)
{
    if (e->type() == QEvent::InputMethod) {
        QInputMethodEvent *event = static_cast<QInputMethodEvent*>(e);
        if (m_enabled && waylandServer()) {
            bool isPreedit = false;
            for (auto attribute : event->attributes()) {
                switch (attribute.type) {
                case QInputMethodEvent::TextFormat:
                case QInputMethodEvent::Cursor:
                case QInputMethodEvent::Language:
                case QInputMethodEvent::Ruby:
                    isPreedit = true;
                    break;
                default:
                    break;
                }
            }
            TextInputInterface *ti = waylandServer()->seat()->focusedTextInput();
            if (ti && ti->isEnabled()) {
                if (!isPreedit && event->preeditString().isEmpty() && !event->commitString().isEmpty()) {
                    ti->commit(event->commitString().toUtf8());
                } else {
                    ti->preEdit(event->preeditString().toUtf8(), event->commitString().toUtf8());
                }
            }
        }
    }
    if (e->type() == QEvent::InputMethodQuery) {
        auto event = static_cast<QInputMethodQueryEvent*>(e);
        TextInputInterface *ti = nullptr;
        if (waylandServer() && m_enabled) {
            ti = waylandServer()->seat()->focusedTextInput();
        }
        if (event->queries().testFlag(Qt::ImEnabled)) {
            event->setValue(Qt::ImEnabled, QVariant(ti != nullptr && ti->isEnabled()));
        }
        if (event->queries().testFlag(Qt::ImCursorRectangle)) {
            // not used by virtual keyboard
        }
        if (event->queries().testFlag(Qt::ImFont)) {
            // not used by virtual keyboard
        }
        if (event->queries().testFlag(Qt::ImCursorPosition)) {
            // the virtual keyboard doesn't send us the cursor position in the preedit
            // this would break text input, thus we ignore it
            // see https://bugreports.qt.io/browse/QTBUG-53517
#if 0
            event->setValue(Qt::ImCursorPosition, QString::fromUtf8(ti->surroundingText().left(ti->surroundingTextCursorPosition())).size());
#else
            event->setValue(Qt::ImCursorPosition, 0);
#endif
        }
        if (event->queries().testFlag(Qt::ImSurroundingText)) {
            // the virtual keyboard doesn't send us the cursor position in the preedit
            // this would break text input, thus we ignore it
            // see https://bugreports.qt.io/browse/QTBUG-53517
#if 0
            event->setValue(Qt::ImSurroundingText, QString::fromUtf8(ti->surroundingText()));
#else
            event->setValue(Qt::ImSurroundingText, QString());
#endif
        }
        if (event->queries().testFlag(Qt::ImCurrentSelection)) {
            // TODO: should be text between cursor and anchor, but might be dangerous
        }
        if (event->queries().testFlag(Qt::ImMaximumTextLength)) {
            // not used by virtual keyboard
        }
        if (event->queries().testFlag(Qt::ImAnchorPosition)) {
            // not used by virtual keyboard
        }
        if (event->queries().testFlag(Qt::ImHints)) {
            if (ti && ti->isEnabled()) {
                Qt::InputMethodHints hints;
                const auto contentHints = ti->contentHints();
                if (!contentHints.testFlag(TextInputInterface::ContentHint::AutoCompletion)) {
                    hints |= Qt::ImhNoPredictiveText;
                }
                if (contentHints.testFlag(TextInputInterface::ContentHint::AutoCorrection)) {
                    // no mapping in Qt
                }
                if (!contentHints.testFlag(TextInputInterface::ContentHint::AutoCapitalization)) {
                    hints |= Qt::ImhNoAutoUppercase;
                }
                if (contentHints.testFlag(TextInputInterface::ContentHint::LowerCase)) {
                    hints |= Qt::ImhPreferLowercase;
                }
                if (contentHints.testFlag(TextInputInterface::ContentHint::UpperCase)) {
                    hints |= Qt::ImhPreferUppercase;
                }
                if (contentHints.testFlag(TextInputInterface::ContentHint::TitleCase)) {
                    // no mapping in Qt
                }
                if (contentHints.testFlag(TextInputInterface::ContentHint::HiddenText)) {
                    hints |= Qt::ImhHiddenText;
                }
                if (contentHints.testFlag(TextInputInterface::ContentHint::SensitiveData)) {
                    hints |= Qt::ImhSensitiveData;
                }
                if (contentHints.testFlag(TextInputInterface::ContentHint::Latin)) {
                    hints |= Qt::ImhPreferLatin;
                }
                if (contentHints.testFlag(TextInputInterface::ContentHint::MultiLine)) {
                    hints |= Qt::ImhMultiLine;
                }
                switch (ti->contentPurpose()) {
                case TextInputInterface::ContentPurpose::Digits:
                    hints |= Qt::ImhDigitsOnly;
                    break;
                case TextInputInterface::ContentPurpose::Number:
                    hints |= Qt::ImhFormattedNumbersOnly;
                    break;
                case TextInputInterface::ContentPurpose::Phone:
                    hints |= Qt::ImhDialableCharactersOnly;
                    break;
                case TextInputInterface::ContentPurpose::Url:
                    hints |= Qt::ImhUrlCharactersOnly;
                    break;
                case TextInputInterface::ContentPurpose::Email:
                    hints |= Qt::ImhEmailCharactersOnly;
                    break;
                case TextInputInterface::ContentPurpose::Date:
                    hints |= Qt::ImhDate;
                    break;
                case TextInputInterface::ContentPurpose::Time:
                    hints |= Qt::ImhTime;
                    break;
                case TextInputInterface::ContentPurpose::DateTime:
                    hints |= Qt::ImhDate;
                    hints |= Qt::ImhTime;
                    break;
                case TextInputInterface::ContentPurpose::Name:
                    // no mapping in Qt
                case TextInputInterface::ContentPurpose::Password:
                    // no mapping in Qt
                case TextInputInterface::ContentPurpose::Terminal:
                    // no mapping in Qt
                case TextInputInterface::ContentPurpose::Normal:
                    // that's the default
                case TextInputInterface::ContentPurpose::Alpha:
                    // no mapping in Qt
                    break;
                }
                event->setValue(Qt::ImHints, QVariant(int(hints)));
            } else {
                event->setValue(Qt::ImHints, Qt::ImhNone);
            }
        }
        if (event->queries().testFlag(Qt::ImPreferredLanguage)) {
            // not used by virtual keyboard
        }
        if (event->queries().testFlag(Qt::ImPlatformData)) {
            // not used by virtual keyboard
        }
        if (event->queries().testFlag(Qt::ImAbsolutePosition)) {
            // not used by virtual keyboard
        }
        if (event->queries().testFlag(Qt::ImTextBeforeCursor)) {
            // not used by virtual keyboard
        }
        if (event->queries().testFlag(Qt::ImTextAfterCursor)) {
            // not used by virtual keyboard
        }
        event->accept();
        return true;
    }
    return QObject::event(e);
}

}
