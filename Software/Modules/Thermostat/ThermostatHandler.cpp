/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "BoardPins.h"
#include "Logger.h"
#include "Tick.h"

#include "EEndpoint.h"
#include "EOperation.h"

#include "ThermostatHandler.h"

using NodeLib::Endpoint;
using NodeLib::Id;
using NodeLib::Message;
using NodeLib::Operation;

namespace
{
    constexpr uint8_t NackReadOnly   = 0x01;
    constexpr uint8_t NackBadRequest = 0x02;

    // Deadbands for on-change publishing (Node-Message-Model-Spec.md §6.1).
    constexpr int16_t  TempDeadband     = 10; // 0.1 degC
    constexpr uint16_t HumidityDeadband = 100; // 1 %RH

    constexpr uint32_t sampleIntervalMs    = 2000;
    constexpr uint32_t keepaliveIntervalMs = 60000;

    // Setpoint adjust (§4.3's touch buttons): each press steps 0.5 degC,
    // clamped to a fixed comfort range -- no adjust-mode/timeout state, one
    // press is one step.
    constexpr int16_t SetpointStepCentiDegC = 50;
    constexpr int16_t SetpointMinCentiDegC  = 1900;
    constexpr int16_t SetpointMaxCentiDegC  = 2300;

    // How long the panel stays lit after the last button press (§4.2:
    // "woken by a button press... after an inactivity timeout it... turns off").
    constexpr uint32_t DisplayAwakeMs = 10000;

    void PackU16(uint8_t* const p, const uint16_t v)
    {
        p[0] = static_cast<uint8_t>(v);
        p[1] = static_cast<uint8_t>(v >> 8);
    }

    uint16_t ReadU16(const uint8_t* const p)
    {
        return static_cast<uint16_t>(p[0] | (p[1] << 8));
    }

    int16_t Abs16(const int16_t v)
    {
        return v < 0 ? static_cast<int16_t>(-v) : v;
    }
} // namespace

ThermostatHandler::ThermostatHandler(NodeLib::Node& node) :
    node(node),
    i2c({Board::I2cSda, Board::I2cScl, Board::I2cAf}),
    sensor(i2c),
    display(i2c),
    oledPower(Board::OledPowerEnable, Hal::Gpio::Mode::Output),
    oledReset(Board::OledReset, Hal::Gpio::Mode::Output),
    buttonDown(Board::UserButton, Hal::Gpio::Mode::InputPullUp),
    buttonUp(Board::UserButton2, Hal::Gpio::Mode::InputPullUp),
    setpoint(2100), // 21.00 degC
    roomTemp(2100),
    humidity(4500),
    roomMode(2), // Auto
    reportedSetpoint(0),
    reportedTemp(0),
    reportedHumidity(0),
    reportedMode(0),
    everReported(false),
    damperActual(0),
    damperMode(0),
    downWasPressed(false),
    upWasPressed(false),
    linkUp(false),
    displayOn(false),
    sampleTimer(),
    keepaliveTimer(),
    displayTimer()
{
}

void ThermostatHandler::Init()
{
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
    keepaliveTimer.Start(keepaliveIntervalMs);
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

    const bool keepalive = keepaliveTimer.Finished();
    if (keepalive)
    {
        keepaliveTimer.Start(keepaliveIntervalMs);
    }
    PublishRoom(keepalive);

    RenderDisplay();
}

void ThermostatHandler::SampleRoom()
{
    int16_t  centiDegC;
    uint16_t centiHumidity;
    if (sensor.Measure(centiDegC, centiHumidity))
    {
        roomTemp = centiDegC;
        humidity = centiHumidity;
    }
    // else: keep the last good reading -- retried on the next sampleTimer tick.
}

void ThermostatHandler::ServiceButtons()
{
    const bool downPressed = !buttonDown.Read(); // active-low
    const bool upPressed   = !buttonUp.Read();

    if (downPressed && !downWasPressed)
    {
        setpoint = static_cast<int16_t>(setpoint - SetpointStepCentiDegC);
        if (setpoint < SetpointMinCentiDegC)
        {
            setpoint = SetpointMinCentiDegC;
        }
        WakeDisplay();
    }
    if (upPressed && !upWasPressed)
    {
        setpoint = static_cast<int16_t>(setpoint + SetpointStepCentiDegC);
        if (setpoint > SetpointMaxCentiDegC)
        {
            setpoint = SetpointMaxCentiDegC;
        }
        WakeDisplay();
    }

    downWasPressed = downPressed;
    upWasPressed   = upPressed;
}

void ThermostatHandler::WakeDisplay()
{
    if (!displayOn)
    {
        display.On();
        displayOn = true;
    }
    displayTimer.Start(DisplayAwakeMs);
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

    display.Clear();

    // Room temperature, big, top-left; degree mark; link status top-right.
    display.DrawNumber(2, 2, 14, 24, 3, roomTemp / 10, 1);
    display.FillCircle(72, 6, 2, true);
    if (linkUp)
    {
        display.FillCircle(120, 6, 3, true);
    }
    else
    {
        display.DrawCircle(120, 6, 3, true);
    }

    // Setpoint, smaller, bottom-left, with a small square "target" marker.
    display.DrawRect(2, 42, 5, 5, true);
    display.DrawNumber(10, 40, 7, 12, 1, setpoint / 10, 1);

    // Humidity, bottom-middle.
    display.DrawNumber(66, 40, 7, 12, 1, static_cast<int>(humidity / 100), 0);
    display.FillCircle(90, 44, 1, true);
    display.FillCircle(93, 47, 1, true);

    // Damper position, bottom row.
    display.DrawRect(2, 58, 60, 5, true);
    display.FillRect(4, 60, (56 * damperActual) / 100, 1, true);

    display.Flush();
}

void ThermostatHandler::PublishRoom(const bool force)
{
    if (force || !everReported || setpoint != reportedSetpoint)
    {
        uint8_t p[2];
        PackU16(p, static_cast<uint16_t>(setpoint));
        node.QueueMessage(Id(node.GetId(), Endpoint::RoomSetpoint, Operation::Report), p, 2);
        reportedSetpoint = setpoint;
    }
    if (force || !everReported || Abs16(roomTemp - reportedTemp) >= TempDeadband)
    {
        uint8_t p[2];
        PackU16(p, static_cast<uint16_t>(roomTemp));
        node.QueueMessage(Id(node.GetId(), Endpoint::RoomTemp, Operation::Report), p, 2);
        reportedTemp = roomTemp;
    }
    if (force || !everReported ||
        (humidity > reportedHumidity ? humidity - reportedHumidity : reportedHumidity - humidity) >=
            HumidityDeadband)
    {
        uint8_t p[2];
        PackU16(p, humidity);
        node.QueueMessage(Id(node.GetId(), Endpoint::RoomHumidity, Operation::Report), p, 2);
        reportedHumidity = humidity;
    }
    if (force || !everReported || roomMode != reportedMode)
    {
        node.QueueMessage(Id(node.GetId(), Endpoint::RoomMode, Operation::Report), roomMode);
        reportedMode = roomMode;
    }
    everReported = true;
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

        case Endpoint::RoomTemp:
        case Endpoint::RoomHumidity:
        case Endpoint::RoomMode:
            if (m.id.operation == Operation::Get)
            {
                PublishRoom(true);
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

        default:
            break;
    }
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
