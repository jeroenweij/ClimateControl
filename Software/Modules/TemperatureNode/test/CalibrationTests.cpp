/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "FakeFlash.h"
#include "Test.h"

#include "Calibration.h"

using Probe = Calibration::Probe;

CC_TEST(Calibration, ABlankPageReadsAsZeroOffsets)
{
    FakeFlash::Reset();
    Calibration calibration;
    calibration.Load();

    CC_CHECK_EQ(calibration.Offset(Probe::Return), 0);
    CC_CHECK_EQ(calibration.Offset(Probe::Supply), 0);
}

CC_TEST(Calibration, AnOffsetSurvivesAReload)
{
    FakeFlash::Reset();
    {
        Calibration calibration;
        calibration.Load();
        CC_CHECK(calibration.SetOffset(Probe::Return, -130));
        CC_CHECK(calibration.SetOffset(Probe::Supply, -90));
    }

    Calibration reloaded; // as after a reset
    reloaded.Load();
    CC_CHECK_EQ(reloaded.Offset(Probe::Return), -130);
    CC_CHECK_EQ(reloaded.Offset(Probe::Supply), -90);
}

CC_TEST(Calibration, AnOutOfRangeOffsetIsRejectedAndNothingChanges)
{
    FakeFlash::Reset();
    Calibration calibration;
    calibration.Load();
    CC_CHECK(calibration.SetOffset(Probe::Supply, 50));

    CC_CHECK(!calibration.SetOffset(Probe::Supply, Calibration::MaxOffset + 1));
    CC_CHECK(!calibration.SetOffset(Probe::Supply, -Calibration::MaxOffset - 1));
    CC_CHECK_EQ(calibration.Offset(Probe::Supply), 50);

    CC_CHECK(calibration.SetOffset(Probe::Supply, Calibration::MaxOffset)); // the limit itself is fine
}

CC_TEST(Calibration, AnUnchangedOffsetCostsNoFlashCycle)
{
    FakeFlash::Reset();
    Calibration calibration;
    calibration.Load();
    CC_CHECK(calibration.SetOffset(Probe::Return, -130));
    CC_CHECK_EQ(FakeFlash::EraseCount(), 1);

    CC_CHECK(calibration.SetOffset(Probe::Return, -130)); // re-sent, same value
    CC_CHECK_EQ(FakeFlash::EraseCount(), 1);
}

CC_TEST(Calibration, ACorruptRecordReadsAsZeroOffsets)
{
    FakeFlash::Reset();
    {
        Calibration calibration;
        calibration.Load();
        CC_CHECK(calibration.SetOffset(Probe::Return, -130));
    }
    FakeFlash::Settings()[4] ^= 0x01; // flip a bit in the stored offset

    Calibration reloaded;
    reloaded.Load();
    CC_CHECK_EQ(reloaded.Offset(Probe::Return), 0);
}

CC_TEST(Calibration, AFailedWriteKeepsTheOldOffset)
{
    FakeFlash::Reset();
    Calibration calibration;
    calibration.Load();
    CC_CHECK(calibration.SetOffset(Probe::Supply, 40));

    FakeFlash::SetNextProgramLimit(0); // the program step fails
    CC_CHECK(!calibration.SetOffset(Probe::Supply, -90));
    CC_CHECK_EQ(calibration.Offset(Probe::Supply), 40);
}
