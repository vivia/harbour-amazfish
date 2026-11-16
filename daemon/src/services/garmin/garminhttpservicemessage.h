#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QPair>
#include <QtCore/QString>
#include <QtCore/QUrl>
#include <QtCore/QVector>
#include <QtCore/QtGlobal>

#include "communicator_v2.h"
#include "garmingfdimessage.h"
#include "garmintransferservice.h"
#include "garmintypes.h"

// -----------------------------------------------------------------------------
// A web request the watch tunnels through Smart field 2 (HttpService)
// -----------------------------------------------------------------------------
struct HttpServiceRequest {
    QString url;
    QString method;
    QVector<QPair<QString, QString>> headers;
    QString rawBody; // RawRequest field 7, rawBody
    bool useDataXfer = false; // RawRequest field 6, body wanted over DataTransferService
};

class GarminHttpServiceMessage : public GarminGfdiMessage {
    Q_OBJECT
public:
    GarminHttpServiceMessage(CommunicatorV2* parent, GarminDataTransferService* transfers)
    {
        mCommunicator = parent;
        mTransfers = transfers;
    }
    // data is the HttpService message content (Smart field 2 payload)
    void parse(const QByteArray& data, quint16 requestId);

private:
    void parseRawRequest(const QByteArray& data, HttpServiceRequest& request);
    void sendErrorResponse(quint16 requestId, bool webRequest);
    void sendRawResponse(quint16 requestId, const QByteArray& body,
        const QVector<QPair<QString, QString>>& headers, int httpStatus);
    void sendDataXferResponse(quint16 requestId, const QByteArray& body,
        const QVector<QPair<QString, QString>>& headers, int httpStatus);
    static QString headerValue(const HttpServiceRequest& request, const QString& name);
    static QString oauthResponseJson(const QString& path, const HttpServiceRequest& request);
    void handleWeatherRequest(quint16 requestId, const QUrl& url, const HttpServiceRequest& request);
    void handleEphemerisRequest(quint16 requestId, const QUrl& url, const HttpServiceRequest& request);
    static bool isGarminApiHost(const QString& host);
    static bool isWeatherHost(const QString& host);
    GarminDataTransferService* mTransfers = nullptr;
};
