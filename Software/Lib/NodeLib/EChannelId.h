/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include "SStream.h"

namespace NodeLib
{
    enum class ChannelId : uint8_t
    {
        INTERNAL_MSG,
        DIGITAL_1,
        DIGITAL_2,
        DIGITAL_3,
        DIGITAL_4,
        DIGITAL_5,
        DIGITAL_6,
        ANALOG_1,
        ANALOG_2,
        ANALOG_3,
        SERVO_1,
        SERVO_2,
        MOSFET_1,
        MOSFET_2,
    };

    inline std::stringstream& operator<<(std::stringstream& oStrStream, const NodeLib::ChannelId ChannelId)
    {
        switch (ChannelId)
        {
            case NodeLib::ChannelId::INTERNAL_MSG:
                oStrStream << "INTERNAL_MSG";
                break;
            case NodeLib::ChannelId::DIGITAL_1:
                oStrStream << "DIGITAL_1";
                break;
            case NodeLib::ChannelId::DIGITAL_2:
                oStrStream << "DIGITAL_2";
                break;
            case NodeLib::ChannelId::DIGITAL_3:
                oStrStream << "DIGITAL_3";
                break;
            case NodeLib::ChannelId::DIGITAL_4:
                oStrStream << "DIGITAL_4";
                break;
            case NodeLib::ChannelId::DIGITAL_5:
                oStrStream << "DIGITAL_5";
                break;
            case NodeLib::ChannelId::DIGITAL_6:
                oStrStream << "DIGITAL_6";
                break;
            case NodeLib::ChannelId::ANALOG_1:
                oStrStream << "ANALOG_1";
                break;
            case NodeLib::ChannelId::ANALOG_2:
                oStrStream << "ANALOG_2";
                break;
            case NodeLib::ChannelId::ANALOG_3:
                oStrStream << "ANALOG_3";
                break;
            case NodeLib::ChannelId::SERVO_1:
                oStrStream << "SERVO_1";
                break;
            case NodeLib::ChannelId::SERVO_2:
                oStrStream << "SERVO_2";
                break;
            case NodeLib::ChannelId::MOSFET_1:
                oStrStream << "MOSFET_1";
                break;
            case NodeLib::ChannelId::MOSFET_2:
                oStrStream << "MOSFET_2";
                break;
        }

        return oStrStream;
    }
} // namespace NodeLib
