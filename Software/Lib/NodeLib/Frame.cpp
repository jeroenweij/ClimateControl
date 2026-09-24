/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "Logger.h"

#include "Frame.h"

using NodeLib::Frame;
using NodeLib::Message;

namespace
{
    const uint8_t frameStart[2] = {0xEE, 0x42};

    // Spec recommends ~2-3 byte-periods (~175-260us at 115200 baud) for the
    // inter-byte timeout (§5). Tools::DelayTimer only has millisecond
    // resolution (Hal::Tick is a 1ms SysTick), so this is coarser than the
    // spec's suggestion -- a hardware timer would be needed for tighter
    // timing. Still bounds how long a single corruption event can wedge the
    // parser, which is the property the spec actually cares about.
    const Tools::time_a interByteTimeoutMs = 2;
} // namespace

Frame::Frame(Hal::Crc& crc) :
    crc(crc),
    state(State::Sync0),
    len(0),
    dataIndex(0),
    headerAndData{},
    headerIndex(0),
    receivedCrc(0),
    interByteTimer(),
    counters{}
{
}

void Frame::ResetToSync()
{
    state = State::Sync0;
    interByteTimer.Stop();
}

void Frame::Update()
{
    if (state != State::Sync0 && interByteTimer.Finished())
    {
        LOG_DEBUG("Frame inter-byte timeout, resync");
        counters.interByteTimeouts++;
        ResetToSync();
    }
}

bool Frame::FeedByte(const uint8_t byte, Message& message)
{
    if (state != State::Sync0)
    {
        interByteTimer.Start(interByteTimeoutMs);
    }

    switch (state)
    {
        case State::Sync0:
            if (byte == frameStart[0])
            {
                state = State::Sync1;
                interByteTimer.Start(interByteTimeoutMs);
            }
            break;

        case State::Sync1:
            if (byte == frameStart[1])
            {
                state = State::Len;
            }
            else if (byte != frameStart[0])
            {
                state = State::Sync0;
            }
            break;

        case State::Len:
            if (byte > MAX_DATA)
            {
                // Max-frame guard (spec §5 point 3) -- LEN can't be trusted, abandon.
                counters.resyncs++;
                ResetToSync();
                break;
            }
            len         = byte;
            headerIndex = 0;
            dataIndex   = 0;
            state       = State::Header;
            break;

        case State::Header:
            headerAndData[headerIndex] = byte;
            headerIndex++;
            if (headerIndex == 3)
            {
                state = (len == 0) ? State::Crc0 : State::Data;
            }
            break;

        case State::Data:
            headerAndData[3 + dataIndex] = byte;
            dataIndex++;
            if (dataIndex == len)
            {
                state = State::Crc0;
            }
            break;

        case State::Crc0:
            receivedCrc = byte;
            state       = State::Crc1;
            break;

        case State::Crc1:
        {
            receivedCrc = static_cast<uint16_t>(receivedCrc | (static_cast<uint16_t>(byte) << 8));

            const uint8_t  headerAndDataLen = static_cast<uint8_t>(3 + len);
            const uint16_t computedCrc      = crc.Compute(headerAndData, headerAndDataLen);

            ResetToSync();

            if (computedCrc != receivedCrc)
            {
                LOG_DEBUG("Frame CRC mismatch, dropped");
                counters.crcErrors++;
                return false;
            }

            message.id.node      = headerAndData[0];
            message.id.endpoint  = static_cast<Endpoint>(headerAndData[1]);
            message.id.operation = static_cast<Operation>(headerAndData[2]);
            message.len          = len;
            for (uint8_t i = 0; i < len; i++)
            {
                message.data[i] = headerAndData[3 + i];
            }

            counters.frames++;
            return true;
        }
    }

    return false;
}

size_t Frame::Encode(const Message& message, uint8_t* const out) const
{
    out[0] = frameStart[0];
    out[1] = frameStart[1];
    out[2] = message.len;
    out[3] = message.id.node;
    out[4] = static_cast<uint8_t>(message.id.endpoint);
    out[5] = static_cast<uint8_t>(message.id.operation);
    for (uint8_t i = 0; i < message.len; i++)
    {
        out[6 + i] = message.data[i];
    }

    const uint8_t  headerAndDataLen = static_cast<uint8_t>(3 + message.len);
    const uint16_t computedCrc      = crc.Compute(&out[3], headerAndDataLen);

    size_t index = static_cast<size_t>(3 + headerAndDataLen);
    out[index++] = static_cast<uint8_t>(computedCrc & 0xFF);
    out[index++] = static_cast<uint8_t>(computedCrc >> 8);
    return index;
}

bool Frame::Write(Hal::Uart& uart, const Message& message) const
{
    // Built into one buffer and queued with a single WriteBytes() call so the
    // whole frame is enqueued atomically -- Hal::Uart::WriteBytes() rejects
    // (queues nothing) rather than partially accept, so several separate calls
    // could leave a truncated, unparseable frame sitting in the ring buffer if
    // a later call didn't fit.
    uint8_t      wireBytes[MaxFrameBytes];
    const size_t length = Encode(message, wireBytes);
    return uart.WriteBytes(wireBytes, length);
}
