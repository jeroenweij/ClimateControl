/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "FakeConfigStore.h"

using NodeLib::ConfigStore;
using NodeLib::ModuleType;

namespace
{
    bool       valid        = true;
    uint8_t    nodeId       = 1;
    ModuleType module       = ModuleType::TemperatureNode;
    uint8_t    settings[16] = {};
} // namespace

namespace FakeConfig
{
    void Reset()
    {
        valid  = true;
        nodeId = 1;
        module = ModuleType::TemperatureNode;
        for (uint8_t i = 0; i < 16; i++)
        {
            settings[i] = 0;
        }
    }

    void SetValid(const bool isValid)
    {
        valid = isValid;
    }

    void SetNodeId(const uint8_t id)
    {
        nodeId = id;
    }

    void SetModule(const ModuleType mod)
    {
        module = mod;
    }
} // namespace FakeConfig

bool ConfigStore::Valid()
{
    return valid;
}

uint8_t ConfigStore::NodeId()
{
    return nodeId;
}

ModuleType ConfigStore::GetModule()
{
    return module;
}

const uint8_t* ConfigStore::Settings()
{
    return settings;
}
