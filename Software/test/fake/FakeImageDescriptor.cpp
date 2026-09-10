/*************************************************************
 * Created by J. Weij
 *
 * Stand-in for the per-module image descriptor the linker script places at
 * the app base (Lib/Board/ImageDescriptor.h). NodeLib::Node reads its
 * firmware-version fields when answering Endpoint::SystemInfo.
 *************************************************************/

#include "ImageDescriptor.h"

extern "C" const Board::ImageDescriptor gImageDescriptor = {
    Board::ImageMagic,
    1,
    Board::ImageModule::TemperatureNode,
    0,
    0,
    1,
    0,
    0,
    0,
    {0, 0},
};
