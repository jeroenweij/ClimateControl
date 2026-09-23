/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "Logger.h"
#include "LogRing.h"

__attribute__((weak)) void Tools::Logger::Write(const char* const, const char* const)
{
}

void Tools::Logger::Emit(const char* const level, const char* const msg)
{
    Tools::LogRing::Push(level, msg);
    Write(level, msg);
}
