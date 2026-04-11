# uReticulum

The `u` carries three meanings at once: **Unified** (one codebase merging
microReticulum, RNode, and RNode CE), **micro** (`µ`, a nod to the
microReticulum heritage), and **Ultimate** (the version that drops the
Arduino framework for FreeRTOS).

A [Reticulum](https://reticulum.network) node stack for embedded
microcontrollers (Nordic nRF, STM32, and others), built on **FreeRTOS**.

uReticulum unifies three existing projects into one codebase:

| Upstream | What uReticulum takes from it |
|---|---|
| [microReticulum](../microReticulum) | The protocol implementation: Identity, Destination, Packet, Link, Transport, Interface. |
| [RNode_Firmware](../RNode_Firmware) | Node behaviour: KISS framing, serial host protocol, LoRa modem configuration, RNode command set. |
| [RNode_Firmware_CE](../RNode_Firmware_CE) | Additional hardware support and Community Edition features. |

## Why rebuild

All three upstream projects are Arduino based. That worked but is now
the biggest thing standing between RNode class hardware and its
potential.

The Arduino framework:

- **Leaves power on the table.** `loop()` is a busy wait. Delay primitives
  are blocking. No tickless idle, no principled low power modes. For
  battery powered mesh nodes this is the single biggest source of
  avoidable current draw.
- **Ties the codebase to a single task.** On ESP32, one core sits idle
  while the other churns through packets, crypto, and UI in sequence.
- **Couples the protocol stack to the transport.** A slow crypto
  operation stalls radio reception. A busy interface starves the display.
- **Pins us to Arduino ecosystem libraries** (ArduinoJson, sandeepmistry
  LoRa, rweather Crypto, hideakitai MsgPack) that are effectively
  unmaintained.

## Why FreeRTOS

- **Power efficiency.** Tickless idle drops the MCU into deep sleep
  whenever no task is runnable. Receive listen current can go from tens
  of milliamps to hundreds of microamps.
- **Preemption.** Transport, each interface RX, and the serial host run
  at their own priorities. Crypto no longer blocks a radio interrupt.
- **Task isolation.** Each interface owns its stack, its priority, and
  its queue into transport. Bugs stay contained.
- **Portability.** FreeRTOS runs on Nordic nRF, STM32, and every other
  MCU family worth targeting, behind the same C API.
- **Mature tooling.** Static analysis, unit testing, and ThreadSanitizer
  on the host simulator build all work out of the box.

## Why RadioLib

The upstream projects each use a different radio library. Consolidating
is worthwhile on its own, but [RadioLib](https://github.com/jgromes/RadioLib)
is specifically the right choice:

- **One driver, every chip we care about.** SX127x, SX126x, SX128x, and
  LR11x0 families plus FSK parts. Swapping radios between boards becomes
  a constructor argument.
- **Explicit non-Arduino HAL.** `RadioLibHal` is a pure virtual class
  consumers subclass. Example ports for ESP-IDF, Pico, Raspberry Pi, and
  Tock already live in the repo.
- **Actively maintained.** New chip families land upstream within weeks
  of silicon availability.
- **MIT licensed.** Compatible with everything else in the tree.
- **Lets us think in `Interface`, not in pins.** With the radio
  abstracted, our Reticulum `Interface` becomes *"give me N bytes, I'll
  give you back a modulated packet,"* with no chip specific code leaking
  into the network stack.
