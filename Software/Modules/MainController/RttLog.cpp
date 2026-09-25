/*************************************************************
 * Created by J. Weij
 *
 * MainController's Tools::Logger console for the bench, built only with the
 * CMake option CC_MC_LOG_RTT (MainController-Server-Link-Spec.md §5.1): each
 * line goes to SEGGER RTT up-channel 0, read over the J-Link already used for
 * flashing (JLinkRTTClient / JLinkRTTLogger). The operator's log is the
 * MainLog push over the uplink; this is only for a unit that never connects.
 *
 * A minimal RTT control block rather than SEGGER's full library: one
 * up-channel, one idle down-channel, non-blocking skip mode (a line that does
 * not fit in the free space is dropped whole), so it can never stall the
 * super-loop. The layout below is the one the J-Link looks for in RAM (the
 * "SEGGER RTT" id), and the _SEGGER_RTT symbol lets it find the block from
 * the ELF directly. Overrides the weak default in Lib/Tools/Logger.cpp.
 *************************************************************/

#include <stdint.h>
#include <string.h>

#include "Logger.h"

namespace
{
    struct RttUpBuffer
    {
        const char*       name;
        char*             buffer;
        uint32_t          size;
        volatile uint32_t wrOff; // written by the target
        volatile uint32_t rdOff; // written by the J-Link
        uint32_t          flags;
    };

    struct RttDownBuffer
    {
        const char*       name;
        char*             buffer;
        uint32_t          size;
        volatile uint32_t wrOff; // written by the J-Link
        volatile uint32_t rdOff; // written by the target
        uint32_t          flags;
    };

    struct RttControlBlock
    {
        char          id[16];
        int32_t       maxUpBuffers;
        int32_t       maxDownBuffers;
        RttUpBuffer   up[1];
        RttDownBuffer down[1];
    };

    const uint32_t upSize          = 512;
    const uint32_t downSize        = 16;
    const uint32_t modeNoBlockSkip = 0;

    char upBuffer[upSize];
    char downBuffer[downSize];

    void Barrier()
    {
        __asm volatile("dmb" ::: "memory");
    }
} // namespace

extern "C"
{
    RttControlBlock _SEGGER_RTT;
}

namespace
{
    void Init()
    {
        RttControlBlock& cb = _SEGGER_RTT;
        cb.maxUpBuffers     = 1;
        cb.maxDownBuffers   = 1;
        cb.up[0]            = {"Terminal", upBuffer, upSize, 0, 0, modeNoBlockSkip};
        cb.down[0]          = {"Terminal", downBuffer, downSize, 0, 0, modeNoBlockSkip};

        // The id goes in last, back half first, so the J-Link never finds a
        // half-initialised block (same order as SEGGER's own SEGGER_RTT_Init).
        Barrier();
        memcpy(&cb.id[7], "RTT", 4);
        Barrier();
        memcpy(&cb.id[0], "SEGGER", 6);
        Barrier();
        cb.id[6] = ' ';
        Barrier();
    }

    uint32_t Free(const RttUpBuffer& ring)
    {
        const uint32_t rd = ring.rdOff;
        const uint32_t wr = ring.wrOff;
        return rd > wr ? rd - wr - 1 : ring.size - (wr - rd) - 1;
    }

    void Append(RttUpBuffer& ring, uint32_t& wr, const char* const text, const size_t length)
    {
        for (size_t i = 0; i < length; i++)
        {
            ring.buffer[wr] = text[i];
            wr              = (wr + 1 == ring.size) ? 0 : wr + 1;
        }
    }
} // namespace

void Tools::Logger::Write(const char* const level, const char* const msg)
{
    static bool initialised = false;
    if (!initialised)
    {
        Init();
        initialised = true;
    }

    RttUpBuffer& ring      = _SEGGER_RTT.up[0];
    const size_t levelLen  = strlen(level);
    const size_t msgLen    = strlen(msg);
    const size_t lineBytes = levelLen + 2 + msgLen + 2;
    if (lineBytes > Free(ring))
    {
        return; // no J-Link reading, or it fell behind -- skip the whole line
    }

    uint32_t wr = ring.wrOff;
    Append(ring, wr, level, levelLen);
    Append(ring, wr, ": ", 2);
    Append(ring, wr, msg, msgLen);
    Append(ring, wr, "\r\n", 2);
    Barrier(); // the bytes land before the J-Link sees the new offset
    ring.wrOff = wr;
}
