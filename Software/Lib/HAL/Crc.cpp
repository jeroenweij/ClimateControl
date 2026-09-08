/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "stm32g0xx_hal.h"

#include "Crc.h"

using Hal::Crc;

namespace
{
    CRC_HandleTypeDef handle = {};
}

extern "C" void HAL_CRC_MspInit(CRC_HandleTypeDef*)
{
    __HAL_RCC_CRC_CLK_ENABLE();
}

Crc::Crc(const Poly poly)
{
    handle.Instance = CRC;

    if (poly == Poly::Ccitt16)
    {
        handle.Init.DefaultPolynomialUse    = DEFAULT_POLYNOMIAL_DISABLE;
        handle.Init.GeneratingPolynomial    = 0x1021;
        handle.Init.CRCLength               = CRC_POLYLENGTH_16B;
        handle.Init.DefaultInitValueUse     = DEFAULT_INIT_VALUE_DISABLE;
        handle.Init.InitValue               = 0xFFFF;
        handle.Init.InputDataInversionMode  = CRC_INPUTDATA_INVERSION_NONE;
        handle.Init.OutputDataInversionMode = CRC_OUTPUTDATA_INVERSION_DISABLE;
    }
    else
    {
        // STM32-native CRC-32: default poly 0x04C11DB7, default init 0xFFFFFFFF,
        // no reflection, no final xor.
        handle.Init.DefaultPolynomialUse    = DEFAULT_POLYNOMIAL_ENABLE;
        handle.Init.CRCLength               = CRC_POLYLENGTH_32B;
        handle.Init.DefaultInitValueUse     = DEFAULT_INIT_VALUE_ENABLE;
        handle.Init.InputDataInversionMode  = CRC_INPUTDATA_INVERSION_NONE;
        handle.Init.OutputDataInversionMode = CRC_OUTPUTDATA_INVERSION_DISABLE;
    }
    handle.InputDataFormat = CRC_INPUTDATA_FORMAT_BYTES;

    HAL_CRC_Init(&handle);
}

uint16_t Crc::Compute(const uint8_t* const data, const size_t len)
{
    return static_cast<uint16_t>(HAL_CRC_Calculate(&handle, reinterpret_cast<const uint32_t*>(data), len));
}

uint32_t Crc::Compute32(const uint8_t* const data, const size_t len)
{
    return HAL_CRC_Calculate(&handle, reinterpret_cast<const uint32_t*>(data), len);
}
