/*
 * uReticulum on Heltec WiFi LoRa 32 V3
 *
 * Build + flash (from this firmware/heltec_v3/ directory):
 *   nix develop ../..
 *   idf.py build flash -p /dev/ttyUSB0
 */

#include <atomic>
#include <stdio.h>
#include <string>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ureticulum/destination.h"
#include "ureticulum/identity.h"
#include "ureticulum/reticulum.h"
#include "ureticulum/transport.h"

#include "lora_interface.h"
#include "oled.h"

static const char* TAG = "app";

namespace {
    std::atomic<uint32_t> g_announce_count{0};
    std::atomic<uint32_t> g_rx_count{0};

    /* Render a concise 8-line status page. */
    void render_status(const std::string& id_hex,
                       const std::string& dst_hex) {
        HeltecV3::Oled::clear();

        /* Row 0: title */
        HeltecV3::Oled::print(0, 0, "uReticulum");
        HeltecV3::Oled::hline(9);

        /* Rows 2,3: identity + destination hashes (first 12 hex chars). */
        char line[24];
        snprintf(line, sizeof(line), "ID  %.12s", id_hex.c_str());
        HeltecV3::Oled::print(2, 0, line);
        snprintf(line, sizeof(line), "DST %.12s", dst_hex.c_str());
        HeltecV3::Oled::print(3, 0, line);

        /* Row 5: radio params */
        HeltecV3::Oled::print(5, 0, "915.0 SF9 BW125 US");

        /* Row 7: counters */
        snprintf(line, sizeof(line), "TX %-4u  RX %-4u",
                 (unsigned)g_announce_count.load(),
                 (unsigned)g_rx_count.load());
        HeltecV3::Oled::print(7, 0, line);

        HeltecV3::Oled::flush();
    }
}

extern "C" void app_main() {
    ESP_LOGI(TAG, "uReticulum on Heltec V3 starting");

    /* Bring up the OLED first so we can show status during bring-up. */
    bool oled_up = HeltecV3::Oled::init();
    if (oled_up) {
        HeltecV3::Oled::clear();
        HeltecV3::Oled::print(0, 0, "uReticulum");
        HeltecV3::Oled::hline(9);
        HeltecV3::Oled::print(2, 0, "Booting...");
        HeltecV3::Oled::flush();
    }

    /* Bring up the LoRa interface. */
    auto lora = HeltecV3::LoraInterface::create();
    if (!lora->start()) {
        ESP_LOGE(TAG, "LoRa interface failed to start, halting");
        if (oled_up) {
            HeltecV3::Oled::set_line(4, "LoRa init FAIL");
            HeltecV3::Oled::flush();
        }
        while (true) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    RNS::Transport::register_interface(lora);

    if (oled_up) {
        HeltecV3::Oled::set_line(4, "LoRa online");
        HeltecV3::Oled::flush();
    }

    /* Create identity + SINGLE/IN destination. */
    RNS::Identity identity;
    RNS::Destination dest(identity,
                          RNS::Type::Destination::IN,
                          RNS::Type::Destination::SINGLE,
                          "ureticulum",
                          "heltec_v3");
    RNS::Transport::register_destination(dest);

    std::string id_hex  = identity.hash().toHex();
    std::string dst_hex = dest.hash().toHex();
    ESP_LOGI(TAG, "identity hash: %s", id_hex.c_str());
    ESP_LOGI(TAG, "destination hash: %s", dst_hex.c_str());

    /* Global on_announce hook — bumps RX count whenever Transport validates
     * an incoming announce. Good sanity check that the radio receive path
     * is healthy. */
    RNS::Transport::on_announce([](const RNS::Bytes& dh, const RNS::Identity&, const RNS::Bytes&) {
        (void)dh;
        g_rx_count.fetch_add(1);
    });

    if (!RNS::Reticulum::start(/*tick_ms=*/50, /*stack_words=*/8192, /*priority=*/5)) {
        ESP_LOGE(TAG, "Reticulum::start failed");
    }

    /* Initial status render. */
    if (oled_up) render_status(id_hex, dst_hex);

    /* Announce loop. */
    while (true) {
        ESP_LOGI(TAG, "announcing %s", dst_hex.c_str());
        try {
            dest.announce(RNS::Bytes("hello from heltec v3"));
            g_announce_count.fetch_add(1);
        } catch (const std::exception& e) {
            ESP_LOGW(TAG, "announce failed: %s", e.what());
        }
        if (oled_up) render_status(id_hex, dst_hex);
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}
