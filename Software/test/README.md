# Host unit tests

The firmware is cross-compiled for the Cortex-M0+ and can't run on a CI
runner, so the portable logic is compiled **for the host** against a set of
fakes and exercised with `ctest`.

```
make -C Software test          # configure + build + ctest
# or
cmake -S Software/test -B Software/build-test
cmake --build Software/build-test
ctest --test-dir Software/build-test --output-on-failure
```

## Layout

```
test/
├── CMakeLists.txt      standalone host project (not part of the ARM build)
├── framework/          Test.h/.cpp  -- tiny xUnit harness (CC_TEST / CC_CHECK)
│                       BusHelpers   -- encode/decode NodeLib frames over FakeBus
└── fake/               host doubles for Lib/HAL + NodeLib::ConfigStore
    ├── include/stm32g0xx_hal.h   dummy GPIO_TypeDef + CMSIS intrinsics
    ├── FakeClock       Hal::Tick  -- test-controlled millisecond clock
    ├── FakeBus         Hal::Uart  -- one shared RS485 segment (Tx capture / Rx inject)
    ├── FakeCrc         Hal::Crc   -- software CRC-16/CCITT-FALSE + CRC-32/MPEG-2
    ├── FakeGpio        Hal::Gpio  -- register-block stub + the GPIOx pointers
    ├── FakeSystem      Hal::System
    ├── FakeBackup      Hal::Backup -- in-memory backup registers
    ├── FakeConfigStore NodeLib::ConfigStore -- settable provisioned identity
    └── FakeImageDescriptor        the linker-placed gImageDescriptor symbol
```

Each module keeps its own tests next to the code:

| Suite | Covers |
|---|---|
| `Lib/Tools/test/DelayTimerTests`        | `Tools::DelayTimer` deadlines / restart / stop |
| `Lib/HAL/test/CrcTests`                 | CRC-16/CRC-32 known-answer vectors |
| `Lib/HAL/test/OneWireTests`             | `Hal::OneWire::Crc8` (Dallas CRC-8) |
| `Lib/NodeLib/test/IdTests`              | `Id` ordering/equality, `Message` constructors |
| `Lib/NodeLib/test/FrameTests`           | v2 framing: round-trip, CRC reject, resync, timeout |
| `Lib/NodeLib/test/NodeMasterTests`      | discovery roster + round-robin poll sequencing |
| `Modules/TemperatureNode/test/Ds18b20Tests`     | scratchpad decode, CRC / stuck-line rejection |
| `Modules/TemperatureNode/test/DuctChannelTests` | sample cycle, auto-report, reconnect re-send |

## Adding a test

Drop a `FooTests.cpp` in the relevant `test/` folder and register it:

```cmake
cc_add_test(FooTests SOURCES FooTests.cpp)
```

`cc_add_test` links the harness, the fakes, and the portable production code
(`nodelib_host`), and registers a `ctest` entry. Add production `.cpp` files
or extra fakes to `SOURCES`, and extra header dirs via `INCLUDES`.
