/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include <stdint.h>

#include "Crc.h"
#include "ImageDescriptor.h"
#include "MemoryMap.h"

#include "AppImage.h"

using Board::ImageDescriptor;

namespace
{
    const ImageDescriptor& Descriptor()
    {
        return *reinterpret_cast<const ImageDescriptor*>(Board::Flash::AppBase + Board::Flash::AppDescriptorOffset);
    }
} // namespace

bool Boot::AppImage::IsValid()
{
    const ImageDescriptor& descriptor = Descriptor();

    if (descriptor.magic != Board::ImageMagic)
    {
        return false;
    }

    if ((descriptor.flags & Board::FlagCrcPresent) == 0)
    {
        // Dev image, flashed raw over SWD. Trust the magic.
        return true;
    }

    if (descriptor.imageSize < 8 || descriptor.imageSize > Board::Flash::AppSize)
    {
        return false;
    }

    Hal::Crc       crc(Hal::Crc::Poly::Ieee32);
    const uint32_t computed =
        crc.Compute32(reinterpret_cast<const uint8_t*>(Board::Flash::AppBase), descriptor.imageSize - 4U);
    const uint32_t stored =
        *reinterpret_cast<const uint32_t*>(Board::Flash::AppBase + descriptor.imageSize - 4U);

    return computed == stored;
}
