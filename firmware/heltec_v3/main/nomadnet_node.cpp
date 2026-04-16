#include "nomadnet_node.h"

#include <string.h>
#include <stdio.h>

#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "heltec_v3_pins.h"

#include "rtreticulum/destination.h"
#include "rtreticulum/identity.h"
#include "rtreticulum/link.h"
#include "rtreticulum/transport.h"

static const char* TAG = "nn_node";

namespace {

    RNS::Destination g_dest{RNS::Type::NONE};
    std::string g_node_name;
    adc_oneshot_unit_handle_t g_adc_handle = nullptr;
    adc_cali_handle_t g_adc_cali = nullptr;

    bool g_adc_active_high = true;  /* V3/V3.1=LOW, V3.2+=HIGH; auto-detected */

    /* Read battery voltage in millivolts. Returns 0 if ADC not initialized. */
    unsigned read_battery_mv() {
        if (!g_adc_handle) return 0;
        /* Enable the voltage divider. */
        gpio_set_level((gpio_num_t)HELTEC_V3_VBAT_CTRL, g_adc_active_high ? 1 : 0);
        /* Average 8 samples for stability. */
        int total = 0;
        for (int i = 0; i < 8; i++) {
            int raw = 0;
            adc_oneshot_read(g_adc_handle, ADC_CHANNEL_0, &raw);
            total += raw;
        }
        gpio_set_level((gpio_num_t)HELTEC_V3_VBAT_CTRL, g_adc_active_high ? 0 : 1);
        int avg = total / 8;
        int mv = 0;
        if (g_adc_cali) {
            adc_cali_raw_to_voltage(g_adc_cali, avg, &mv);
        } else {
            mv = avg * 3300 / 4095;
        }
        /* Heltec V3 voltage divider ratio ~4.9:1 (empirical constant 5.42
         * from MeshCore/RNode firmware, adjusted for calibrated mV input). */
        return (unsigned)((mv * 542) / 100);
    }

    /* Format uptime as "Xd Xh Xm Xs". */
    void fmt_uptime(char* buf, size_t len, uint64_t secs) {
        unsigned d = (unsigned)(secs / 86400);
        unsigned h = (unsigned)((secs % 86400) / 3600);
        unsigned m = (unsigned)((secs % 3600) / 60);
        unsigned s = (unsigned)(secs % 60);
        if (d > 0)      snprintf(buf, len, "%ud %uh %um %us", d, h, m, s);
        else if (h > 0) snprintf(buf, len, "%uh %um %us", h, m, s);
        else if (m > 0) snprintf(buf, len, "%um %us", m, s);
        else            snprintf(buf, len, "%us", s);
    }

    RNS::Bytes serve_index(const RNS::Bytes&, const RNS::Bytes&,
                           const RNS::Bytes&, const RNS::Bytes&,
                           const RNS::Identity&, double) {
        char page[2048];
        char uptime[32];
        uint64_t secs = esp_timer_get_time() / 1000000;
        fmt_uptime(uptime, sizeof(uptime), secs);

        unsigned free_heap = (unsigned)(esp_get_free_heap_size() / 1024);
        unsigned min_heap  = (unsigned)(esp_get_minimum_free_heap_size() / 1024);

        int8_t rssi = 0;
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) rssi = ap.rssi;

        unsigned bat_mv = read_battery_mv();

        int n = snprintf(page, sizeof(page),
            ">RTReticulum\n"
            "\n"
            "Uptime ........ %s\n\n"
            "Battery ....... %u.%02uV\n\n"
            "Free heap ..... %u KB (min %u KB)\n\n"
            "WiFi RSSI ..... %d dBm\n",
            uptime,
            bat_mv / 1000, (bat_mv % 1000) / 10,
            free_heap, min_heap,
            (int)rssi);

        /* Interfaces section */
        n += snprintf(page + n, sizeof(page) - n,
            "\n>Interfaces\n\n");
        for (auto& iface : RNS::Transport::interfaces()) {
            n += snprintf(page + n, sizeof(page) - n,
                "%s  TX %uB  RX %uB  %s\n\n",
                iface->name().c_str(),
                (unsigned)iface->txb(),
                (unsigned)iface->rxb(),
                iface->online() ? "online" : "offline");
        }

        /* Peers section — walk the path table */
        auto& paths = RNS::Transport::path_table();
        if (!paths.empty()) {
            n += snprintf(page + n, sizeof(page) - n,
                ">Known Peers\n\n");
            double now_s = (double)(esp_timer_get_time() / 1000000);
            for (auto& [dest_hash, entry] : paths) {
                /* Skip our own destinations */
                if (dest_hash == g_dest.hash()) continue;

                std::string hash_short = dest_hash.toHex().substr(0, 12);
                RNS::Bytes app_data = RNS::Identity::recall_app_data(dest_hash);
                std::string name;
                if (!app_data.empty()) {
                    name = std::string(reinterpret_cast<const char*>(app_data.data()),
                                       app_data.size());
                }

                char age[32];
                unsigned age_s = (unsigned)(now_s - entry.timestamp);
                fmt_uptime(age, sizeof(age), age_s);

                if (!name.empty()) {
                    n += snprintf(page + n, sizeof(page) - n,
                        "%s  %s  %u hop  %s ago\n\n",
                        hash_short.c_str(), name.c_str(),
                        (unsigned)entry.hops, age);
                } else {
                    n += snprintf(page + n, sizeof(page) - n,
                        "%s  %u hop  %s ago\n\n",
                        hash_short.c_str(),
                        (unsigned)entry.hops, age);
                }
                if ((size_t)n >= sizeof(page) - 64) break;
            }
        }

        n += snprintf(page + n, sizeof(page) - n,
            "-\n"
            "\nServed live by RTReticulum on ESP32-S3\n");

        if (n < 0 || (size_t)n >= sizeof(page)) n = sizeof(page) - 1;
        return RNS::Bytes(reinterpret_cast<const uint8_t*>(page), n);
    }

}

namespace HeltecV3::NomadnetNode {

void start(const RNS::Identity& identity, const char* node_name) {
    g_node_name = node_name;

    /* Initialize battery ADC (GPIO1 via ADC1_CH0).
     * Auto-detect ADC_CTRL polarity: V3/V3.1 boards have a pull-up on
     * GPIO37 (active LOW), V3.2+ have a transistor (active HIGH). Read
     * the pin as input first to detect. */
    {
        gpio_config_t det_cfg = {};
        det_cfg.pin_bit_mask = (1ULL << HELTEC_V3_VBAT_CTRL);
        det_cfg.mode = GPIO_MODE_INPUT;
        det_cfg.pull_up_en = GPIO_PULLUP_DISABLE;
        det_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
        gpio_config(&det_cfg);
        vTaskDelay(pdMS_TO_TICKS(10));
        g_adc_active_high = (gpio_get_level((gpio_num_t)HELTEC_V3_VBAT_CTRL) == 0);
    }
    gpio_config_t ctrl_cfg = {};
    ctrl_cfg.pin_bit_mask = (1ULL << HELTEC_V3_VBAT_CTRL);
    ctrl_cfg.mode = GPIO_MODE_OUTPUT;
    gpio_config(&ctrl_cfg);
    gpio_set_level((gpio_num_t)HELTEC_V3_VBAT_CTRL, g_adc_active_high ? 0 : 1);

    adc_oneshot_unit_init_cfg_t adc_cfg = {};
    adc_cfg.unit_id = ADC_UNIT_1;
    if (adc_oneshot_new_unit(&adc_cfg, &g_adc_handle) == ESP_OK) {
        adc_oneshot_chan_cfg_t ch_cfg = {};
        ch_cfg.atten = ADC_ATTEN_DB_12;
        ch_cfg.bitwidth = ADC_BITWIDTH_DEFAULT;
        adc_oneshot_config_channel(g_adc_handle, ADC_CHANNEL_0, &ch_cfg);

        adc_cali_curve_fitting_config_t cali_cfg = {};
        cali_cfg.unit_id = ADC_UNIT_1;
        cali_cfg.atten = ADC_ATTEN_DB_12;
        cali_cfg.bitwidth = ADC_BITWIDTH_DEFAULT;
        adc_cali_create_scheme_curve_fitting(&cali_cfg, &g_adc_cali);
    }

    g_dest = RNS::Destination(identity,
                              RNS::Type::Destination::IN,
                              RNS::Type::Destination::SINGLE,
                              "nomadnetwork",
                              "node");
    RNS::Transport::register_destination(g_dest);

    /* Register page handlers. The path must match what nomadnet's
     * Browser.py requests: "/page/index.mu" for the landing page. */
    g_dest.register_request_handler("/page/index.mu", serve_index);

    /* Accept incoming Links on this destination. When a peer opens a
     * Link, the Transport layer creates the Link; request handlers
     * fire on inbound REQUEST packets over that Link. */
    RNS::Transport::set_link_request_handler(
        [](const RNS::Destination& d, const RNS::Bytes& req, const RNS::Packet& pkt)
            -> std::shared_ptr<RNS::Link> {
            ESP_LOGI(TAG, "LINKREQUEST for dest %s (%zu bytes)",
                     d.hash().toHex().c_str(), (size_t)req.size());
            auto link = RNS::Link::validate_request(d, req, pkt);
            if (link) {
                ESP_LOGI(TAG, "Link validated, hash=%s",
                         link->hash().toHex().c_str());
            } else {
                ESP_LOGE(TAG, "Link validation FAILED");
            }
            return link;
        });

    /* Announce the node so nomadnet users can discover it. The
     * app_data is the node name as UTF-8 — that's what nomadnet's
     * Directory shows. */
    RNS::Bytes name_bytes(reinterpret_cast<const uint8_t*>(node_name),
                          strlen(node_name));
    g_dest.announce(name_bytes);
    ESP_LOGI(TAG, "nomadnet node '%s' online, dest=%s",
             node_name, g_dest.hash().toHex().c_str());
}

void announce() {
    if (!g_dest) return;
    RNS::Bytes name_bytes(reinterpret_cast<const uint8_t*>(g_node_name.c_str()),
                          g_node_name.size());
    g_dest.announce(name_bytes);
}

}
