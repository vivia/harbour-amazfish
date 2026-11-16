#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QMap>

// -----------------------------------------------------------------------------
// DataTransferService, Smart field 7 (gdi_data_transfer_service.proto).
//
// A RawRequest with useDataXfer gets a body by id instead of inline bytes.
// The watch then pulls the body in chunks it sizes itself. Like Gadgetbridge's
// DataTransferHandler the whole body is kept in RAM until the watch has it.
// -----------------------------------------------------------------------------
class GarminDataTransferService {
public:
    // Remember the body, answer with the id to put in RawResponse.xferData.
    quint32 registerData(const QByteArray& data);

    // data is the DataTransferService message content (Smart field 7 payload).
    // Returns the answer, a DataTransferService with a DataDownloadResponse
    // (field 2), ready for Smart field 7.
    QByteArray handleDownloadRequest(const QByteArray& data);

    // Registered bodies do not survive a disconnect.
    void clear();

private:
    QMap<quint32, QByteArray> mById;
    quint32 mNextId = 1;
};
