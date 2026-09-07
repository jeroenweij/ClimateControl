/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include "SStream.h"

namespace Tools
{
    namespace Logger
    {
        // Default implementation (Logger.cpp) is a no-op weak symbol -- override
        // elsewhere (e.g. to write to a debug UART) to keep Tools decoupled from
        // any particular Hal::Uart instance.
        void Write(const char* const level, const char* const msg);
    } // namespace Logger
} // namespace Tools

#define LOG_ERROR(message)                              \
    {                                                   \
        std::stringstream logStream;                    \
        logStream << message;                           \
        Tools::Logger::Write("ERROR", logStream.str()); \
    }

#define LOG_WARN(message)                                 \
    {                                                     \
        std::stringstream logStream;                      \
        logStream << message;                             \
        Tools::Logger::Write("WARNING", logStream.str()); \
    }

#define LOG_INFO(message)                              \
    {                                                  \
        std::stringstream logStream;                   \
        logStream << message;                          \
        Tools::Logger::Write("INFO", logStream.str()); \
    }

#ifdef DEBUG
#define LOG_DEBUG(message)                              \
    {                                                   \
        std::stringstream logStream;                    \
        logStream << message;                           \
        Tools::Logger::Write("DEBUG", logStream.str()); \
    }
#else
#define LOG_DEBUG(message) \
    {                      \
    }
#endif
