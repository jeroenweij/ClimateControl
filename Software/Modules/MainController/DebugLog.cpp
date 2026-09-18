/*************************************************************
 * Created by J. Weij
 *
 * MainController's Tools::Logger backend -- bit-banged on Board::LogTx
 * (BoardPins.h), the only free pin once both on-chip USARTs are committed
 * to the bus and NINA (MainController-Spec.md §5). This only ever sees the
 * sparse INFO/WARN/ERROR call sites: LOG_DEBUG's per-frame bus tracing is
 * compiled out entirely in Release builds (Lib/Tools/Logger.h), so this
 * never touches bus timing. Overrides the weak default in Lib/Tools/
 * Logger.cpp -- see that file's header comment.
 *************************************************************/

#include "BitBangSerial.h"
#include "BoardPins.h"

#include "Logger.h"

void Tools::Logger::Write(const char* const level, const char* const msg)
{
    static Hal::BitBangSerial serial(Board::LogTx);

    serial.WriteString(level);
    serial.WriteString(": ");
    serial.WriteString(msg);
    serial.WriteString("\r\n");
}
