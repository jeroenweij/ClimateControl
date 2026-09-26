/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include "SStream.h"

namespace NodeLib
{
    // The types of module on the bus
    enum class ModuleType : uint8_t
    {
        Unknown         = 0x00,
        ControllerNode  = 0x01,
        TemperatureNode = 0x02,
        MainController  = 0x03,
        Thermostat      = 0x04,
    };

    inline std::stringstream& operator<<(std::stringstream& oStrStream, const ModuleType moduleType)
    {
        switch (moduleType)
        {
            case ModuleType::Unknown:
                oStrStream << "Unknown";
                break;
            case ModuleType::MainController:
                oStrStream << "MainController";
                break;
            case ModuleType::ControllerNode:
                oStrStream << "ControllerNode";
                break;
            case ModuleType::TemperatureNode:
                oStrStream << "TemperatureNode";
                break;
            case ModuleType::Thermostat:
                oStrStream << "Thermostat";
                break;
            default:
                // Deliberate: the value comes straight off the wire (Announce),
                // so an out-of-range one (newer firmware, corrupt byte) must still
                // log as its raw number rather than as nothing.
                oStrStream << "Module(" << static_cast<unsigned int>(moduleType) << ")";
                break;
        }

        return oStrStream;
    }
} // namespace NodeLib
