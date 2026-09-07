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

Crc::Crc()
{
    handle.Instance                     = CRC;
    handle.Init.DefaultPolynomialUse    = DEFAULT_POLYNOMIAL_DISABLE;
    handle.Init.GeneratingPolynomial    = 0x1021;
    handle.Init.CRCLength               = CRC_POLYLENGTH_16B;
    handle.Init.DefaultInitValueUse     = DEFAULT_INIT_VALUE_DISABLE;
    handle.Init.InitValue               = 0xFFFF;
    handle.Init.InputDataInversionMode  = CRC_INPUTDATA_INVERSION_NONE;
    handle.Init.OutputDataInversionMode = CRC_OUTPUTDATA_INVERSION_DISABLE;
    handle.InputDataFormat              = CRC_INPUTDATA_FORMAT_BYTES;

    HAL_CRC_Init(&handle);
}

uint16_t Crc::Compute(const uint8_t* const data, const size_t len)
{
    return static_cast<uint16_t>(HAL_CRC_Calculate(&handle, reinterpret_cast<const uint32_t*>(data), len));
}
