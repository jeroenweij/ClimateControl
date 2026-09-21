/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "I2c.h"

#include "FakeI2c.h"

using Hal::I2c;
using Hal::I2cPins;

namespace
{
    struct Device
    {
        bool    used;
        uint8_t address;
        bool    nack;
        uint8_t readQueue[16];
        size_t  readLen;
        size_t  readPos;
        uint8_t written[1200];
        size_t  writtenLen;
    };

    const int maxDevices = 4;
    Device    devices[maxDevices];

    Device& DeviceFor(const uint8_t address)
    {
        for (int i = 0; i < maxDevices; i++)
        {
            if (devices[i].used && devices[i].address == address)
            {
                return devices[i];
            }
        }
        for (int i = 0; i < maxDevices; i++)
        {
            if (!devices[i].used)
            {
                devices[i]         = Device{};
                devices[i].used    = true;
                devices[i].address = address;
                return devices[i];
            }
        }
        return devices[0];
    }
} // namespace

namespace FakeI2c
{
    void ResetAll()
    {
        for (int i = 0; i < maxDevices; i++)
        {
            devices[i] = Device{};
        }
    }

    void SetNack(const uint8_t address7, const bool nack)
    {
        DeviceFor(address7).nack = nack;
    }

    void QueueRead(const uint8_t address7, const uint8_t* const data, const size_t len)
    {
        Device& d = DeviceFor(address7);
        for (size_t i = 0; i < len && d.readLen < sizeof(d.readQueue); i++)
        {
            d.readQueue[d.readLen++] = data[i];
        }
    }

    size_t WrittenLen(const uint8_t address7)
    {
        return DeviceFor(address7).writtenLen;
    }

    const uint8_t* Written(const uint8_t address7)
    {
        return DeviceFor(address7).written;
    }

    void ClearWritten(const uint8_t address7)
    {
        DeviceFor(address7).writtenLen = 0;
    }
} // namespace FakeI2c

I2c::I2c(const I2cPins& pins) :
    pins(pins)
{
}

void I2c::Init()
{
}

bool I2c::Write(const uint8_t address7, const uint8_t* const data, const uint8_t len)
{
    Device& d = DeviceFor(address7);
    if (d.nack)
    {
        return false;
    }
    for (uint8_t i = 0; i < len && d.writtenLen < sizeof(d.written); i++)
    {
        d.written[d.writtenLen++] = data[i];
    }
    return true;
}

bool I2c::Read(const uint8_t address7, uint8_t* const data, const uint8_t len)
{
    Device& d = DeviceFor(address7);
    if (d.nack)
    {
        return false;
    }
    for (uint8_t i = 0; i < len; i++)
    {
        data[i] = d.readPos < d.readLen ? d.readQueue[d.readPos++] : 0xFFu;
    }
    return true;
}
