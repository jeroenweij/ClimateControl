/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include <stdint.h>

#include "ConfigStore.h"

// Test control over the factory-provisioned identity that NodeLib::Node reads
// in Init(). Defaults after Reset(): a valid, provisioned TemperatureNode at
// NodeId 1.
namespace FakeConfig
{
    void Reset();
    void SetValid(bool valid);
    void SetNodeId(uint8_t nodeId);
    void SetModule(NodeLib::ConfigStore::Module module);
} // namespace FakeConfig
