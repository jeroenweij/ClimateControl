/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "EEndpoint.h"
#include "EOperation.h"

#include "SupplyTemp.h"

using NodeLib::Endpoint;
using NodeLib::Message;
using NodeLib::Operation;

namespace
{
    int16_t ReadI16(const uint8_t* const p)
    {
        return static_cast<int16_t>(static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8));
    }
} // namespace

SupplyTemp::SupplyTemp() :
    value(0),
    valid(false),
    staleTimer()
{
}

void SupplyTemp::Snoop(const Message& m)
{
    if (m.id.endpoint != Endpoint::SupplyTemp || m.id.operation != Operation::Report || m.len < 2)
    {
        return;
    }
    value = ReadI16(m.data);
    valid = true;
    staleTimer.Start(staleTimeoutMs);
}

void SupplyTemp::Loop()
{
    if (valid && staleTimer.Finished())
    {
        valid = false;
    }
}

bool SupplyTemp::Valid() const
{
    return valid;
}

int16_t SupplyTemp::CentiDegC() const
{
    return value;
}
