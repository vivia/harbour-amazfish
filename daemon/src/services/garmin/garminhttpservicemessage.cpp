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
// Parse the content of HttpService (Smart field 2)
//
// Every request gets Gadgetbridge's error status so the watch stops retrying,
// and the url is logged so the requested endpoints can be studied.
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
