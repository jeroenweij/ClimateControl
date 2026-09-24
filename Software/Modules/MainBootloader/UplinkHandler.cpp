/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "Tick.h"

#include "../MainController/Secrets.h"
#include "UplinkHandler.h"

using NodeLib::Endpoint;
using NodeLib::Id;
using NodeLib::Message;
using NodeLib::Operation;

namespace
{
    void PackU16(uint8_t* const out, const uint16_t value)
    {
        out[0] = static_cast<uint8_t>(value);
        out[1] = static_cast<uint8_t>(value >> 8);
    }

    void PackU32(uint8_t* const out, const uint32_t value)
    {
        out[0] = static_cast<uint8_t>(value);
        out[1] = static_cast<uint8_t>(value >> 8);
        out[2] = static_cast<uint8_t>(value >> 16);
        out[3] = static_cast<uint8_t>(value >> 24);
    }

    bool IsRelayedEndpoint(const Endpoint endpoint)
    {
        const uint8_t value = static_cast<uint8_t>(endpoint);
        return value >= 0x10 && value <= 0x5F;
    }
} // namespace

UplinkHandler::UplinkHandler(Boot::Firmware& firmware) :
    firmware(firmware),
    outboundQueue(),
    nina(),
    config{Secrets::WifiSsid, Secrets::WifiPassword, Secrets::ServerHost, static_cast<uint16_t>(Secrets::ServerPort)},
    link(nina, config, *this, outboundQueue, outboundQueueSize)
{
}

void UplinkHandler::Init()
{
    link.Init();
}

void UplinkHandler::Loop()
{
    link.Loop();
}

void UplinkHandler::BuildHello(Message& hello)
{
    // fwVersion 0 = "the bootloader" (MainController-Server-Link-Spec.md §5);
    // no bus, so no nodes.
    uint8_t payload[23];
    PackU16(&payload[0], 0);
    PackU32(&payload[2], Hal::Tick::Millis() / 1000u);
    payload[6] = 0;
    for (size_t i = 0; i < sizeof(Secrets::UplinkToken); i++)
    {
        payload[7 + i] = Secrets::UplinkToken[i];
    }

    hello     = Message(Id(0, Endpoint::UplinkHello, Operation::Report));
    hello.len = sizeof(payload);
    for (uint8_t i = 0; i < hello.len; i++)
    {
        hello.data[i] = payload[i];
    }
}

void UplinkHandler::OnFrame(const Message& message)
{
    if (IsRelayedEndpoint(message.id.endpoint))
    {
        // No bus while resident in the bootloader -- nothing to relay to.
        return;
    }

    if (message.id.endpoint == Endpoint::OtaControl)
    {
        firmware.OnControl(message);
    }
    else if (message.id.endpoint == Endpoint::OtaData)
    {
        firmware.OnData(message);
    }

    Message reply;
    if (firmware.PopReply(reply))
    {
        link.Send(reply);
    }
}
