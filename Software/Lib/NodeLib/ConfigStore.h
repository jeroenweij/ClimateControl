/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include <stdint.h>

namespace NodeLib
{
    // Read-only view of the node's factory-provisioned identity: a single fixed
    // record at Board::Flash::ConfigBase, written once by the provisioning
    // J-Link step and never by firmware. See
    // Spec/Node-Flash-Layout-and-Bootloader-Spec.md Sec6.3.
    //
    // The NodeId is not configurable at runtime -- there is deliberately no
    // setter here and none on NodeLib::Node.
    class ConfigStore
    {
      public:
        enum class Module : uint8_t
        {
            Unknown         = 0,
            ControllerNode  = 1,
            TemperatureNode = 2,
            MainController  = 3,
            Thermostat      = 4,
        };

        // magic + CRC32 + NodeId-range check on the flash record. A node whose
        // record does not pass this reached the field un-provisioned (a factory
        // escape) and must not join the bus.
        static bool Valid();

        // Only meaningful when Valid() is true.
        static uint8_t        NodeId();
        static Module         GetModule();
        static const uint8_t* Settings(); // 16 bytes, interpreted per GetModule()
    };
} // namespace NodeLib
