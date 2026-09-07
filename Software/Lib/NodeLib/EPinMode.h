/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include "SStream.h"

namespace NodeLib
{
    enum class PinMode
    {
        DIGITAL_IN,
        DIGITAL_OUT,
        ANALOG_IN,
        SERVO,
        NOT_CONFIGURED,
    };

    inline std::stringstream& operator<<(std::stringstream& oStrStream, const PinMode pinmode)
    {
        switch (pinmode)
        {
            case PinMode::NOT_CONFIGURED:
                oStrStream << "NOT_CONFIGURED";
                break;
            case PinMode::DIGITAL_IN:
                oStrStream << "DIGITAL_IN";
                break;
            case PinMode::DIGITAL_OUT:
                oStrStream << "DIGITAL_OUT";
                break;
            case PinMode::ANALOG_IN:
                oStrStream << "ANALOG_IN";
                break;
            case PinMode::SERVO:
                oStrStream << "SERVO";
                break;
        }

        return oStrStream;
    }
} // namespace NodeLib
