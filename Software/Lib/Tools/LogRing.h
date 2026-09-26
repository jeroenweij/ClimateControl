/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace Tools
{
    // Small in-RAM ring of the most recent log lines, fed by every LOG_*
    // macro (Logger::Emit) and drained over the bus one line per Get by
    // Endpoint::DiagLog (Node-Message-Model-Spec.md §3) -- so a node's log can
    // be read remotely, not only from its debug UART.
    //
    // Each line is truncated to LineSize characters and stamped with the node's
    // uptime in seconds when it was logged, so the reader can place a backlog
    // in time. Fixed storage, no heap. Single-context: Push() and Pop() are
    // both called from the super-loop, never from an interrupt.
    namespace LogRing
    {
        constexpr uint8_t Lines    = 10;
        constexpr uint8_t LineSize = 32; // "<level>: <message>", truncated

        // Appends "<level>: <msg>". When the ring is full the oldest line is
        // overwritten and counted as lost.
        void Push(const char level, const char* const msg);

        // Copies the oldest unread line into 'out' (at most 'cap' bytes, no
        // terminator), sets 'uptimeSec' to when it was logged, and returns its
        // length; 0 when there is nothing to read (then 'uptimeSec' is now).
        // If lines were overwritten since the last read, a single
        // "~ <n> lost" line is returned first.
        size_t Pop(uint8_t* const out, const size_t cap, uint32_t& uptimeSec);

        // Lines currently buffered (a pending "lost" marker is not counted).
        uint8_t Buffered();

        void Clear();
    } // namespace LogRing
} // namespace Tools
