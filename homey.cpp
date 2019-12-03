#include <QtDebug>
#include <QJsonDocument>
#include <QJsonArray>

#include "homey.h"
#include "../remote-software/sources/entities/entity.h"
#include "../remote-software/sources/entities/entities.h"
#include "math.h"

void Homey::create(const QVariantMap &config, QObject *entities, QObject *notifications, QObject *api, QObject *configObj)
{
    QMap<QObject *, QVariant> returnData;

    QVariantList data;
    QString mdns;

    for (QVariantMap::const_iterator iter = config.begin(); iter != config.end(); ++iter) {
        if (iter.key() == "mdns") {
            mdns = iter.value().toString();
        } else if (iter.key() == "data") {
            data = iter.value().toList();
        }
    }

    for (int i=0; i<data.length(); i++)
    {
        HomeyBase* ha = new HomeyBase(this);
        ha->setup(data[i].toMap(), entities, notifications, api, configObj);

        QVariantMap d = data[i].toMap();
        d.insert("mdns", mdns);
        d.insert("type", config.value("type").toString());
        returnData.insert(ha, d);
    }

    emit createDone(returnData);
}

HomeyBase::HomeyBase(QObject *parent)
{
    this->setParent(parent);
}

HomeyBase::~HomeyBase()
{
    if (m_thread.isRunning()) {
        m_thread.exit();
        m_thread.wait(5000);
    }
}

void HomeyBase::setup(const QVariantMap& config, QObject* entities, QObject* notifications, QObject* api, QObject *configObj)
{
    for (QVariantMap::const_iterator iter = config.begin(); iter != config.end(); ++iter) {
        if (iter.key() == "friendly_name")
            setFriendlyName(iter.value().toString());
        else if (iter.key() == "id")
            setIntegrationId(iter.value().toString());
    }

    // crate a new instance and pass on variables
    HomeyThread *HAThread = new HomeyThread(config, entities, notifications, api, configObj);

    // move to thread
    HAThread->moveToThread(&m_thread);

    // connect signals and slots
    QObject::connect(&m_thread, &QThread::finished, HAThread, &QObject::deleteLater);

    QObject::connect(this, &HomeyBase::connectSignal, HAThread, &HomeyThread::connect);
    QObject::connect(this, &HomeyBase::disconnectSignal, HAThread, &HomeyThread::disconnect);
    QObject::connect(this, &HomeyBase::sendCommandSignal, HAThread, &HomeyThread::sendCommand);

    QObject::connect(HAThread, &HomeyThread::stateChanged, this, &HomeyBase::stateHandler);

    m_thread.start();
}

void HomeyBase::connect()
{
    emit connectSignal();
}

void HomeyBase::disconnect()
{
    emit disconnectSignal();
}

void HomeyBase::sendCommand(const QString &type, const QString &entity_id, const QString &command, const QVariant &param)
{
    emit sendCommandSignal(type, entity_id, command, param);
}

void HomeyBase::stateHandler(int state)
{
    if (state == 0)
    {
        setState(CONNECTED);
    }
    else if (state == 1)
    {
        setState(CONNECTING);
    }
    else if (state == 2)
    {
        setState(DISCONNECTED);
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//// Homey THREAD CLASS
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

HomeyThread::HomeyThread(const QVariantMap &config, QObject *entities, QObject *notifications, QObject* api, QObject *configObj)
{
    for (QVariantMap::const_iterator iter = config.begin(); iter != config.end(); ++iter)
    {
        if (iter.key() == "data")
        {
            QVariantMap map = iter.value().toMap();
            m_ip = map.value("ip").toString();
            m_token = map.value("token").toString();
        } else if (iter.key() == "id") {
            m_id = iter.value().toString();
        }
    }
    m_entities = qobject_cast<EntitiesInterface *>(entities);
    m_notifications = qobject_cast<NotificationsInterface *>(notifications);
    m_api = qobject_cast<YioAPIInterface *>(api);
    m_config = qobject_cast<ConfigInterface *>(configObj);

    m_webSocketId = 4;

    m_websocketReconnect = new QTimer(this);

    m_websocketReconnect->setSingleShot(true);
    m_websocketReconnect->setInterval(2000);
    m_websocketReconnect->stop();

    m_socket = new QWebSocket;
    m_socket->setParent(this);

    QObject::connect(m_socket, SIGNAL(textMessageReceived(const QString &)), this, SLOT(onTextMessageReceived(const QString &)));
    QObject::connect(m_socket, SIGNAL(error(QAbstractSocket::SocketError)), this, SLOT(onError(QAbstractSocket::SocketError)));
    QObject::connect(m_socket, SIGNAL(stateChanged(QAbstractSocket::SocketState)), this, SLOT(onStateChanged(QAbstractSocket::SocketState)));

    QObject::connect(m_websocketReconnect, SIGNAL(timeout()), this, SLOT(onTimeout()));
}

void HomeyThread::onTextMessageReceived(const QString &message)
{
    QJsonParseError parseerror;
    QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8(), &parseerror);
    if (parseerror.error != QJsonParseError::NoError)
    {
        qDebug() << "JSON error : " << parseerror.errorString();
        return;
    }
    QVariantMap map = doc.toVariant().toMap();

    QString m = map.value("error").toString();
    if (m.length() > 0)
    {
        qDebug() << "error : " << m;
    }

    QString type = map.value("type").toString();
    //    int id = map.value("id").toInt();

    if (type == "connected")
    {
        setState(0);
    }

    // handle get config request from homey app
    if (type == "command" && map.value("command").toString() == "get_config")
    {
        // get loaded homey entities
        QList<EntityInterface *> es = m_entities->getByIntegration(m_id);

        // create return map object
        QVariantMap returnData;

        // set type
        returnData.insert("type", "sendConfig");

        // create list to store entity ids
        QStringList list;

        // interate throug the list and get the entity ids

        foreach (EntityInterface *value, es)
        {
            list.append(value->entity_id());
            qDebug() << value->entity_id();
        }
        qDebug() << "LIST" << list;
        // insert list to data key in response
        returnData.insert("devices", list);

        // convert map to json
        QJsonDocument doc = QJsonDocument::fromVariant(returnData);
        QString message = doc.toJson(QJsonDocument::JsonFormat::Compact);

        // send message
        m_socket->sendTextMessage(message);
    }

    // handle fetch states from homey app
    if (type == "sendStates")
    {
        QVariantMap data = map.value("data").toMap();
        updateEntity(data.value("entity_id").toString(), data);
    }

    if (type == "event")
    {
        QVariantMap data = map.value("data").toMap();
        updateEntity(data.value("entity_id").toString(), data);
    }
}

void HomeyThread::onStateChanged(QAbstractSocket::SocketState state)
{
    if (state == QAbstractSocket::UnconnectedState && !m_userDisconnect)
    {
        setState(2);
        m_websocketReconnect->start();
    }
}

void HomeyThread::onError(QAbstractSocket::SocketError error)
{
    qDebug() << error;
    m_socket->close();
    setState(2);
    m_websocketReconnect->start();
}

void HomeyThread::onTimeout()
{
    if (m_tries == 3)
    {
        m_websocketReconnect->stop();

        m_notifications->add(true, tr("Cannot connect to Homey."), tr("Reconnect"), "homey");
        disconnect();
        m_tries = 0;
    }
    else
    {
        m_webSocketId = 4;
        if (m_state != 1)
        {
            setState(1);
        }
        QString url = QString("ws://").append(m_ip);
        m_socket->open(QUrl(url));

        m_tries++;
    }
}

void HomeyThread::webSocketSendCommand(QVariantMap data)
{
    QJsonDocument doc = QJsonDocument::fromVariant(data);
    QString message = doc.toJson(QJsonDocument::JsonFormat::Compact);
    m_socket->sendTextMessage(message);
}

int HomeyThread::convertBrightnessToPercentage(float value)
{
    return int(round(value * 100));
}

void HomeyThread::updateEntity(const QString &entity_id, const QVariantMap &attr)
{
    EntityInterface *entity = m_entities->getEntityInterface(entity_id);
    if (entity)
    {
        if (entity->type() == "light")
        {
            updateLight(entity, attr);
        }
        if (entity->type() == "blind")
        {
            updateBlind(entity, attr);
        }
        if (entity->type() == "media_player")
        {
            updateMediaPlayer(entity, attr);
        }
    }
}

void HomeyThread::updateLight(EntityInterface *entity, const QVariantMap &attr)
{
    QVariantMap attributes;

    //onoff to state.
    if (attr.contains("onoff"))
    {
        attributes.insert("state", attr.value("onoff"));
        printf("Setting state");
    }

    // brightness
    if (entity->supported_features().indexOf("BRIGHTNESS") > -1)
    {
        if (attr.contains("dim"))
        {
            attributes.insert("brightness", convertBrightnessToPercentage(attr.value("dim").toFloat()));
            printf("Setting brightness");
        }
    }

    // color
    if (entity->supported_features().indexOf("COLOR") > -1)
    {
        QVariant color = attr.value("attributes").toMap().value("rgb_color");
        QVariantList cl(color.toList());
        char buffer[10];
        sprintf(buffer, "#%02X%02X%02X", cl.value(0).toInt(), cl.value(1).toInt(), cl.value(2).toInt());
        attributes.insert("color", buffer);
    }

    m_entities->update(entity->entity_id(), attributes);
}

void HomeyThread::updateBlind(EntityInterface *entity, const QVariantMap &attr)
{
    //    QVariantMap attributes;

    //    // state
    //    if (attr.value("state").toString() == "open") {
    //        attributes.insert("state", true);
    //    } else {
    //        attributes.insert("state", false);
    //    }

    //    // position
    //    if (entity->supported_features().indexOf("POSITION") > -1) {
    //        attributes.insert("position", attr.value("attributes").toMap().value("current_position").toInt());
    //    }

    //    m_entities->update(entity->entity_id(), attributes);
}

void HomeyThread::updateMediaPlayer(EntityInterface *entity, const QVariantMap &attr)
{
    /*  capabilities:
       [ 'speaker_album',
         'speaker_artist',
         'speaker_duration',
         'speaker_next',
         'speaker_playing',
         'speaker_position',
         'speaker_prev',
         'speaker_repeat',
         'volume_set',
         'volume_mute',
         'speaker_shuffle',
         'speaker_track',
         'sonos_group',
         'sonos_audio_clip' ]
    */
    QVariantMap attributes;

    //state
    if (attr.contains("speaker_playing"))
    {
        if (attr.value("speaker_playing").toBool())
        {
            attributes.insert("state", 3); //Playing
            printf("Setting state 2");
        }
        else
        {
            attributes.insert("state", 2); //idle
            printf("Setting state 3");
        }
    }

    if (attr.contains("onoff"))
    {
        if (attr.value("onoff").toBool())
        {
            attributes.insert("state", 1); //On
        }
        else
        {
            attributes.insert("state", 0); //Off
        }
    }

    // source
    //if (entity->supported_features().indexOf("SOURCE") > -1 && attr.value("attributes").toMap().contains("source")) {
    //    attributes.insert("source", attr.value("attributes").toMap().value("source").toString());
    //}

    // volume  //volume_set
    if (attr.contains("volume_set"))
    {
        attributes.insert("volume", int(round(attr.value("volume_set").toDouble()*100)));
    }

    // media type
    if (entity->supported_features().indexOf("MEDIA_TYPE") > -1 && attr.value("attributes").toMap().contains("media_content_type"))
    {
        attributes.insert("mediaType", attr.value("attributes").toMap().value("media_content_type").toString());
    }

    // media image
    if (attr.contains("album_art"))
    {
        attributes.insert("mediaImage", attr.value("album_art"));
    }

    // media title
    if (attr.contains("speaker_track"))
    {
        attributes.insert("mediaTitle", attr.value("speaker_track").toString());
    }

    // media artist
    if (attr.contains("speaker_artist"))
    {
        attributes.insert("mediaArtist", attr.value("speaker_artist").toString());
    }

    m_entities->update(entity->entity_id(), attributes);
}

void HomeyThread::setState(int state)
{
    m_state = state;
    emit stateChanged(state);
}

void HomeyThread::connect()
{
    m_userDisconnect = false;

    setState(1);

    // reset the reconnnect trial variable
    m_tries = 0;

    // turn on the websocket connection
    QString url = QString("ws://").append(m_ip);
    m_socket->open(QUrl(url));
}

void HomeyThread::disconnect()
{
    m_userDisconnect = true;

    // turn of the reconnect try
    m_websocketReconnect->stop();

    // turn off the socket
    m_socket->close();

    setState(2);
}

void HomeyThread::sendCommand(const QString &type, const QString &entity_id, const QString &command, const QVariant &param)
{
    QVariantMap map;
    //example
    //{"command":"onoff","deviceId":"78f3ab16-c622-4bd7-aebf-3ca981e41375","type":"command","value":true}

    QVariantMap attributes;

    map.insert("type", "command");

    map.insert("deviceId", QVariant(entity_id));
    if (type == "light")
    {
        if (command == "TOGGLE")
        {
            map.insert("command", QVariant("toggle"));
            map.insert("value", true);
            webSocketSendCommand(map);
        }
        else if (command == "ON")
        {
            map.insert("command", QVariant("onoff"));
            map.insert("value", true);
            webSocketSendCommand(map);
        }
        else if (command == "OFF")
        {
            map.insert("command", QVariant("onoff"));
            map.insert("value", false);
            webSocketSendCommand(map);
        }
        else if (command == "BRIGHTNESS")
        {
            map.insert("command", "dim");
            float value = param.toFloat() / 100;
            map.insert("value", value);
            webSocketSendCommand(map);
        }
        else if (command == "COLOR")
        {
            QColor color = param.value<QColor>();
            //QVariantMap data;
            QVariantList list;
            list.append(color.red());
            list.append(color.green());
            list.append(color.blue());
            map.insert("command", "color");
            map.insert("value", list);
            webSocketSendCommand(map);
            //webSocketSendCommand(type, "turn_on", entity_id, &data);
        }
    }
    if (type == "blind")
    {
        if (command == "OPEN")
        {
            map.insert("command", "windowcoverings_closed");
            map.insert("value", "false");
            webSocketSendCommand(map);
        }
        else if (command == "CLOSE")
        {
            map.insert("command", "windowcoverings_closed");
            map.insert("value", "true");
            webSocketSendCommand(map);
        }
        else if (command == "STOP")
        {
            map.insert("command", "windowcoverings_tilt_set");
            map.insert("value", 0);
            webSocketSendCommand(map);
        }
        else if (command == "POSITION")
        {
            map.insert("command", "windowcoverings_set");
            map.insert("value", param);
            webSocketSendCommand(map);
        }
    }
    if (type == "media_player")

    {
        if (command == "VOLUME_SET")
        {
            map.insert("command", "volume_set");
            map.insert("value", param.toDouble()/100);
            attributes.insert("volume", param);
            m_entities->update(entity_id, attributes); //buggy homey fix
            webSocketSendCommand(map);
        }
        else if (command == "PLAY")
        {
            map.insert("command", "speaker_playing");
            map.insert("value", true);
            webSocketSendCommand(map);
        }
        else if (command == "STOP")
        {
            map.insert("command", "speaker_playing");
            map.insert("value", false);
            webSocketSendCommand(map);
        }
        else if (command == "PAUSE")
        {
            map.insert("command", "speaker_playing");
            map.insert("value", false);
            webSocketSendCommand(map);
        }
        else if (command == "PREVIOUS")
        {
            map.insert("command", "speaker_prev");
            map.insert("value", true);
            webSocketSendCommand(map);
        }
        else if (command == "NEXT")
        {
            map.insert("command", "speaker_next");
            map.insert("value", true);
            webSocketSendCommand(map);
        }
        else if (command == "TURNON")
        {
            map.insert("command", QVariant("onoff"));
            map.insert("value", true);
            webSocketSendCommand(map);
        }
        else if (command == "TURNOFF")
        {
            map.insert("command", QVariant("onoff"));
            map.insert("value", false);
            webSocketSendCommand(map);
        }
    }
}
