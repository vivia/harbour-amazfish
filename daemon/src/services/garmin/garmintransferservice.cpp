#include "garmintransferservice.h"

#include "protobuftools.h"

#include <QtCore/QDebug>

// Field numbers from gdi_data_transfer_service.proto.
// DataTransferService: dataDownloadRequest=1, dataDownloadResponse=2.
// DataDownloadRequest: id=1, offset=2, maxChunkSize=3.
// DataDownloadResponse: status=1, id=2, offset=3, payload=4.
static const quint32 kFieldDtrRequest = 1;
static const quint32 kFieldDtrResponse = 2;
static const quint32 kFieldReqId = 1;
static const quint32 kFieldReqOffset = 2;
static const quint32 kFieldReqMaxChunkSize = 3;

// DataTransferService.Status: SUCCESS=1, INVALID_ID=2, INVALID_OFFSET=3.
static const quint64 kStatusSuccess = 1;
static const quint64 kStatusInvalidId = 2;
static const quint64 kStatusInvalidOffset = 3;

// Used when a request has no maxChunkSize. The Lily 2 Active always sets one,
// this only keeps such a request from asking for a whole megabyte at once.
static const quint64 kDefaultMaxChunkSize = 16384;

// An idle watch leaves its ephemeris transfers unclaimed. Drop the oldest so a
// long session cannot add one body per cycle forever.
static const int kMaxRegisteredTransfers = 4;

quint32 GarminDataTransferService::registerData(const QByteArray& data)
{
    const quint32 id = mNextId++;
    mById.insert(id, data);
    while (mById.size() > kMaxRegisteredTransfers) {
        mById.erase(mById.begin());
    }
    qDebug().noquote() << QStringLiteral("Garmin: Data ready for transfer, id %1 size %2").arg(id).arg(data.size());
    return id;
}

void GarminDataTransferService::clear()
{
    mById.clear();
}

QByteArray GarminDataTransferService::handleDownloadRequest(const QByteArray& data)
{
    bool haveId = false;
    bool haveOffset = false;
    quint64 id = 0;
    quint64 offset = 0;
    quint64 maxChunkSize = 0;

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

        if (fieldNum != kFieldDtrRequest || wireType != 2) {
            continue;
        }
        int reqCursor = 0;
        while (reqCursor < fieldData.size()) {
            quint32 reqFieldNum;
            quint8 reqWireType;
            QByteArray reqData;
            int reqNext;
            if (!parseField(fieldData, reqCursor, reqFieldNum, reqWireType, reqData, reqNext)) {
                break;
            }
            reqCursor = reqNext;
            quint64 value;
            int valueLen;
            if (reqWireType != 0 || !decodeVarint(reqData, value, valueLen)) {
                continue;
            }
            if (reqFieldNum == kFieldReqId) {
                id = value;
                haveId = true;
            } else if (reqFieldNum == kFieldReqOffset) {
                offset = value;
                haveOffset = true;
            } else if (reqFieldNum == kFieldReqMaxChunkSize) {
                maxChunkSize = value;
            }
        }
        // Only the first dataDownloadRequest is answered.
        break;
    }

    QByteArray answer;
    if (!haveId || !haveOffset) {
        qWarning().noquote() << QStringLiteral("Garmin: Data download request without id or offset, refusing");
        return answer;
    }

    // Status, id and offset are required in the proto, so they are always sent.
    QByteArray inner;
    encodeFieldKey(inner, 1, 0);
    QByteArray payload;

    const QMap<quint32, QByteArray>::iterator it = mById.find(quint32(id));
    if (it == mById.end()) {
        qWarning().noquote() << QStringLiteral("Garmin: Data download for unknown id %1").arg(id);
        encodeVarint(inner, kStatusInvalidId);
    } else if (offset >= quint64(it->size())) {
        qWarning().noquote() << QStringLiteral("Garmin: Data download id %1 at invalid offset %2 of %3")
                                    .arg(id)
                                    .arg(offset)
                                    .arg(it->size());
        encodeVarint(inner, kStatusInvalidOffset);
    } else {
        if (maxChunkSize == 0) {
            maxChunkSize = kDefaultMaxChunkSize;
        }
        const int chunkSize = int(qMin(maxChunkSize, quint64(it->size()) - offset));
        payload = it.value().mid(int(offset), chunkSize);
        encodeVarint(inner, kStatusSuccess);
    }
    encodeFieldKey(inner, 2, 0);
    encodeVarint(inner, id);
    encodeFieldKey(inner, 3, 0);
    encodeVarint(inner, offset);
    if (!payload.isEmpty()) {
        encodeFieldKey(inner, 4, 2);
        encodeVarint(inner, quint64(payload.size()));
        inner.append(payload);
    }

    encodeFieldKey(answer, kFieldDtrResponse, 2);
    encodeVarint(answer, quint64(inner.size()));
    answer.append(inner);
    return answer;
}
