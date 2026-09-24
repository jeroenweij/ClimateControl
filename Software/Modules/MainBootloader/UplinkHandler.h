/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include "NinaLink.h"

#include "Firmware.h"
#include "NinaUart.h"

// MainController's bootloader-side uplink: the same NinaLink connection the
// application uses (Lib/Nina), with the bootloader's frame handling on top --
// OtaControl / OtaData drive Boot::Firmware, everything else is ignored. There
// is no bus here, so relayed-range frames have nowhere to go.
class UplinkHandler : public NinaLinkHandler
{
  public:
    explicit UplinkHandler(Boot::Firmware& firmware);

    void Init();
    void Loop();

    // NinaLinkHandler
    void BuildHello(NodeLib::Message& hello) override;
    void OnFrame(const NodeLib::Message& message) override;

  private:
    // Host tests (test/NinaBringUpTests.cpp).
    friend struct UplinkHandlerTestAccess;

    Boot::Firmware& firmware;

    // Just OtaControl/OtaData replies and the odd keepalive -- one request is
    // answered before the next is read, so a handful of slots is plenty.
    static const uint8_t outboundQueueSize = 4;
    NodeLib::Message     outboundQueue[outboundQueueSize];

    Boot::NinaUart nina;
    NinaLinkConfig config;
    NinaLink       link;
};
