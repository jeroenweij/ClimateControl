/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "EEndpoint.h"
#include "EFirmware.h"
#include "EModuleType.h"
#include "EOperation.h"

#include "ThermostatMonitor.h"

using NodeLib::Endpoint;
using NodeLib::FirmwareOp;
using NodeLib::Id;
using NodeLib::MAX_NODES;
using NodeLib::Message;
using NodeLib::ModuleType;
using NodeLib::NodeMaster;
using NodeLib::Operation;

namespace
{
    // ThermostatFirmware Status Report: op(1) state(1) expectedOffset(4)
    // lastError(1) fwVersion(2 LE) -- ThermostatLink::FillOtaStatus().
    constexpr uint8_t statusLen = 9;

    // 0x63: controllerNodeId(1) linkUp(1) blState(1) fwMajor(1) fwMinor(1) uid[12]
    constexpr uint8_t thermostatStatusLen = 17;
} // namespace

ThermostatMonitor::SThermostat::SThermostat() :
    seenLink(false),
    seenFirmware(false),
    linkUp(false),
    blState(0),
    fwVersion(0),
    due(false)
{
}

bool ThermostatMonitor::SThermostat::Known() const
{
    return seenLink && seenFirmware;
}

ThermostatMonitor::ThermostatMonitor(NodeMaster& master) :
    master(master),
    thermostats{},
    pollCursor(0),
    pollTimer(pollStepMs),
    keepaliveTimer(keepaliveMs)
{
}

bool ThermostatMonitor::IsControllerNode(const uint8_t nodeId) const
{
    return nodeId >= 1 && nodeId < MAX_NODES && master.NodeModule(nodeId) == ModuleType::ControllerNode;
}

void ThermostatMonitor::Observe(const Message& m)
{
    if (m.id.operation != Operation::Report || !IsControllerNode(m.id.node))
    {
        return;
    }
    SThermostat& t = thermostats[m.id.node - 1];

    if (m.id.endpoint == Endpoint::RoomLink && m.len >= 1)
    {
        const bool linkUp = m.data[0] != 0;
        t.due             = t.due || !t.seenLink || linkUp != t.linkUp;
        t.linkUp          = linkUp;
        t.seenLink        = true;
    }
    else if (m.id.endpoint == Endpoint::ThermostatFirmware && m.len >= statusLen &&
             m.data[0] == static_cast<uint8_t>(FirmwareOp::Status))
    {
        const uint8_t  blState   = m.data[1];
        const uint16_t fwVersion = static_cast<uint16_t>(m.data[7] | (m.data[8] << 8));
        t.due                    = t.due || !t.seenFirmware || blState != t.blState || fwVersion != t.fwVersion;
        t.blState                = blState;
        t.fwVersion              = fwVersion;
        t.seenFirmware           = true;
    }
}

void ThermostatMonitor::Loop()
{
    if (pollTimer.Finished())
    {
        pollTimer.Start(pollStepMs);
        PollNext();
    }

    if (keepaliveTimer.Finished())
    {
        keepaliveTimer.Start(keepaliveMs);
        for (uint8_t nodeId = 1; nodeId < MAX_NODES; nodeId++)
        {
            SThermostat& t = thermostats[nodeId - 1];
            if (!master.NodeActive(nodeId))
            {
                t = SThermostat(); // gone from the bus -- start fresh when it returns
            }
            else if (t.Known())
            {
                t.due = true;
            }
        }
    }
}

void ThermostatMonitor::PollNext()
{
    for (uint8_t step = 0; step < MAX_NODES - 1; step++)
    {
        pollCursor = static_cast<uint8_t>(pollCursor % (MAX_NODES - 1) + 1); // 1..MAX_NODES-1, wrapping
        if (!master.NodeActive(pollCursor) || !IsControllerNode(pollCursor))
        {
            continue;
        }
        const SThermostat& t = thermostats[pollCursor - 1];
        if (t.seenFirmware && t.blState != 0)
        {
            continue; // a push is running -- its own Status Reports keep us current
        }
        master.QueueMessage(Message(Id(pollCursor, Endpoint::RoomLink, Operation::Get)));
        master.QueueMessage(Message(Id(pollCursor, Endpoint::ThermostatFirmware, Operation::Get)));
        return;
    }
}

void ThermostatMonitor::ResendAll()
{
    for (SThermostat& t : thermostats)
    {
        t.due = t.Known();
    }
}

bool ThermostatMonitor::NextStatus(Message& out)
{
    for (uint8_t nodeId = 1; nodeId < MAX_NODES; nodeId++)
    {
        SThermostat& t = thermostats[nodeId - 1];
        if (!t.due || !t.Known())
        {
            continue;
        }
        t.due = false;

        out         = Message(Id(0, Endpoint::ThermostatStatus, Operation::Report));
        out.data[0] = nodeId;
        out.data[1] = t.linkUp ? 1 : 0;
        out.data[2] = t.blState;
        out.data[3] = static_cast<uint8_t>(t.fwVersion >> 8);
        out.data[4] = static_cast<uint8_t>(t.fwVersion);
        for (uint8_t i = 5; i < thermostatStatusLen; i++)
        {
            out.data[i] = 0; // uid -- not carried by the ControllerNode
        }
        out.len = thermostatStatusLen;
        return true;
    }
    return false;
}
