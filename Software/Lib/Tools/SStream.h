/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Lightweight, fixed-buffer, no-heap std::stringstream workalike -- real
// <sstream> is available via arm-none-eabi's libstdc++ but is too heavy for an
// 8KB-RAM part. Matches just enough of the real interface (chainable
// operator<<, str()) for NodeLib's logging headers (EChannelId.h, EOperation.h,
// EPinMode.h, id.h), which each define their own
// operator<<(std::stringstream&, ...) for their enum types.
namespace std
{
    struct stringstream
    {
        stringstream() :
            length(0)
        {
            buffer[0] = '\0';
        }

        const char* str() const
        {
            return buffer;
        }

        stringstream& operator<<(const char* const arg)
        {
            return Append(arg);
        }

        stringstream& operator<<(const char arg)
        {
            const char text[2] = {arg, '\0'};
            return Append(text);
        }

        stringstream& operator<<(const uint8_t arg)
        {
            return AppendFormatted("%u", static_cast<unsigned int>(arg));
        }

        stringstream& operator<<(const int arg)
        {
            return AppendFormatted("%d", arg);
        }

        stringstream& operator<<(const unsigned int arg)
        {
            return AppendFormatted("%u", arg);
        }

        stringstream& operator<<(const long arg)
        {
            return AppendFormatted("%ld", arg);
        }

        stringstream& operator<<(const unsigned long arg)
        {
            return AppendFormatted("%lu", arg);
        }

        stringstream& operator<<(const bool arg)
        {
            return Append(arg ? "1" : "0");
        }

      private:
        static const size_t capacity = 128;

        stringstream& Append(const char* const text)
        {
            const size_t textLen = strlen(text);
            const size_t space   = capacity - 1 - length;
            const size_t toCopy  = textLen < space ? textLen : space;
            memcpy(buffer + length, text, toCopy);
            length += toCopy;
            buffer[length] = '\0';
            return *this;
        }

        template <typename T>
        stringstream& AppendFormatted(const char* const format, const T value)
        {
            char temp[24];
            snprintf(temp, sizeof(temp), format, value);
            return Append(temp);
        }

        char   buffer[capacity];
        size_t length;
    };
} // namespace std
