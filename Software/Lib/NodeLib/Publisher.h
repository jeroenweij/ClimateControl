/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include <stdint.h>

#include "DelayTimer.h"

#include "EEndpoint.h"
#include "Id.h"

namespace NodeLib
{
    // Minimum change that re-reports a value, shared by every module
    // (Node-Message-Model-Spec.md §6.1). Enums, modes, setpoints and link
    // state use the default of 1 -- any change.
    constexpr uint16_t TempMinChange     = 10; // 0.1 degC, centi-degC values
    constexpr uint16_t HumidityMinChange = 100; // 1 %RH, centi-%RH values

    // The on-change + keepalive reporting a node does by itself
    // (Node-Message-Model-Spec.md §6.1), for simple 1- or 2-byte values.
    // Owned by Node, driven through Node::AddPublished() /
    // PublishIfChanged() / ClearPublished(); Node pulls the Reports that are
    // due with Next() once per Loop().
    //
    // A value is reported when it first becomes known, whenever it moves by
    // at least its minChange from what was last reported, as a keepalive
    // about every keepaliveMs (staggered: one entry per keepaliveMs / count,
    // so one poll window never carries every value at once), and again
    // after the master was lost. Nothing is reported while the master isn't
    // polling -- values just go out once it is back. Fixed-size table, no heap.
    class Publisher
    {
      public:
        static const uint8_t  maxPublished = 12;
        static const uint32_t keepaliveMs  = 60000;

        Publisher();

        // Declares a published endpoint: 1- or 2-byte little-endian payload
        // (the value's low bytes; signed or unsigned alike). False if the
        // table is full or the size is not 1 or 2.
        bool Add(const Endpoint endpoint, const uint8_t size, const uint16_t minChange);

        // The endpoint's current value -- call every loop, it only reports
        // what's due. An endpoint that wasn't Add()ed is ignored.
        void Update(const Endpoint endpoint, const int32_t value);

        // The value is no longer known (e.g. a probe went missing): stop
        // reporting it, including keepalives, until the next Update().
        void Clear(const Endpoint endpoint);

        // The next Report that is due, addressed from nodeId; false if none.
        // 'connected' is whether the master is polling this node right now.
        bool Next(const bool connected, const uint8_t nodeId, Message& out);

      private:
        struct SEntry
        {
            Endpoint endpoint;
            uint8_t  size;
            uint16_t minChange;
            int32_t  value;
            int32_t  reported;
            bool     known; // value set by Update(), not Clear()ed since
            bool     reportedValid; // 'reported' went out on this connection
            bool     keepaliveDue;
        };

        SEntry* Find(const Endpoint endpoint);
        bool    Due(const SEntry& e) const;

        SEntry            entries[maxPublished];
        uint8_t           count;
        uint8_t           keepaliveCursor;
        bool              wasConnected;
        Tools::DelayTimer keepaliveTimer;
    };
} // namespace NodeLib
