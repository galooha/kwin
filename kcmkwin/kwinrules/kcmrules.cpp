/*
 * Copyright (c) 2004 Lubos Lunak <l.lunak@kde.org>
 * Copyright (c) 2020 Ismael Asensio <isma.af@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License or (at your option) version 3 or any later version
 * accepted by the membership of KDE e.V. (or its successor approved
 * by the membership of KDE e.V.), which shall act as a proxy
 * defined in Section 14 of version 3 of the license.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "kcmrules.h"

#include <QDBusConnection>
#include <QDBusMessage>

#include <KAboutData>
#include <KConfig>
#include <KLocalizedString>
#include <KPluginFactory>


namespace KWin
{

KCMKWinRules::KCMKWinRules(QObject *parent, const QVariantList &arguments)
    : KQuickAddons::ConfigModule(parent, arguments)
    , m_ruleBookModel(new RuleBookModel(this))
    , m_rulesModel(new RulesModel(this))
{
    auto about = new KAboutData(QStringLiteral("kcm_kwinrules"),
                                i18n("Window Rules"),
                                QStringLiteral("1.0"),
                                QString(),
                                KAboutLicense::GPL);
    about->addAuthor(i18n("Ismael Asensio"),
                     i18n("Author"),
                     QStringLiteral("isma.af@gmail.com"));
    setAboutData(about);

    setQuickHelp(i18n("<p><h1>Window-specific Settings</h1> Here you can customize window settings specifically only"
                      " for some windows.</p>"
                      " <p>Please note that this configuration will not take effect if you do not use"
                      " KWin as your window manager. If you do use a different window manager, please refer to its documentation"
                      " for how to customize window behavior.</p>"));

    // When called from window menu, "uuid" and "whole-app" are set in arguments list
    for (const QVariant arg : arguments) {
        if (arg.toString().startsWith("uuid=")) {
            const QUuid winUuid = QUuid(arg.toString().mid(strlen("uuid=")));
            getWindowProperties(winUuid);
        }
        if (arg.toString() == QStringLiteral("whole-app")) {
            m_wholeApp = true;
        }
    }

    connect(m_rulesModel, &RulesModel::descriptionChanged, this, [this]{
        if (m_editIndex.isValid()) {
            m_ruleBookModel->setDescriptionAt(m_editIndex.row(), m_rulesModel->description());
        }
    } );
    connect(m_rulesModel, &RulesModel::dataChanged, this, &KCMKWinRules::updateNeedsSave);
    connect(m_ruleBookModel, &RulesModel::dataChanged, this, &KCMKWinRules::updateNeedsSave);
}

void KCMKWinRules::load()
{
    m_ruleBookModel->load();
    setNeedsSave(false);

    if (!m_winProperties.isEmpty()) {
        QModelIndex matchedIndex = m_ruleBookModel->findRuleWithProperties(m_winProperties, m_wholeApp);
        if (!matchedIndex.isValid()) {
            m_ruleBookModel->insertRow(0);
            m_ruleBookModel->setRuleAt(0, newRuleForProperties());
            matchedIndex = m_ruleBookModel->index(0);
            updateNeedsSave();
        }
        editRule(matchedIndex.row());
        m_rulesModel->setWindowProperties(m_winProperties, true);
        m_winProperties.clear();
        return;
    }

    m_editIndex = QModelIndex();
    emit editIndexChanged();
}

void KCMKWinRules::save()
{
    saveCurrentRule();

    m_ruleBookModel->save();

    // Notify kwin to reload configuration
    QDBusMessage message = QDBusMessage::createSignal("/KWin", "org.kde.KWin", "reloadConfig");
    QDBusConnection::sessionBus().send(message);
}

void KCMKWinRules::updateNeedsSave()
{
    setNeedsSave(true);
    emit needsSaveChanged();
}

void KCMKWinRules::saveCurrentRule()
{
    if (m_editIndex.isValid() && needsSave()) {
        m_ruleBookModel->setRuleAt(m_editIndex.row(), m_rulesModel->exportToRules());
    }
}

int KCMKWinRules::editIndex() const
{
    if (!m_editIndex.isValid()) {
        return -1;
    }
    return m_editIndex.row();
}


void KCMKWinRules::setRuleDescription(int index, const QString &description)
{
    if (index < 0 || index >= m_ruleBookModel->rowCount()) {
        return;
    }

    if (m_editIndex.row() == index) {
        m_rulesModel->setDescription(description);
        return;
    }
    m_ruleBookModel->setDescriptionAt(index, description);

    updateNeedsSave();
}


void KCMKWinRules::editRule(int index)
{
    if (index < 0 || index >= m_ruleBookModel->rowCount()) {
        return;
    }
    saveCurrentRule();

    m_editIndex = m_ruleBookModel->index(index);
    emit editIndexChanged();

    m_rulesModel->importFromRules(m_ruleBookModel->ruleAt(m_editIndex.row()));

    // Set the active page to rules editor (0:RulesList, 1:RulesEditor)
    setCurrentIndex(1);
}

void KCMKWinRules::createRule()
{
    const int newIndex = m_ruleBookModel->rowCount();
    m_ruleBookModel->insertRow(newIndex);

    updateNeedsSave();

    editRule(newIndex);
}

void KCMKWinRules::removeRule(int index)
{
    if (index < 0 || index >= m_ruleBookModel->rowCount()) {
        return;
    }

    m_ruleBookModel->removeRow(index);

    emit editIndexChanged();
    updateNeedsSave();
}

void KCMKWinRules::moveRule(int sourceIndex, int destIndex)
{
    const int lastIndex = m_ruleBookModel->rowCount() - 1;
    if (sourceIndex == destIndex
            || (sourceIndex < 0 || sourceIndex > lastIndex)
            || (destIndex < 0 || destIndex > lastIndex)) {
        return;
    }

    m_ruleBookModel->moveRow(QModelIndex(), sourceIndex, QModelIndex(), destIndex);

    emit editIndexChanged();
    updateNeedsSave();
}

void KCMKWinRules::exportToFile(const QUrl &path, const QList<int> &indexes)
{
    if (indexes.isEmpty()) {
        return;
    }
    saveCurrentRule();

    const auto config = KSharedConfig::openConfig(path.toLocalFile(), KConfig::SimpleConfig);

    for (const QString &groupName : config->groupList()) {
        config->deleteGroup(groupName);
    }

    for (int index : indexes) {
        if (index < 0 || index > m_ruleBookModel->rowCount()) {
            continue;
        }
        const Rules *rule = m_ruleBookModel->ruleAt(index);
        RuleSettings settings(config, rule->description);
        settings.setDefaults();
        rule->write(&settings);
        settings.save();
    }
}

void KCMKWinRules::importFromFile(const QUrl &path)
{
    const auto config = KSharedConfig::openConfig(path.toLocalFile(), KConfig::SimpleConfig);
    const QStringList groups = config->groupList();
    if (groups.isEmpty()) {
        return;
    }

    for (const QString &groupName : groups) {
        RuleSettings settings(config, groupName);

        const bool remove = settings.deleteRule();
        const QString importDescription = settings.description();
        if (importDescription.isEmpty()) {
            continue;
        }

        // Try to find a rule with the same description to replace
        int newIndex = -2;
        for (int index = 0; index < m_ruleBookModel->rowCount(); index++) {
            if (m_ruleBookModel->descriptionAt(index) == importDescription) {
                newIndex = index;
                break;
            }
        }

        if (remove) {
            m_ruleBookModel->removeRow(newIndex);
            continue;
        }

        if (newIndex < 0) {
            newIndex = m_ruleBookModel->rowCount();
            m_ruleBookModel->insertRow(newIndex);
        }

        m_ruleBookModel->setRuleAt(newIndex, new Rules(&settings));

        // Reset rule editor if the current rule changed when importing
        if (m_editIndex.row() == newIndex) {
            m_rulesModel->importFromRules(m_ruleBookModel->ruleAt(m_editIndex.row()));
        }
    }

    updateNeedsSave();
}

void KCMKWinRules::getWindowProperties(const QUuid &winUuid)
{
    if (winUuid.isNull()) {
        qDebug() << "Invalid window uuid.";
        return;
    }

    QDBusMessage message = QDBusMessage::createMethodCall(QStringLiteral("org.kde.KWin"),
                                                          QStringLiteral("/KWin"),
                                                          QStringLiteral("org.kde.KWin"),
                                                          QStringLiteral("getWindowInfo"));
    message.setArguments({winUuid.toString()});
    QDBusPendingReply<QVariantMap> async = QDBusConnection::sessionBus().asyncCall(message);

    QDBusPendingCallWatcher *callWatcher = new QDBusPendingCallWatcher(async, this);
    connect(callWatcher, &QDBusPendingCallWatcher::finished, this,
            [this, winUuid] (QDBusPendingCallWatcher *self) {
                QDBusPendingReply<QVariantMap> reply = *self;
                self->deleteLater();
                if (!reply.isValid() || reply.value().isEmpty()) {
                    qDebug() << "Error retrieving properties for window" << winUuid;
                    return;
                }
                qDebug() << "Retrieved properties for window" << winUuid;
                m_winProperties = reply.value();
            }
    );
}


// Code adapted from original `findRule()` method in `kwin_rules_dialog::main.cpp`
// TODO: This could be merged (or replaced) with RulesModel::setWindowProperties()
// adding the missing bits, but let's keep it unchanged for now to avoid unintended bugs
Rules *KCMKWinRules::newRuleForProperties() const
{
    const QByteArray wmclass_class = m_winProperties.value("resourceClass").toByteArray().toLower();
    const QByteArray wmclass_name = m_winProperties.value("resourceName").toByteArray().toLower();
    const QByteArray role = m_winProperties.value("role").toByteArray().toLower();
    const NET::WindowType type = static_cast<NET::WindowType>(m_winProperties.value("type").toInt());
    const QString title = m_winProperties.value("caption").toString();
    const QByteArray machine = m_winProperties.value("clientMachine").toByteArray();

    Rules *rule = new Rules;

    if (m_wholeApp) {
        rule->description = i18n("Application settings for %1", QString::fromLatin1(wmclass_class));
        // TODO maybe exclude some types? If yes, then also exclude them above
        // when searching.
        rule->types = NET::AllTypesMask;
        rule->titlematch = Rules::UnimportantMatch;
        rule->clientmachine = machine; // set, but make unimportant
        rule->clientmachinematch = Rules::UnimportantMatch;
        rule->windowrolematch = Rules::UnimportantMatch;
        if (wmclass_name == wmclass_class) {
            rule->wmclasscomplete = false;
            rule->wmclass = wmclass_class;
            rule->wmclassmatch = Rules::ExactMatch;
        } else {
            // WM_CLASS components differ - perhaps the app got -name argument
            rule->wmclasscomplete = true;
            rule->wmclass = wmclass_name + ' ' + wmclass_class;
            rule->wmclassmatch = Rules::ExactMatch;
        }
        return rule;
    }

    rule->description = i18n("Window settings for %1", QString::fromLatin1(wmclass_class));
    if (type == NET::Unknown) {
        rule->types = NET::NormalMask;
    } else {
        rule->types = NET::WindowTypeMask( 1 << type); // convert type to its mask
    }
    rule->title = title; // set, but make unimportant
    rule->titlematch = Rules::UnimportantMatch;
    rule->clientmachine = machine; // set, but make unimportant
    rule->clientmachinematch = Rules::UnimportantMatch;
    if (!role.isEmpty() && role != "unknown" && role != "unnamed") { // Qt sets this if not specified
        rule->windowrole = role;
        rule->windowrolematch = Rules::ExactMatch;
        if (wmclass_name == wmclass_class) {
            rule->wmclasscomplete = false;
            rule->wmclass = wmclass_class;
            rule->wmclassmatch = Rules::ExactMatch;
        } else {
            // WM_CLASS components differ - perhaps the app got -name argument
            rule->wmclasscomplete = true;
            rule->wmclass = wmclass_name + ' ' + wmclass_class;
            rule->wmclassmatch = Rules::ExactMatch;
        }
    } else { // no role set
        if (wmclass_name != wmclass_class) {
            rule->wmclasscomplete = true;
            rule->wmclass = wmclass_name + ' ' + wmclass_class;
            rule->wmclassmatch = Rules::ExactMatch;
        } else {
            // This is a window that has no role set, and both components of WM_CLASS
            // match (possibly only differing in case), which most likely means either
            // the application doesn't give a damn about distinguishing its various
            // windows, or it's an app that uses role for that, but this window
            // lacks it for some reason. Use non-complete WM_CLASS matching, also
            // include window title in the matching, and pray it causes many more positive
            // matches than negative matches.
            rule->titlematch = Rules::ExactMatch;
            rule->wmclasscomplete = false;
            rule->wmclass = wmclass_class;
            rule->wmclassmatch = Rules::ExactMatch;
        }
    }
    return rule;
}

K_PLUGIN_CLASS_WITH_JSON(KCMKWinRules, "kcm_kwinrules.json");

} // namespace

#include "kcmrules.moc"
