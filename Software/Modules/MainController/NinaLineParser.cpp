/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include <string.h>

#include "NinaLineParser.h"

namespace
{
    bool StartsWith(const char* const line, const size_t length, const char* const prefix)
    {
        const size_t prefixLen = strlen(prefix);
        return length >= prefixLen && memcmp(line, prefix, prefixLen) == 0;
    }

    // Decimal integer starting at line[offset]. No sign, no overflow checks --
    // peer handles are small non-negative indices.
    int ParseInt(const char* const line, const size_t length, const size_t offset)
    {
        int value = 0;
        for (size_t i = offset; i < length && line[i] >= '0' && line[i] <= '9'; i++)
        {
            value = value * 10 + (line[i] - '0');
        }
        return value;
    }
} // namespace

NinaLineParser::NinaLineParser() :
    lineBuffer{},
    lineLength(0),
    lastPeerHandle(-1),
    eventQueue{},
    eventHead(0),
    eventCount(0)
{
}

void NinaLineParser::Reset()
{
    lineLength = 0;
    eventHead  = 0;
    eventCount = 0;
}

int NinaLineParser::LastPeerHandle() const
{
    return lastPeerHandle;
}

void NinaLineParser::QueueEvent(const Event event)
{
    if (eventCount >= eventQueueSize)
    {
        return; // drop -- queue full, oldest events already being drained
    }
    const size_t tail = (eventHead + eventCount) % eventQueueSize;
    eventQueue[tail]  = event;
    eventCount++;
}

bool NinaLineParser::NextEvent(Event& event)
{
    if (eventCount == 0)
    {
        return false;
    }
    event     = eventQueue[eventHead];
    eventHead = (eventHead + 1) % eventQueueSize;
    eventCount--;
    return true;
}

void NinaLineParser::ClassifyLine(const char* const line, const size_t length, LineResult& result)
{
    if (length == 0)
    {
        return; // the blank separator line between a command echo and its response
    }

    if (length == 2 && line[0] == 'O' && line[1] == 'K')
    {
        result = LineResult::Ok;
        return;
    }
    if (StartsWith(line, length, "ERROR"))
    {
        result = LineResult::Error;
        return;
    }
    if (StartsWith(line, length, "+UDCP:"))
    {
        lastPeerHandle = ParseInt(line, length, 6);
        return;
    }
    if (!StartsWith(line, length, "+UU"))
    {
        return; // command echo, or a quoted response value we don't need
    }

    // Unsolicited event codes (MainController-Server-Link-Spec.md §3).
    if (StartsWith(line, length, "+UUWLE"))
    {
        QueueEvent(Event::LinkUp);
    }
    else if (StartsWith(line, length, "+UUWLD"))
    {
        QueueEvent(Event::LinkDown);
    }
    else if (StartsWith(line, length, "+UUNU"))
    {
        QueueEvent(Event::NetworkUp);
    }
    else if (StartsWith(line, length, "+UUND"))
    {
        QueueEvent(Event::NetworkDown);
    }
    else if (StartsWith(line, length, "+UUDPC"))
    {
        QueueEvent(Event::PeerConnected);
    }
    else if (StartsWith(line, length, "+UUDPD"))
    {
        QueueEvent(Event::PeerDisconnected);
    }
    // else: an unsolicited code this driver doesn't act on -- ignored.
}

NinaLineParser::LineResult NinaLineParser::FeedByte(const uint8_t byte)
{
    if (byte != '\n')
    {
        // Accumulate, dropping '\r' and anything past the buffer (a
        // malformed/oversized line just gets truncated, not overrun).
        if (byte != '\r' && lineLength < lineBufferSize)
        {
            lineBuffer[lineLength] = static_cast<char>(byte);
            lineLength++;
        }
        return LineResult::None;
    }

    LineResult result = LineResult::None;
    ClassifyLine(lineBuffer, lineLength, result);
    lineLength = 0;
    return result;
}
