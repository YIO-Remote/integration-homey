/******************************************************************************
 *
 * Copyright (C) 2019 Marton Borzak <hello@martonborzak.com>
 * Copyright (C) 2019 Christian Riedl <ric@rts.co.at>
 * Copyright (C) 2019 Niels de Klerk <hello@martonborzak.com>
 *
 * This file is part of the YIO-Remote software project.
 *
 * YIO-Remote software is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * YIO-Remote software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with YIO-Remote software. If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *****************************************************************************/

#pragma once

#include <QColor>
#include <QLoggingCategory>
#include <QObject>
#include <QString>
#include <QThread>
#include <QTimer>
#include <QVariant>
#include <QtWebSockets/QWebSocket>

#include "yio-interface/configinterface.h"
#include "yio-interface/entities/entitiesinterface.h"
#include "yio-interface/entities/entityinterface.h"
#include "yio-interface/notificationsinterface.h"
#include "yio-interface/plugininterface.h"
#include "yio-interface/yioapiinterface.h"
#include "yio-plugin/integration.h"
#include "yio-plugin/plugin.h"

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//// HOMEY FACTORY
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

const bool USE_WORKER_THREAD = true;

class HomeyPlugin : public Plugin {
    Q_OBJECT
    Q_INTERFACES(PluginInterface)
    Q_PLUGIN_METADATA(IID "YIO.PluginInterface" FILE "homey.json")

 public:
    HomeyPlugin();

    // Plugin interface
 protected:
    Integration* createIntegration(const QVariantMap& config, EntitiesInterface* entities,
                                   NotificationsInterface* notifications, YioAPIInterface* api,
                                   ConfigInterface* configObj) override;
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//// HOMEY THREAD CLASS
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

class Homey : public Integration {
    Q_OBJECT

 public:
    Homey(const QVariantMap& config, EntitiesInterface* entities, NotificationsInterface* notifications,
          YioAPIInterface* api, ConfigInterface* configObj, Plugin* plugin);

    Q_INVOKABLE void connect() override;
    Q_INVOKABLE void disconnect() override;
    Q_INVOKABLE void sendCommand(const QString& type, const QString& entityId, int command,
                                 const QVariant& param) override;

 public slots:  // NOLINT open issue: https://github.com/cpplint/cpplint/pull/99
    void onTextMessageReceived(const QString& message);
    void onStateChanged(QAbstractSocket::SocketState state);
    void onError(QAbstractSocket::SocketError error);
    void onTimeout();

 private:
    void webSocketSendCommand(const QVariantMap& data);
    int  convertBrightnessToPercentage(float value);

    void updateEntity(const QString& entity_id, const QVariantMap& attr);
    void updateLight(EntityInterface* entity, const QVariantMap& attr);
    void updateBlind(EntityInterface* entity, const QVariantMap& attr);
    void updateMediaPlayer(EntityInterface* entity, const QVariantMap& attr);
    void updateClimate(EntityInterface* entity, const QVariantMap& attr);
    void updateSwitch(EntityInterface* entity, const QVariantMap& attr);

 private:
    QString     m_id;
    QString     m_ip;
    QString     m_token;
    QWebSocket* m_webSocket;
    QTimer*     m_wsReconnectTimer;
    int         m_tries;
    int         m_webSocketId;
    bool        m_userDisconnect = false;
};
