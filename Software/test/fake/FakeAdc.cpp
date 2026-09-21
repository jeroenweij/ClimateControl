/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "Adc.h"

#include "FakeAdc.h"

using Hal::Adc;

namespace
{
    uint16_t value = 0;
} // namespace

namespace FakeAdc
{
    void Reset()
    {
        value = 0;
    }

    void SetValue(const uint16_t counts)
    {
        value = counts;
    }
} // namespace FakeAdc

Adc::Adc(const uint8_t channel) :
    channel(channel)
{
}

uint16_t Adc::Read()
{
    return value;
}
