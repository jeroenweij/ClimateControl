/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "Publisher.h"

using NodeLib::Endpoint;
using NodeLib::Id;
using NodeLib::Message;
using NodeLib::Operation;
using NodeLib::Publisher;

Publisher::Publisher() :
    entries{},
    count(0),
    keepaliveCursor(0),
    wasConnected(false),
    keepaliveTimer()
{
}

bool Publisher::Add(const Endpoint endpoint, const uint8_t size, const uint16_t minChange)
{
    if (count >= maxPublished || (size != 1 && size != 2) || Find(endpoint) != nullptr)
    {
        return false;
    }
    SEntry& e       = entries[count++];
    e.endpoint      = endpoint;
    e.size          = size;
    e.minChange     = minChange == 0 ? 1 : minChange;
    e.value         = 0;
    e.reported      = 0;
    e.known         = false;
    e.reportedValid = false;
    e.keepaliveDue  = false;
    return true;
}

Publisher::SEntry* Publisher::Find(const Endpoint endpoint)
{
    for (uint8_t i = 0; i < count; i++)
    {
        if (entries[i].endpoint == endpoint)
        {
            return &entries[i];
        }
    }
    return nullptr;
}

void Publisher::Update(const Endpoint endpoint, const int32_t value)
{
    SEntry* const e = Find(endpoint);
    if (e != nullptr)
    {
        e->value = value;
        e->known = true;
    }
}

void Publisher::Clear(const Endpoint endpoint)
{
    SEntry* const e = Find(endpoint);
    if (e != nullptr)
    {
        e->known         = false;
        e->reportedValid = false; // report it afresh once it is known again
        e->keepaliveDue  = false;
    }
}

bool Publisher::Due(const SEntry& e) const
{
    if (!e.known)
    {
        return false;
    }
    if (!e.reportedValid || e.keepaliveDue)
    {
        return true;
    }
    const int32_t delta = e.value - e.reported;
    return delta >= e.minChange || delta <= -static_cast<int32_t>(e.minChange);
}

bool Publisher::Next(const bool connected, const uint8_t nodeId, Message& out)
{
    if (!connected)
    {
        if (wasConnected)
        {
            // Lost the master: whatever it had may be gone -- send it all
            // again once it is back.
            for (uint8_t i = 0; i < count; i++)
            {
                entries[i].reportedValid = false;
                entries[i].keepaliveDue  = false;
            }
            keepaliveTimer.Stop();
        }
        wasConnected = false;
        return false;
    }
    wasConnected = true;

    if (count > 0)
    {
        if (!keepaliveTimer.IsRunning())
        {
            keepaliveTimer.Start(keepaliveMs / count); // (re)connected -- everything goes out below anyway
        }
        else if (keepaliveTimer.Finished())
        {
            entries[keepaliveCursor].keepaliveDue = true;
            keepaliveCursor                       = static_cast<uint8_t>((keepaliveCursor + 1) % count);
            keepaliveTimer.Start(keepaliveMs / count);
        }
    }

    for (uint8_t i = 0; i < count; i++)
    {
        SEntry& e = entries[i];
        if (!Due(e))
        {
            continue;
        }
        out         = Message(Id(nodeId, e.endpoint, Operation::Report));
        out.data[0] = static_cast<uint8_t>(e.value);
        if (e.size == 2)
        {
            out.data[1] = static_cast<uint8_t>(e.value >> 8);
        }
        out.len         = e.size;
        e.reported      = e.value;
        e.reportedValid = true;
        e.keepaliveDue  = false;
        return true;
    }
    return false;
}
