/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "LogRing.h"
#include "Tick.h"

namespace
{
    char     lines[Tools::LogRing::Lines][Tools::LogRing::LineSize];
    uint8_t  lengths[Tools::LogRing::Lines];
    uint32_t stamps[Tools::LogRing::Lines]; // uptime seconds when logged
    uint8_t  head; // oldest unread line
    uint8_t  count; // buffered lines
    uint8_t  lost; // lines overwritten since the marker was last returned (saturates)

    // "~ <n> lost" into out; returns the length.
    size_t FormatLost(uint8_t* const out, const uint8_t n)
    {
        size_t i = 0;
        out[i++] = '~';
        out[i++] = ' ';
        if (n >= 100)
        {
            out[i++] = static_cast<uint8_t>('0' + n / 100);
        }
        if (n >= 10)
        {
            out[i++] = static_cast<uint8_t>('0' + (n / 10) % 10);
        }
        out[i++]                 = static_cast<uint8_t>('0' + n % 10);
        static const char tail[] = " lost";
        for (size_t t = 0; tail[t] != '\0'; t++)
        {
            out[i++] = static_cast<uint8_t>(tail[t]);
        }
        return i;
    }
} // namespace

void Tools::LogRing::Push(const char* const level, const char* const msg)
{
    if (count == Lines)
    {
        head = static_cast<uint8_t>((head + 1) % Lines);
        count--;
        if (lost < 255)
        {
            lost++;
        }
    }

    const uint8_t slot = static_cast<uint8_t>((head + count) % Lines);
    size_t        n    = 0;
    for (const char* p = level; *p != '\0' && n < LineSize; p++)
    {
        lines[slot][n++] = *p;
    }
    static const char sep[] = ": ";
    for (size_t s = 0; sep[s] != '\0' && n < LineSize; s++)
    {
        lines[slot][n++] = sep[s];
    }
    for (const char* p = msg; *p != '\0' && n < LineSize; p++)
    {
        lines[slot][n++] = *p;
    }
    lengths[slot] = static_cast<uint8_t>(n);
    stamps[slot]  = Hal::Tick::Millis() / 1000u;
    count++;
}

size_t Tools::LogRing::Pop(uint8_t* const out, const size_t cap, uint32_t& uptimeSec)
{
    uptimeSec = Hal::Tick::Millis() / 1000u;

    if (lost > 0 && cap >= 12) // "~ 255 lost" is 10 bytes
    {
        const size_t n = FormatLost(out, lost);
        lost           = 0;
        return n;
    }

    if (count == 0)
    {
        return 0;
    }

    uptimeSec      = stamps[head];
    const size_t n = lengths[head] < cap ? lengths[head] : cap;
    for (size_t i = 0; i < n; i++)
    {
        out[i] = static_cast<uint8_t>(lines[head][i]);
    }
    head = static_cast<uint8_t>((head + 1) % Lines);
    count--;
    return n;
}

uint8_t Tools::LogRing::Buffered()
{
    return count;
}

void Tools::LogRing::Clear()
{
    head  = 0;
    count = 0;
    lost  = 0;
}
