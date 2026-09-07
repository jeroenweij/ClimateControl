/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include <stdint.h>

#include "SStream.h"

namespace NodeLib
{
    enum class Operation : uint8_t
    {
        GET,
        SET,
        SETPWM,
        VALUE,
        ERROR,
        SETMODE,
        DETECTNODES,
        HELLOWORLD,
        SENDQ,
        ENDOFQ
    };

    inline std::stringstream& operator<<(std::stringstream& oStrStream, const NodeLib::Operation Operation)
    {
        switch (Operation)
        {
            case NodeLib::Operation::GET:
                oStrStream << "GET";
                break;
            case NodeLib::Operation::SET:
                oStrStream << "SET";
                break;
            case NodeLib::Operation::SETPWM:
                oStrStream << "SETPWM";
                break;
            case NodeLib::Operation::VALUE:
                oStrStream << "VALUE";
                break;
            case NodeLib::Operation::ERROR:
                oStrStream << "ERROR";
                break;
            case NodeLib::Operation::SETMODE:
                oStrStream << "SETMODE";
                break;
            case NodeLib::Operation::DETECTNODES:
                oStrStream << "DETECTNODES";
                break;
            case NodeLib::Operation::HELLOWORLD:
                oStrStream << "HELLOWORLD";
                break;
            case NodeLib::Operation::SENDQ:
                oStrStream << "SENDQ";
                break;
            case NodeLib::Operation::ENDOFQ:
                oStrStream << "ENDOFQ";
                break;
        }

        return oStrStream;
    }
} // namespace NodeLib
