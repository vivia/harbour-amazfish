#include "garminfiletransferdatamessage.h"

void GarminFileTransferDataMessage::parse(const QByteArray& data)
{
    qDebug() << Q_FUNC_INFO << "Garmin: parsing file transfer data, chunk size" << data.size();
    if (data.size() < 7) {
        return;
    }

    FileTransferDataChunk msg;
    msg.flags = quint8(data[0]);
    msg.crc = u16le(data, 1);
    msg.dataOffset = i32le(data, 3);
    msg.bytes = data.mid(7);

    if (mCommunicator) {
        mCommunicator->onFileTransferDataReceived(msg);
    }
}
