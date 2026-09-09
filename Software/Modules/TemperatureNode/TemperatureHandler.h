/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include "INodeHandler.h"
#include "Node.h"

#include "DuctChannel.h"

// TemperatureNode application logic: two duct probes on the RS485 bus.
//
//   OneWire1 (PA5) -- "incoming" / return air  -> Endpoint::ReturnTemp
//   OneWire2 (PA4) -- "outgoing" / supply air  -> Endpoint::SupplyTemp
//
// (mapping per TemperatureNode-Spec.md §4.1). Every TemperatureNode endpoint is
// read-only -- the node only measures (spec §2). Transport / System* / Firmware /
// Diagnostics* are serviced inside NodeLib and never reach here.
class TemperatureHandler : public NodeLib::INodeHandler
{
  public:
    explicit TemperatureHandler(NodeLib::Node& node);

    void Init();
    void Loop();

    void ReceivedMessage(const NodeLib::Message& message) override;
    void ConnectionLost() override;
    void FillStatus(NodeLib::SystemStatus& status) override;

  private:
    // Endpoint::SensorStatus bitfield -- per-sensor present/valid.
    enum SensorStatusBit : uint8_t
    {
        ReturnSensorValid = 1u << 0,
        SupplySensorValid = 1u << 1,
    };

    // Endpoint::SystemStatus errorFlags bits contributed by this module.
    enum ErrorBit : uint16_t
    {
        ReturnSensorFault = 1u << 0,
        SupplySensorFault = 1u << 1,
    };

    uint8_t SensorStatus() const;
    void    ReportSensorStatus();

    NodeLib::Node& node;

    DuctChannel returnChannel;
    DuctChannel supplyChannel;

    uint8_t reportedSensorStatus;
    bool    sensorStatusReported;
};
