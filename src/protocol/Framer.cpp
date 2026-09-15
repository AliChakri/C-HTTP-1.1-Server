#include "protocol/Framer.h"

#include <algorithm>

FrameStatus Framer::extractLine(std::vector<uint8_t>& readBuffer, std::string& outLine)
{
    if (readBuffer.empty())
    {
        return FrameStatus::Incomplete;
    }

    // Look for "\n\r"
    for (size_t i = 0; i < readBuffer.size(); i++)
    {
        if (i > MAX_CHUNK_BODY_SIZE)
        {
            return FrameStatus::HeaderTooLarge;
        }

        if (i + 1 < readBuffer.size() && readBuffer[i] == '\r' && readBuffer[i + 1] == '\n')
        {
            // Found CRLF line
            outLine.assign(readBuffer.begin(), readBuffer.begin() + i);

            // Erase line conent + 2 delimiter
            readBuffer.erase(readBuffer.begin(), readBuffer.begin() + i + 2);

            return FrameStatus::Success;
        }
    }

    // if buffered data exeeds max allowed header size without finding \r\n
    if (readBuffer.size() > MAX_HTTP_HEADER_SIZE)
    {
      return FrameStatus::HeaderTooLarge;
    }

    return FrameStatus::Incomplete;
}

FrameStatus Framer::extractBytes(std::vector<uint8_t>& readBuffer, size_t requiredBytes, std::vector<uint8_t>& outBytes)
{
    if (requiredBytes > MAX_HTTP_BODY_SIZE)
    {
        return FrameStatus::PayloadTooLarge;
    }

    if (readBuffer.size() < requiredBytes)
    {
        return FrameStatus::Incomplete;
    }

    // Slice required body bytes
    outBytes.assign(readBuffer.begin(), readBuffer.begin() + requiredBytes);

    // Erase extracted payload from readBuffer
    readBuffer.erase(readBuffer.begin(), readBuffer.begin() + requiredBytes);

    return FrameStatus::Success;
}
