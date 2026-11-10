#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QPair>
#include <QtCore/QString>
#include <QtCore/QUrl>
#include <QtCore/QVector>
#include <QtCore/QtGlobal>

#include "communicator_v2.h"
#include "garmingfdimessage.h"
#include "garmintypes.h"

// -----------------------------------------------------------------------------
// A web request the watch tunnels through Smart field 2 (HttpService)
// -----------------------------------------------------------------------------
struct HttpServiceRequest {
    QString url;
    QString method;
    QVector<QPair<QString, QString>> headers;
    QString rawBody; // RawRequest field 7, rawBody
};

class GarminHttpServiceMessage : public GarminGfdiMessage {
    Q_OBJECT
public:
    GarminHttpServiceMessage(CommunicatorV2* parent)
    {
        mCommunicator = parent;
    }
    // data is the HttpService message content (Smart field 2 payload)
    void parse(const QByteArray& data, quint16 requestId);

private:
    void parseRawRequest(const QByteArray& data, HttpServiceRequest& request);
    void sendErrorResponse(quint16 requestId, bool webRequest);
    void sendRawResponse(quint16 requestId, const QByteArray& body);
    static QString oauthResponseJson(const QString& path, const HttpServiceRequest& request);
    void handleWeatherRequest(quint16 requestId, const QUrl& url);
    static bool isWeatherHost(const QString& host);
};
