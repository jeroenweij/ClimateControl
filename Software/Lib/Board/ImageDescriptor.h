/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include <stdint.h>

// The application image descriptor, at a fixed 0xC0 offset from the app's flash
// base (right after the vector table). The bootloader reads it to decide whether
// the image is runnable. See Spec/Node-Flash-Layout-and-Bootloader-Spec.md §4.
//
// magic / module / version / buildId are baked in from source at link time.
// imageSize and the trailing CRC-32 (last 4 bytes of the .bin) are filled in by
// the post-build finalize step, which also sets FlagCrcPresent -- an image
// flashed raw over SWD during development has FlagCrcPresent clear and is
// accepted on the magic alone.

namespace Board
{
    constexpr uint32_t ImageMagic = 0x43436D67; // 'CCmg' -- ClimateControl image

    enum ImageFlags : uint16_t
    {
        FlagCrcPresent = 1u << 0, // imageSize valid + trailing CRC-32 appended
    };

    enum class ImageModule : uint16_t
    {
        Unknown         = 0,
        ControllerNode  = 1,
        TemperatureNode = 2,
        MainController  = 3,
        Thermostat      = 4,
    };

    struct __attribute__((packed)) ImageDescriptor // 32 bytes
    {
        uint32_t    magic;
        uint16_t    headerVersion;
        ImageModule module;
        uint32_t    imageSize; // bytes from app base to end of image, incl. trailing CRC-32
        uint16_t    fwVersionMajor;
        uint16_t    fwVersionMinor;
        uint16_t    flags;
        uint16_t    reserved0;
        uint32_t    buildId; // git short hash
        uint32_t    reserved1[2];
    };
    static_assert(sizeof(ImageDescriptor) == 32, "ImageDescriptor must be 32 bytes");

// Place one of these in an application's sources, e.g.:
//   CC_IMAGE_DESCRIPTOR(Board::ImageModule::MainController, 0, 1)
#define CC_IMAGE_DESCRIPTOR(MODULE, MAJOR, MINOR)                                                 \
    extern "C" __attribute__((section(".image_descriptor"), used)) const ::Board::ImageDescriptor \
        gImageDescriptor = {::Board::ImageMagic, 1, (MODULE), 0, (MAJOR), (MINOR)}
} // namespace Board
