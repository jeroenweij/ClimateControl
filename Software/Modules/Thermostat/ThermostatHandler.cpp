/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "BoardPins.h"
#include "Logger.h"
#include "Tick.h"

#include "EEndpoint.h"
#include "EOperation.h"
#include "RoomDemand.h"
#include "SettingsPage.h"

#include "ThermostatHandler.h"

using NodeLib::Endpoint;
using NodeLib::Id;
using NodeLib::Message;
using NodeLib::Operation;

namespace
{
    constexpr uint8_t NackReadOnly    = 0x01;
    constexpr uint8_t NackBadRequest  = 0x02;
    constexpr uint8_t NackWriteFailed = 0x04;
    constexpr uint8_t NackUnavailable = 0x05; // no valid sensor reading

    // This node's runtime settings record (NodeLib::SettingsPage): payload
    // bytes 0-1 = RoomTempOffset, int16 LE centi-degC.
    constexpr uint32_t SettingsMagic = 0x54534854; // 'THST'

    // Failed samples in a row (one per sampleIntervalMs) before the room
    // reading is dropped: rides out a glitch, but a dead sensor stops being
    // reported within seconds.
    constexpr uint8_t sensorFailLimit = 3;

    constexpr uint32_t sampleIntervalMs = 2000;

    // Setpoint adjust (§4.3's touch buttons): each press steps 0.5 degC,
    // clamped to a fixed comfort range -- no adjust-mode/timeout state, one
    // press is one step.
    constexpr int16_t SetpointStepCentiDegC = 50;
    constexpr int16_t SetpointMinCentiDegC  = 1900;
    constexpr int16_t SetpointMaxCentiDegC  = 2300;

    // How long the panel stays lit after the last button press (§4.2:
    // "woken by a button press... after an inactivity timeout it... turns off").
    constexpr uint32_t DisplayAwakeMs = 60000;

    // Minimum time between display redraws (ThermostatHandler.h, shownView).
    constexpr uint32_t minRedrawMs = 200;

    // Damper bar: the bottom DamperBarHeight rows, 100 % = the full panel width.
    constexpr int DamperBarPx     = Ssd1306::Width;
    constexpr int DamperBarHeight = 2;

    // Digit boxes: {w, h, segment thickness}.
    constexpr int BigW = 18, BigH = 30, BigT = 4; // room temperature
    constexpr int BigY   = 6;
    constexpr int SmallW = 8, SmallH = 14, SmallT = 2; // setpoint, humidity
    constexpr int SmallY     = 42;
    constexpr int SmallInset = 8; // setpoint / humidity distance from the panel edges

    // Icons, '#' = lit (Ssd1306::DrawGlyph()).
    const char* const heatGlyph[] = {
        "..##...##...##..",
        ".##...##...##...",
        "##...##...##....",
        "##...##...##....",
        "##...##...##....",
        ".##...##...##...",
        "..##...##...##..",
        "...##...##...##.",
        "....##...##...##",
        "....##...##...##",
        "....##...##...##",
        "...##...##...##.",
        "..##...##...##..",
    };
    const char* const snowflakeGlyph[] = {
        "......#......",
        ".#..#.#.#..#.",
        "..#..###..#..",
        "...#..#..#...",
        ".#..#.#.#..#.",
        "..#..###..#..",
        "#############",
        "..#..###..#..",
        ".#..#.#.#..#.",
        "...#..#..#...",
        "..#..###..#..",
        ".#..#.#.#..#.",
        "......#......",
    };
    const char* const noLinkGlyph[] = {
        "#........#",
        "##......##",
        ".##....##.",
        "..##..##..",
        "...####...",
        "...####...",
        "..##..##..",
        ".##....##.",
        "##......##",
        "#........#",
    };
    const char* const percentGlyph[] = {
        ".##....##.",
        "#..#...##.",
        "#..#..##..",
        "#..#..##..",
        ".##..##...",
        ".....##...",
        "....##....",
        "....##....",
        "...##.....",
        "...##..##.",
        "..##..#..#",
        "..##..#..#",
        ".##...#..#",
        ".##....##.",
    };

    template <int N>
    constexpr int Rows(const char* const (&)[N])
    {
        return N;
    }

    void PackU16(uint8_t* const p, const uint16_t v)
    {
        p[0] = static_cast<uint8_t>(v);
        p[1] = static_cast<uint8_t>(v >> 8);
    }

    uint16_t ReadU16(const uint8_t* const p)
    {
        return static_cast<uint16_t>(p[0] | (p[1] << 8));
    }
} // namespace

ThermostatHandler::ThermostatHandler(NodeLib::Node& node) :
    node(node),
    i2c({Board::I2cSda, Board::I2cScl, Board::I2cAf}),
    sensor(i2c),
    display(i2c),
    oledPower(Board::OledPowerEnable, Hal::Gpio::Mode::Output),
    oledReset(Board::OledReset, Hal::Gpio::Mode::Output),
    buttonUp(Board::UserButton, Hal::Gpio::Mode::InputPullUp),
    buttonDown(Board::UserButton2, Hal::Gpio::Mode::InputPullUp),
    setpoint(2100), // 21.00 degC
    roomTemp(0),
    humidity(0),
    roomMode(2), // Auto
    tempOffset(DefaultRoomTempOffsetCentiDegC),
    roomValid(false),
    sensorFailures(0),
    damperActual(0),
    damperMode(0),
    airMode(static_cast<uint8_t>(NodeLib::AirMode::Unknown)),
    downWasPressed(false),
    upWasPressed(false),
    linkUp(false),
    displayOn(false),
    shownView{},
    redrawNeeded(false),
    redrawTimer(),
    sampleTimer(),
    displayTimer()
{
    // Reported to the ControllerNode on change + keepalive, and again after
    // the link was lost (Node's publisher, Node-Message-Model-Spec.md §6.1).
    node.AddPublished(Endpoint::RoomSetpoint, 2);
    node.AddPublished(Endpoint::RoomTemp, 2, NodeLib::TempMinChange);
    node.AddPublished(Endpoint::RoomHumidity, 2, NodeLib::HumidityMinChange);
    node.AddPublished(Endpoint::RoomMode, 1);
    node.AddPublished(Endpoint::RoomTempOffset, 2);
}

void ThermostatHandler::Init()
{
    uint8_t settings[2];
    if (NodeLib::SettingsPage::Load(SettingsMagic, settings, sizeof(settings)))
    {
        tempOffset = static_cast<int16_t>(ReadU16(settings));
    }
    LOG_INFO("Room temperature offset " << tempOffset << " centi-degC");

    // OLED VBAT is behind an MCU-gated load switch, Hi-Z (off) by default at
    // reset (ControllerNode-Thermostat-Link-Spec.md §4.1) -- power it, then
    // pulse RES# per the datasheet's reset circuit (hold low >= 3us).
    oledPower.Write(true);
    Hal::Tick::DelayMs(5); // let VBAT settle before reset/I2C bring-up
    oledReset.Write(false);
    Hal::Tick::DelayUs(10);
    oledReset.Write(true);

    i2c.Init();
    display.Init();
    display.Off(); // starts asleep; a button press wakes it (§4.2)

    sampleTimer.Start(sampleIntervalMs);
    SampleRoom(); // a real first reading now, rather than waiting sampleIntervalMs
}

void ThermostatHandler::Loop()
{
    ServiceButtons();

    if (sampleTimer.Finished())
    {
        SampleRoom();
        sampleTimer.Start(sampleIntervalMs);
    }

    PublishRoom();

    RenderDisplay();
}

void ThermostatHandler::SampleRoom()
{
    int16_t  centiDegC;
    uint16_t centiHumidity;
    if (sensor.Measure(centiDegC, centiHumidity))
    {
        roomTemp       = static_cast<int16_t>(centiDegC + tempOffset);
        humidity       = centiHumidity;
        roomValid      = true;
        sensorFailures = 0;
        return;
    }

    // Keep the last good reading through a glitch, retried on the next
    // sampleTimer tick -- but not for ever.
    if (sensorFailures < sensorFailLimit)
    {
        sensorFailures++;
    }
    if (roomValid && sensorFailures >= sensorFailLimit)
    {
        LOG_WARN("Room sensor not answering -- reading dropped");
        roomValid = false;
    }
}

void ThermostatHandler::ServiceButtons()
{
    const bool downPressed = !buttonDown.Read(); // active-low
    const bool upPressed   = !buttonUp.Read();
    const bool downEdge    = downPressed && !downWasPressed;
    const bool upEdge      = upPressed && !upWasPressed;
    downWasPressed         = downPressed;
    upWasPressed           = upPressed;

    // With the panel off, a press only wakes it -- the user can't see the
    // setpoint yet, so it must not change it.
    if (!displayOn)
    {
        if (downEdge || upEdge)
        {
            WakeDisplay();
        }
        return;
    }

    if (downEdge)
    {
        setpoint = static_cast<int16_t>(setpoint - SetpointStepCentiDegC);
        if (setpoint < SetpointMinCentiDegC)
        {
            setpoint = SetpointMinCentiDegC;
        }
        WakeDisplay();
    }
    if (upEdge)
    {
        setpoint = static_cast<int16_t>(setpoint + SetpointStepCentiDegC);
        if (setpoint > SetpointMaxCentiDegC)
        {
            setpoint = SetpointMaxCentiDegC;
        }
        WakeDisplay();
    }
}

void ThermostatHandler::WakeDisplay()
{
    if (!displayOn)
    {
        display.On();
        displayOn    = true;
        redrawNeeded = true; // the panel may show a stale frame -- draw now
    }
    displayTimer.Start(DisplayAwakeMs);
}

bool ThermostatHandler::SView::operator==(const SView& other) const
{
    return tempTenths == other.tempTenths && setpointTenths == other.setpointTenths &&
        humidityPercent == other.humidityPercent && damperBar == other.damperBar && airMode == other.airMode && linkUp == other.linkUp &&
        roomValid == other.roomValid;
}

ThermostatHandler::SView ThermostatHandler::CurrentView() const
{
    SView view;
    view.tempTenths      = static_cast<int16_t>(roomTemp / 10);
    view.setpointTenths  = static_cast<int16_t>(setpoint / 10);
    view.humidityPercent = static_cast<int16_t>(humidity / 100);
    const int damper     = damperActual > 100 ? 100 : damperActual; // a bad Set can't overrun the panel
    view.damperBar       = static_cast<uint8_t>((DamperBarPx * damper) / 100);
    view.airMode         = airMode;
    view.linkUp          = linkUp;
    view.roomValid       = roomValid;
    return view;
}

void ThermostatHandler::RenderDisplay()
{
    if (displayTimer.Finished())
    {
        display.Off();
        displayOn = false;
    }
    if (!displayOn)
    {
        return;
    }

    const SView view = CurrentView();
    if (!redrawNeeded && view == shownView)
    {
        return; // nothing visible changed
    }
    redrawNeeded = true;
    if (redrawTimer.IsRunning() && !redrawTimer.Finished())
    {
        return; // redrawn too recently -- pick it up once minRedrawMs is over
    }

    display.Clear();

    // Room temperature, big, centred; dashes while there is no sensor reading.
    if (view.roomValid)
    {
        const int w = Ssd1306::NumberWidth(BigW, BigT, view.tempTenths, 1);
        display.DrawNumber((Ssd1306::Width - w) / 2, BigY, BigW, BigH, BigT, view.tempTenths, 1);
    }
    else
    {
        const int dashW = 14, dashGap = 6, w = 3 * dashW + 2 * dashGap;
        for (int i = 0; i < 3; i++)
        {
            display.FillRect((Ssd1306::Width - w) / 2 + i * (dashW + dashGap), BigY + BigH / 2 - 2, dashW, BigT, true);
        }
    }

    // Top-right: no link -> X; else what the duct air does for the room.
    const char* const* icon     = nullptr;
    int                iconRows = 0;
    if (!view.linkUp)
    {
        icon     = noLinkGlyph;
        iconRows = Rows(noLinkGlyph);
    }
    else if (view.airMode == static_cast<uint8_t>(NodeLib::AirMode::Heating))
    {
        icon     = heatGlyph;
        iconRows = Rows(heatGlyph);
    }
    else if (view.airMode == static_cast<uint8_t>(NodeLib::AirMode::Cooling))
    {
        icon     = snowflakeGlyph;
        iconRows = Rows(snowflakeGlyph);
    }
    if (icon != nullptr)
    {
        display.DrawGlyph(Ssd1306::Width - Ssd1306::GlyphWidth(icon) - 4, 4, icon, iconRows);
    }

    // Setpoint, bottom-left.
    display.DrawNumber(SmallInset, SmallY, SmallW, SmallH, SmallT, view.setpointTenths, 1);

    // Humidity, bottom-right, with a % sign.
    const int percentW = Ssd1306::GlyphWidth(percentGlyph);
    const int percentX = Ssd1306::Width - SmallInset - percentW;
    display.DrawGlyph(percentX, SmallY, percentGlyph, Rows(percentGlyph));
    if (view.roomValid)
    {
        const int w = Ssd1306::NumberWidth(SmallW, SmallT, view.humidityPercent, 0);
        display.DrawNumber(percentX - 3 - w, SmallY, SmallW, SmallH, SmallT, view.humidityPercent, 0);
    }
    else
    {
        display.FillRect(percentX - 3 - 2 * SmallW - 3, SmallY + SmallH / 2 - 1, SmallW, SmallT, true);
        display.FillRect(percentX - 3 - SmallW, SmallY + SmallH / 2 - 1, SmallW, SmallT, true);
    }

    // Damper position, bottom rows, edge to edge.
    display.FillRect(0, Ssd1306::Height - DamperBarHeight, view.damperBar, DamperBarHeight, true);

    display.Flush();

    shownView    = view;
    redrawNeeded = false;
    redrawTimer.Start(minRedrawMs);
}

void ThermostatHandler::PublishRoom()
{
    node.PublishIfChanged(Endpoint::RoomSetpoint, setpoint);
    node.PublishIfChanged(Endpoint::RoomMode, roomMode);
    node.PublishIfChanged(Endpoint::RoomTempOffset, tempOffset);
    if (roomValid)
    {
        node.PublishIfChanged(Endpoint::RoomTemp, roomTemp);
        node.PublishIfChanged(Endpoint::RoomHumidity, humidity);
    }
    else
    {
        // Silence, not a stale value: the ControllerNode drops a room
        // reading that stops being refreshed.
        node.ClearPublished(Endpoint::RoomTemp);
        node.ClearPublished(Endpoint::RoomHumidity);
    }
}

void ThermostatHandler::ReportRoom(const Endpoint endpoint)
{
    uint8_t p[2];
    switch (endpoint)
    {
        case Endpoint::RoomTemp:
        case Endpoint::RoomHumidity:
            if (!roomValid)
            {
                node.QueueMessage(Id(node.GetId(), endpoint, Operation::Nack), NackUnavailable);
                return;
            }
            break;
        default:
            break;
    }

    switch (endpoint)
    {
        case Endpoint::RoomTemp:
            PackU16(p, static_cast<uint16_t>(roomTemp));
            node.QueueMessage(Id(node.GetId(), endpoint, Operation::Report), p, 2);
            break;
        case Endpoint::RoomHumidity:
            PackU16(p, humidity);
            node.QueueMessage(Id(node.GetId(), endpoint, Operation::Report), p, 2);
            break;
        case Endpoint::RoomMode:
            node.QueueMessage(Id(node.GetId(), endpoint, Operation::Report), roomMode);
            break;
        default:
            break;
    }
}

void ThermostatHandler::ReceivedMessage(const Message& m)
{
    switch (m.id.endpoint)
    {
        case Endpoint::RoomSetpoint:
            if (m.id.operation == Operation::Get)
            {
                uint8_t p[2];
                PackU16(p, static_cast<uint16_t>(setpoint));
                node.QueueMessage(Id(node.GetId(), Endpoint::RoomSetpoint, Operation::Report), p, 2);
            }
            else if (m.id.operation == Operation::Set && m.len >= 2)
            {
                setpoint = static_cast<int16_t>(ReadU16(m.data)); // master override
                LOG_INFO("Setpoint override " << setpoint);
            }
            else
            {
                node.QueueMessage(Id(node.GetId(), m.id.endpoint, Operation::Nack), NackBadRequest);
            }
            break;

        case Endpoint::RoomTempOffset:
            if (m.id.operation == Operation::Get)
            {
                uint8_t p[2];
                PackU16(p, static_cast<uint16_t>(tempOffset));
                node.QueueMessage(Id(node.GetId(), Endpoint::RoomTempOffset, Operation::Report), p, 2);
            }
            else if (m.id.operation == Operation::Set && m.len >= 2)
            {
                SetTempOffset(m);
            }
            else
            {
                node.QueueMessage(Id(node.GetId(), m.id.endpoint, Operation::Nack), NackBadRequest);
            }
            break;

        case Endpoint::RoomTemp:
        case Endpoint::RoomHumidity:
        case Endpoint::RoomMode:
            if (m.id.operation == Operation::Get)
            {
                ReportRoom(m.id.endpoint);
            }
            else
            {
                node.QueueMessage(Id(node.GetId(), m.id.endpoint, Operation::Nack), NackReadOnly);
            }
            break;

        case Endpoint::DamperActual:
            if (m.id.operation == Operation::Set && m.len >= 1)
            {
                damperActual = m.data[0];
            }
            break;
        case Endpoint::DamperMode:
            if (m.id.operation == Operation::Set && m.len >= 1)
            {
                damperMode = m.data[0];
            }
            break;
        case Endpoint::AirMode:
            if (m.id.operation == Operation::Set && m.len >= 1)
            {
                airMode = m.data[0];
            }
            break;

        default:
            break;
    }
}

void ThermostatHandler::SetTempOffset(const Message& m)
{
    const int16_t offset = static_cast<int16_t>(ReadU16(m.data));
    if (offset > NodeLib::RoomTempOffsetLimit || offset < -NodeLib::RoomTempOffsetLimit)
    {
        node.QueueMessage(Id(node.GetId(), Endpoint::RoomTempOffset, Operation::Nack), NackBadRequest);
        return;
    }
    if (offset != tempOffset) // a re-sent identical Set costs no flash cycle
    {
        uint8_t settings[2];
        PackU16(settings, static_cast<uint16_t>(offset));
        if (!NodeLib::SettingsPage::Save(SettingsMagic, settings, sizeof(settings)))
        {
            LOG_ERROR("RoomTempOffset write failed");
            node.QueueMessage(Id(node.GetId(), Endpoint::RoomTempOffset, Operation::Nack), NackWriteFailed);
            return;
        }
        // Shift the current reading too, rather than waiting for the next sample.
        roomTemp   = static_cast<int16_t>(roomTemp - tempOffset + offset);
        tempOffset = offset;
        LOG_INFO("Room temperature offset " << tempOffset << " centi-degC");
    }
    uint8_t p[2];
    PackU16(p, static_cast<uint16_t>(tempOffset));
    node.QueueMessage(Id(node.GetId(), Endpoint::RoomTempOffset, Operation::Report), p, 2);
}

void ThermostatHandler::ConnectionLost()
{
    // Hold the setpoint locally (we are its source of truth) and flag the UI.
    LOG_WARN("ControllerNode link lost");
    linkUp = false;
}

void ThermostatHandler::Snoop(const Message&)
{
    // This link is a fixed point-to-point pair -- any frame at all on this
    // node's UART can only be from the ControllerNode, so it's sufficient
    // liveness evidence on its own (unlike the shared main bus).
    linkUp = true;
}

void ThermostatHandler::PrepareForReset()
{
    display.Off();
}
