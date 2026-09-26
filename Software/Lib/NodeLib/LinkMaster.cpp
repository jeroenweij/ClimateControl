/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "BoardPins.h"

#include "LinkMaster.h"

using NodeLib::Endpoint;
using NodeLib::LinkMaster;
using NodeLib::Message;
using NodeLib::Operation;

LinkMaster::LinkMaster() :
    Node(Hal::Uart::Instance::Usart2, Board::LinkUart),
    peerId(THERMOSTAT_NODE_ID),
    linkUp(false),
    peerInBootloader(false),
    sendOk(true),
    discovering(false),
    pollTimer(),
    linkTimer(),
    discoverTimer()
{
    nodeId = masterNodeId;
}

void LinkMaster::Init()
{
    Node::Init(); // master path: brings up USART2, skips the flash id read

    // One Discover so the peer's Announce tells us app vs. bootloader; repeated
    // periodically thereafter (see discoverTimer in Loop()).
    WriteMessage(Message(BROADCAST_NODE, Operation::Discover));
    discovering = true;

    discoverTimer.Start(discoverIntervalMs);

    pollTimer.Start(pollIntervalMs);
}

void LinkMaster::Loop()
{
    PumpRx();

    if (linkTimer.Finished())
    {
        linkUp           = false;
        peerInBootloader = false;
        if (handler)
        {
            handler->ConnectionLost();
        }
    }

    if (discoverTimer.Finished())
    {
        WriteMessage(Message(BROADCAST_NODE, Operation::Discover));
        discovering = true;
        discoverTimer.ReStart();
    }

    if (sendOk && !discovering && messagesQueued > 0)
    {
        flushQueue();
    }

    if (pollTimer.Finished())
    {
        WriteMessage(Message(peerId, Operation::Poll));
        sendOk = false;
        pollTimer.ReStart();
    }
}

void LinkMaster::NotePeerAlive()
{
    linkUp = true;
    linkTimer.Start(linkTimeoutMs);
}

void LinkMaster::HandleMasterMessage(const Message& m)
{
    NotePeerAlive();

    if (m.id.endpoint == Endpoint::Transport)
    {
        switch (m.id.operation)
        {
            case Operation::Announce:
                // Bootloader Announce carries a state byte at data[1]; the app's
                // is length 1. A non-zero state means "in the bootloader".
                peerInBootloader = (m.len >= 2 && m.data[1] != 0);
                discovering      = false;
                break;
            case Operation::Done:
                sendOk = true;
                break;
            default:
                break;
        }
        return;
    }

    // Room* / System* / Firmware Status etc. -- straight to the ControllerNode.
    if (handler)
    {
        handler->ReceivedMessage(m);
    }
}

void LinkMaster::SendToPeer(const Endpoint endpoint, const Operation op, const uint8_t* const data, const uint8_t len)
{
    QueueMessage(Id(peerId, endpoint, op), data, len);
}

void LinkMaster::GetFromPeer(const Endpoint endpoint)
{
    QueueMessage(Id(peerId, endpoint, Operation::Get), nullptr, 0);
}

bool LinkMaster::LinkUp() const
{
    return linkUp;
}

uint8_t LinkMaster::PeerId() const
{
    return peerId;
}

bool LinkMaster::PeerInBootloader() const
{
    return peerInBootloader;
}
