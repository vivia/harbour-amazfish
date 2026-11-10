#include "garminhttpservicemessage.h"

#include "protobuftools.h"

#include <QtCore/QDebug>
#include <QtCore/QStringList>
#include <QtCore/QUrl>

// Field numbers from gdi_http_service.proto.
// HttpService: webRequest=1, webResponse=2, rawRequest=5, rawResponse=6, showURLNotification=11.
// RawRequest: url=1, method=3, body=4, header=5 (repeated Header key=1 value=2), rawBody=7.
static const quint32 kFieldWebRequest = 1;
static const quint32 kFieldWebResponse = 2;
static const quint32 kFieldRawRequest = 5;
static const quint32 kFieldRawResponse = 6;

// RawResponse: status=1 (Status enum), httpStatus=2, body=3, xferData=4, header=5.
// HttpService.Status.OK is 100 on the wire, not 0.
static const quint32 kFieldRrStatus = 1;
static const quint32 kFieldRrHttpStatus = 2;
static const quint32 kFieldRrBody = 3;
static const quint32 kFieldRrHeader = 5;
static const quint64 kStatusOk = 100;

static QString methodToString(quint64 method)
{
    switch (method) {
    case 0:
        return QStringLiteral("UNKNOWN_METHOD");
    case 1:
        return QStringLiteral("GET");
    case 2:
        return QStringLiteral("PUT");
    case 3:
        return QStringLiteral("POST");
    case 4:
        return QStringLiteral("DELETE");
    case 5:
        return QStringLiteral("PATCH");
    case 6:
        return QStringLiteral("HEAD");
    default:
        return QStringLiteral("INVALID(%1)").arg(method);
    }
}

// -----------------------------------------------------------------------------
// Parse the content of one RawRequest (HttpService field 5)
// -----------------------------------------------------------------------------
void GarminHttpServiceMessage::parseRawRequest(const QByteArray& data, HttpServiceRequest& request)
{
    int cursor = 0;
    while (cursor < data.size()) {
        quint32 fieldNum;
        quint8 wireType;
        QByteArray fieldData;
        int nextCursor;
        if (!parseField(data, cursor, fieldNum, wireType, fieldData, nextCursor)) {
            break;
        }
        cursor = nextCursor;

        if (fieldNum == 1 && wireType == 2) {
            request.url = QString::fromUtf8(fieldData);
        } else if (fieldNum == 3 && wireType == 0) {
            quint64 method;
            int methodLen;
            if (decodeVarint(fieldData, method, methodLen)) {
                request.method = methodToString(method);
            }
        } else if (fieldNum == 7 && wireType == 2) {
            request.rawBody = QString::fromUtf8(fieldData);
        } else if (fieldNum == 5 && wireType == 2) {
            // Header: key=1, value=2
            QString key;
            QString value;
            int headerCursor = 0;
            while (headerCursor < fieldData.size()) {
                quint32 hFieldNum;
                quint8 hWireType;
                QByteArray hData;
                int hNext;
                if (!parseField(fieldData, headerCursor, hFieldNum, hWireType, hData, hNext)) {
                    break;
                }
                headerCursor = hNext;
                if (hFieldNum == 1 && hWireType == 2) {
                    key = QString::fromUtf8(hData);
                } else if (hFieldNum == 2 && hWireType == 2) {
                    value = QString::fromUtf8(hData);
                }
            }
            request.headers.append(qMakePair(key, value));
        }
    }
}

// -----------------------------------------------------------------------------
// Fake OAuth answers, following Gadgetbridge OauthInterceptor
// -----------------------------------------------------------------------------
static bool isOauthPath(const QString& path)
{
    return path.startsWith(QLatin1String("/api/oauth/"))
        || path.startsWith(QLatin1String("/oauth/"))
        || path.startsWith(QLatin1String("/oauthTokenExchangeService/"));
}

// Same scope set Gadgetbridge sends: the union of its Swim 2, Venu 3 and
// Enduro 3 lists, duplicates dropped, first occurrence kept.
static const QStringList& oauthScopes()
{
    static const char* const scopes[] = {
        // Swim 2
        "GCS_EPHEMERIS_SONY_READ",
        // Venu 3
        "GCS_CIQ_APPSTORE_MOBILE_READ",
        "GCS_EMERGENCY_ASSISTANCE_CREATE",
        "GCS_GEOLOCATION_ELEVATION_READ",
        "GCS_IMAGE_READ",
        "GCS_LIVETRACK_FIT_CREATE",
        "GCS_LIVETRACK_FIT_READ",
        "GCS_LIVETRACK_FIT_UPDATE",
        "OMT_GOLF_SUBSCRIPTION_READ",
        "OMT_SUBSCRIPTION_READ",
        // Enduro 3
        "CSE_CDS_ACCOUNT_READ",
        "GCS_DEVICE_INSTRUCTION_OUTDOOR_READ",
        "GCS_IMAGE_STORAGE_READ",
        "GCS_LIVE_EVENT_SHARING_CREATE",
        "GCS_LTE_SIGNAL_UPDATE",
        "GCS_MESSAGING_FITNESS_CREATE",
        "GCS_MESSAGING_FITNESS_READ",
        "GCS_MESSAGING_FITNESS_UPDATE",
        "GCS_STOCKS_READ",
        "GCS_TIDE_READ",
        "GCS_WEATHER_RACEDAY_READ",
        "MARINE_SERVER_ACCESS",
        "MARINE_SERVER_CHARTS_SUBSCRIPTION_READ",
        "MARINE_SERVER_CHARTS_SUBSCRIPTION_WRITE",
        "OMT_BIRDSEYE_READ",
        "OMT_OUTDOOR_MAP_SUBSCRIPTION_CREATE",
        "OMT_OUTDOOR_MAP_SUBSCRIPTION_DELETE",
        "OMT_OUTDOOR_MAP_SUBSCRIPTION_READ",
        "YAR_BILLING_SUBSCRIBER_READ",
        "YAR_INREACH_HERMES_READ",
        "YAR_INREACH_IRIS_CREATE",
        "YAR_INREACH_IRIS_READ",
        "YAR_INREACH_IRIS_UPDATE",
        "YAR_INREACH_VOICE_EVENT_CREATE",
    };
    static QStringList list;
    if (list.isEmpty()) {
        for (const char* const scope : scopes) {
            list << QLatin1String(scope);
        }
    }
    return list;
}

static QString quoted(const QString& value)
{
    return QLatin1Char('"') + value + QLatin1Char('"');
}

// connectToIT / connect_exchange: Gadgetbridge writes the fields of its
// AuthorizationResponse in declaration order, refreshTokenExpiresIn as a string.
static QString authorizationResponseJson()
{
    return QStringLiteral("{\"accessToken\":%1,\"tokenType\":\"Bearer\",\"refreshToken\":%2,"
                          "\"expiresIn\":7776000,\"scope\":\"%3\",\"refreshTokenExpiresIn\":\"31536000\","
                          "\"customerId\":%4}")
        .arg(quoted(uuidString()), quoted(uuidString()),
            oauthScopes().join(QLatin1Char(' ')), quoted(uuidString()));
}

// /api/oauth/token and /oauth/refresh_token/token: Gadgetbridge keeps the
// refresh token from the request body when it can, and writes RefreshResponse
// fields in declaration order (customerId stays camelCase there).
static QString refreshResponseJson(const HttpServiceRequest& request)
{
    QString refreshToken;
    for (const QString& param : request.rawBody.split(QLatin1Char('&'))) {
        const QStringList parts = param.split(QLatin1Char('='));
        if (parts.size() == 2 && parts.at(0) == QLatin1String("refresh_token")) {
            refreshToken = parts.at(1);
            break;
        }
    }
    if (refreshToken.isEmpty()) {
        qWarning().noquote() << QStringLiteral("Garmin: oauth refresh request has no refresh_token, using a new one");
        refreshToken = uuidString();
    }

    return QStringLiteral("{\"access_token\":%1,\"token_type\":\"Bearer\",\"expires_in\":7776000,"
                          "\"scope\":\"%2\",\"refresh_token\":%3,\"refresh_token_expires_in\":\"31536000\","
                          "\"customerId\":%4}")
        .arg(quoted(uuidString()), oauthScopes().join(QLatin1Char(' ')),
            quoted(refreshToken), quoted(uuidString()));
}

QString GarminHttpServiceMessage::oauthResponseJson(const QString& path, const HttpServiceRequest& request)
{
    if (path == QLatin1String("/oauthTokenExchangeService/connectToIT")
        || path == QLatin1String("/oauth/connect_exchange/token")) {
        return authorizationResponseJson();
    }
    if (path == QLatin1String("/api/oauth/token") || path == QLatin1String("/oauth/refresh_token/token")) {
        return refreshResponseJson(request);
    }
    return QString();
}

// -----------------------------------------------------------------------------
// Parse the content of HttpService (Smart field 2)
//
// OAuth token exchanges are answered with fake keys. The rest gets
// Gadgetbridge's error status so the watch stops retrying, and the url is
// logged so the requested endpoints can be studied.
// -----------------------------------------------------------------------------
void GarminHttpServiceMessage::parse(const QByteArray& data, quint16 requestId)
{
    bool haveRawRequest = false;
    bool haveWebRequest = false;
    HttpServiceRequest request;

    int cursor = 0;
    while (cursor < data.size()) {
        quint32 fieldNum;
        quint8 wireType;
        QByteArray fieldData;
        int nextCursor;
        if (!parseField(data, cursor, fieldNum, wireType, fieldData, nextCursor)) {
            break;
        }
        cursor = nextCursor;

        if (fieldNum == kFieldRawRequest && wireType == 2) {
            haveRawRequest = true;
            parseRawRequest(fieldData, request);
        } else if (fieldNum == kFieldWebRequest && wireType == 2) {
            // WebRequest.url is field 1
            haveWebRequest = true;
            quint32 urlFieldNum;
            quint8 urlWireType;
            QByteArray urlData;
            int urlNext;
            if (parseField(fieldData, 0, urlFieldNum, urlWireType, urlData, urlNext)
                && urlFieldNum == 1 && urlWireType == 2) {
                request.url = QString::fromUtf8(urlData);
            }
        }
    }

    qDebug().noquote() << QStringLiteral("Garmin: HttpService request id %1: %2 %3")
                              .arg(requestId)
                              .arg(request.method.isEmpty() ? QStringLiteral("?") : request.method)
                              .arg(request.url);
    for (const QPair<QString, QString>& header : request.headers) {
        qDebug().noquote() << QStringLiteral("Garmin: HttpService header: %1: %2")
                                  .arg(header.first, header.second);
    }

    if (haveRawRequest) {
        const QUrl url(request.url);
        const QString path = url.path();
        if (isOauthPath(path)) {
            if (request.method == QLatin1String("POST")) {
                const QString json = oauthResponseJson(path, request);
                if (!json.isEmpty()) {
                    qDebug().noquote() << QStringLiteral("Garmin: Fake OAuth response for %1, %2 bytes")
                                              .arg(path)
                                              .arg(json.toUtf8().size());
                    sendRawResponse(requestId, json.toUtf8());
                    return;
                }
            }
            qWarning().noquote() << QStringLiteral("Garmin: No fake OAuth handler for %1 %2")
                                        .arg(request.method, path);
        }
        sendErrorResponse(requestId, false);
    } else if (haveWebRequest) {
        sendErrorResponse(requestId, true);
    } else {
        qWarning().noquote() << QStringLiteral("Garmin: HttpService message has no request, ignoring");
    }
}

// -----------------------------------------------------------------------------
// Answer like Gadgetbridge does for blocked or failed requests: an error status,
// no body. RawResponse: status=1 UNKNOWN_STATUS(0).
// WebResponse: status=1 UNKNOWN_STATUS(0), httpStatus=2 = 0.
// -----------------------------------------------------------------------------
void GarminHttpServiceMessage::sendErrorResponse(quint16 requestId, bool webRequest)
{
    QByteArray inner;
    encodeFieldKey(inner, 1, 0);
    encodeVarint(inner, 0); // Status: UNKNOWN_STATUS
    if (webRequest) {
        encodeFieldKey(inner, 2, 0);
        encodeVarint(inner, 0); // httpStatus: 0
    }

    // HttpService: field 6 RawResponse, or field 2 WebResponse
    QByteArray httpService;
    encodeFieldKey(httpService, webRequest ? kFieldWebResponse : kFieldRawResponse, 2);
    encodeVarint(httpService, quint64(inner.size()));
    httpService.append(inner);

    // Smart.field2 = HttpService
    QByteArray smart;
    encodeFieldKey(smart, 2, 2);
    encodeVarint(smart, quint64(httpService.size()));
    smart.append(httpService);

    // sendProtobufResponse adds the ProtobufResponse framing and chunks the
    // payload when it is larger than one chunk.
    if (mCommunicator) {
        mCommunicator->sendProtobufResponse(smart, requestId);
    }
}

// -----------------------------------------------------------------------------
// Success answer to a RawRequest: Status OK (100), HTTP 200, full body inline.
// Like Gadgetbridge we do not split the body ourselves; the protobuf layer
// chunks the response and the watch drives the continuation with status
// messages. Header follows Gadgetbridge: Content-Type only.
// -----------------------------------------------------------------------------
void GarminHttpServiceMessage::sendRawResponse(quint16 requestId, const QByteArray& body)
{
    QByteArray header;
    appendStringField(header, 1, QStringLiteral("Content-Type"));
    appendStringField(header, 2, QStringLiteral("application/json"));

    QByteArray inner; // RawResponse
    encodeFieldKey(inner, kFieldRrStatus, 0);
    encodeVarint(inner, kStatusOk);
    encodeFieldKey(inner, kFieldRrHttpStatus, 0);
    encodeVarint(inner, 200);
    encodeFieldKey(inner, kFieldRrBody, 2);
    encodeVarint(inner, quint64(body.size()));
    inner.append(body);
    encodeFieldKey(inner, kFieldRrHeader, 2);
    encodeVarint(inner, quint64(header.size()));
    inner.append(header);

    QByteArray httpService;
    encodeFieldKey(httpService, kFieldRawResponse, 2);
    encodeVarint(httpService, quint64(inner.size()));
    httpService.append(inner);

    // Smart.field2 = HttpService
    QByteArray smart;
    encodeFieldKey(smart, 2, 2);
    encodeVarint(smart, quint64(httpService.size()));
    smart.append(httpService);

    if (mCommunicator) {
        mCommunicator->sendProtobufResponse(smart, requestId);
    }
}
