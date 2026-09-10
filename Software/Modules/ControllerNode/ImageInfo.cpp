/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "ImageDescriptor.h"

// Fixed 0xC0-offset descriptor the bootloader reads (Node-Flash-Layout-and-
// Bootloader-Spec.md §4). imageSize + trailing CRC-32 are filled by the post-build
// finalize step; the version comes from CC_FW_VERSION in the top-level CMake.
CC_IMAGE_DESCRIPTOR(Board::ImageModule::ControllerNode, CC_FW_VERSION_MAJOR, CC_FW_VERSION_MINOR);
