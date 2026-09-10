/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "BoardPins.h"
#include "Logger.h"

#include "ConfigStore.h"
#include "LinkMaster.h"

using NodeLib::ConfigStore;
using NodeLib::Endpoint;
using NodeLib::LinkMaster;
using NodeLib::Message;
using NodeLib::Operation;

LinkMaster::LinkMaster(const uint32_t baudRate) :
    Node(baudRate, Hal::Uart::Instance::Usart2, Board::LinkUart),
    peerId(0),
    linkUp(false),
    peerInBootloader(false),
    pollTimer(),
    linkTimer()
{
    nodeId = masterNodeId;
}

void LinkMaster::Init()
{
    Node::Init(); // master path: brings up USART2, skips the flash id read

    // The Thermostat shares this ControllerNode's provisioned id (§5.2.1).
    peerId = ConfigStore::Valid() ? ConfigStore::NodeId() : 0;
    LOG_INFO("LinkMaster peer id " << peerId);

    // One Discover so the peer's Announce tells us app vs. bootloader.
    WriteMessage(Message(BROADCAST_NODE, Operation::Discover));

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

    if (pollTimer.Finished())
    {
        flushQueue(); // push any injected Set/Get to the peer
        WriteMessage(Message(peerId, Operation::Poll));
        pollTimer.Start(pollIntervalMs);
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
                break;
            case Operation::Done:
                break; // liveness only -- handled by NotePeerAlive()
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

void LinkMaster::PollPeerNow()
{
    pollTimer.Start(0);
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
