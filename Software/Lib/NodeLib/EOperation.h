/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include "SStream.h"

#include <stdint.h>

namespace NodeLib
{
    // Flat verb enum -- see Spec/Node-Message-Model-Spec.md Sec4.
    //   0x0_  endpoint access verbs (Get/Set/Report/Ack/Nack)
    //   0x1_  transport verbs, only valid on Endpoint::Transport
    enum class Operation : uint8_t
    {
        Get    = 0x01,
        Set    = 0x02,
        Report = 0x03,
        Ack    = 0x04,
        Nack   = 0x05,

        Discover = 0x10, // master -> broadcast : enumerate nodes
        Announce = 0x11, // node   -> master    : presence (module type + UID)
        Poll     = 0x12, // master -> node      : grant transmit window
        Done     = 0x13, // node   -> master    : end of queued messages
    };

    inline std::stringstream& operator<<(std::stringstream& oStrStream, const Operation operation)
    {
        switch (operation)
        {
            case Operation::Get:
                oStrStream << "Get";
                break;
            case Operation::Set:
                oStrStream << "Set";
                break;
            case Operation::Report:
                oStrStream << "Report";
                break;
            case Operation::Ack:
                oStrStream << "Ack";
                break;
            case Operation::Nack:
                oStrStream << "Nack";
                break;
            case Operation::Discover:
                oStrStream << "Discover";
                break;
            case Operation::Announce:
                oStrStream << "Announce";
                break;
            case Operation::Poll:
                oStrStream << "Poll";
                break;
            case Operation::Done:
                oStrStream << "Done";
                break;
        }

        return oStrStream;
    }
} // namespace NodeLib
