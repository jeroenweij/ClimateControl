/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include "Id.h"

#include <stdint.h>

namespace NodeLib
{
    class IVariableHandler
    {
      public:
        virtual void ReceivedMessage(const Message& message) = 0;
        virtual void ConnectionLost()                        = 0;
    };
} // namespace NodeLib
