/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include <stdint.h>

// Line-oriented classifier for u-connectXpress AT responses/URCs
// (MainController-Server-Link-Spec.md §3). Pure byte-in logic, no hardware
// dependency -- NinaAt owns the real Hal::Uart and feeds bytes here, the same
// split NodeLib::Frame uses relative to Node.
//
// u-connectXpress lines are CR/LF-terminated; blank lines (the one between a
// command echo and its response, e.g. "AT+GMM\r\n\r\n\"NINA-W152\"\r\nOK\r\n")
// are skipped. Only the handful of lines this driver actually needs are
// classified -- "OK"/"ERROR" complete an outstanding command, "+UDCP:<n>"
// captures the peer handle, "+UU..." lines become Events. Anything else
// (command echoes, quoted response values we don't need) is ignored.
class NinaLineParser
{
  public:
    NinaLineParser();

    enum class LineResult
    {
        None, // no line completed, or a completed line wasn't Ok/Error
        Ok,
        Error,
    };

    // Feed one received byte. Call once per byte read from the NINA UART
    // while in command mode (never in data mode -- there are no AT lines
    // once ATO succeeds).
    LineResult FeedByte(const uint8_t byte);

    void Reset();

    // Value from the most recently seen "+UDCP:<n>" line (AT+UDCP's peer
    // handle). -1 if none seen yet.
    int LastPeerHandle() const;

    enum class Event
    {
        LinkDown, // +UUWLD
        NetworkUp, // +UUNU
        NetworkDown, // +UUND
        PeerConnected, // +UUDPC
        PeerDisconnected, // +UUDPD
    };

    // Drains one queued unsolicited event, oldest first. Returns false (and
    // leaves 'event' untouched) once the queue is empty.
    bool NextEvent(Event& event);

  private:
    static const size_t lineBufferSize = 96;
    static const size_t eventQueueSize = 8;

    void ClassifyLine(const char* const line, const size_t length, LineResult& result);
    void QueueEvent(const Event event);

    char   lineBuffer[lineBufferSize];
    size_t lineLength;

    int lastPeerHandle;

    Event  eventQueue[eventQueueSize];
    size_t eventHead;
    size_t eventCount;
};
