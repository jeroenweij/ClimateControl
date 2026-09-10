/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include "SStream.h"

namespace NodeLib
{
    // The addressable thing on a node. Flat enum, organised in high-nibble
    // blocks -- see Spec/Node-Message-Model-Spec.md Sec3. Replaces the
    // IO-expander ChannelId.
    enum class Endpoint : uint8_t
    {
        Transport = 0x00, // operation field carries Discover / Announce / Poll / Done

        // 0x1_  system -- every node; NodeLib-owned
        SystemInfo    = 0x10, // RO  module, hwRev, fwVersion, uid[12]
        SystemStatus  = 0x11, // RO  state, uptimeSec, errorFlags, resetCause
        SystemControl = 0x12, // WO  1=reset->app  2=reset->bootloader  3=identify

        // 0x2_  firmware -- every node; data[0] = FirmwareOp
        Firmware = 0x20,

        // ControllerNode only: "act on my paired Thermostat over the link".
        // data[0] = FirmwareOp, same as Firmware. App-delivered (NOT NodeLib-
        // handled) -- the ControllerNode terminates it and re-originates a link
        // transaction. See ControllerNode-Thermostat-Link-Spec.md §5.4.
        ThermostatFirmware = 0x22,

        // 0x3_  application, ControllerNode
        DamperTarget = 0x30, // RW  uint8 %
        DamperActual = 0x31, // RO  uint8 %
        DamperMode   = 0x32, // RW  enum: 0 closed 1 open 2 auto 3 manual

        // 0x3_  application, TemperatureNode
        SupplyTemp   = 0x38, // RO  int16 centi-degC
        ReturnTemp   = 0x39, // RO  int16 centi-degC
        SensorStatus = 0x3A, // RO  bitfield

        // 0x4_  room -- relayed from the paired Thermostat (ControllerNode only)
        RoomSetpoint = 0x40, // RO / RW*  int16 centi-degC
        RoomTemp     = 0x41, // RO  int16 centi-degC
        RoomHumidity = 0x42, // RO  uint16 centi-%RH
        RoomMode     = 0x43, // RO  enum (same coding as DamperMode)
        RoomLink     = 0x44, // RO  0 down / 1 up

        // 0x5_  diagnostics -- every node; NodeLib-owned
        DiagRxCounters = 0x50, // RO  rxFrames(4) crcErrors(4) resyncs(4) interByteTimeouts(4)
        DiagTxCounters = 0x51, // RO  txFrames(4) queueDrops(4)
        DiagLastError  = 0x52, // RO  code(1) uptimeAtFault(4) context(2)
        DiagLog        = 0x53, // RO  Get -> Report next buffered log line
        DiagReset      = 0x54, // WO  Set -> clear the counters
    };

    inline std::stringstream& operator<<(std::stringstream& oStrStream, const Endpoint endpoint)
    {
        switch (endpoint)
        {
            case Endpoint::Transport:
                oStrStream << "Transport";
                break;
            case Endpoint::SystemInfo:
                oStrStream << "SystemInfo";
                break;
            case Endpoint::SystemStatus:
                oStrStream << "SystemStatus";
                break;
            case Endpoint::SystemControl:
                oStrStream << "SystemControl";
                break;
            case Endpoint::Firmware:
                oStrStream << "Firmware";
                break;
            case Endpoint::ThermostatFirmware:
                oStrStream << "ThermostatFirmware";
                break;
            case Endpoint::DamperTarget:
                oStrStream << "DamperTarget";
                break;
            case Endpoint::DamperActual:
                oStrStream << "DamperActual";
                break;
            case Endpoint::DamperMode:
                oStrStream << "DamperMode";
                break;
            case Endpoint::SupplyTemp:
                oStrStream << "SupplyTemp";
                break;
            case Endpoint::ReturnTemp:
                oStrStream << "ReturnTemp";
                break;
            case Endpoint::SensorStatus:
                oStrStream << "SensorStatus";
                break;
            case Endpoint::RoomSetpoint:
                oStrStream << "RoomSetpoint";
                break;
            case Endpoint::RoomTemp:
                oStrStream << "RoomTemp";
                break;
            case Endpoint::RoomHumidity:
                oStrStream << "RoomHumidity";
                break;
            case Endpoint::RoomMode:
                oStrStream << "RoomMode";
                break;
            case Endpoint::RoomLink:
                oStrStream << "RoomLink";
                break;
            case Endpoint::DiagRxCounters:
                oStrStream << "DiagRxCounters";
                break;
            case Endpoint::DiagTxCounters:
                oStrStream << "DiagTxCounters";
                break;
            case Endpoint::DiagLastError:
                oStrStream << "DiagLastError";
                break;
            case Endpoint::DiagLog:
                oStrStream << "DiagLog";
                break;
            case Endpoint::DiagReset:
                oStrStream << "DiagReset";
                break;
        }

        return oStrStream;
    }
} // namespace NodeLib
