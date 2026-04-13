#pragma once

#include <memory>

namespace HeltecV3 {

    class LoraInterface;

    namespace RNodeBridge {

        /* Enter RNode bridge mode. Silences ESP_LOG on UART0, installs a
         * raw RX callback on the LoRa interface, drives UART0 as a KISS
         * command channel, and never returns. The caller must not start
         * Reticulum; in RNode mode the SX1262 is driven by the host. */
        [[noreturn]] void run(std::shared_ptr<LoraInterface> lora);

    }

}
