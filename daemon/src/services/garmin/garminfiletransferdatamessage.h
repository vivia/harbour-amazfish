#ifndef GARMINFILETRANSFERDATAMESSAGE_H
#define GARMINFILETRANSFERDATAMESSAGE_H

#include "communicator_v2.h"
#include "garmingfdimessage.h"

#include <QObject>

class GarminFileTransferDataMessage : public GarminGfdiMessage {
public:
    GarminFileTransferDataMessage(CommunicatorV2* parent)
    {
        mCommunicator = parent;
    }
    void parse(const QByteArray& data);
};

#endif // GARMINFILETRANSFERDATAMESSAGE_H
