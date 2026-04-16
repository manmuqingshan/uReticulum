#pragma once

#include <functional>
#include <memory>

#include "rtreticulum/interface.h"

class EspIdfHal;
class SX1262;

namespace HeltecV3 {

    /* RTReticulum LoRa interface backed by RadioLib's SX1262 driver. RX is
     * triggered by DIO1 interrupt; the ISR sets a notification that the
     * interface task drains in loop(). */
    class LoraInterface : public RNS::InterfaceImpl {
    public:
        /* When set, incoming frames bypass handle_incoming() and go to this
         * callback instead. Used by the RNode bridge mode, where we want the
         * raw bytes plus RSSI/SNR to forward over KISS, not delivery into
         * the RTReticulum stack. */
        using RawRxCallback = std::function<void(const uint8_t* data, size_t len,
                                                 float rssi_dbm, float snr_db)>;

        /* Public so std::shared_ptr can construct/destruct via new/delete. */
        LoraInterface();
        ~LoraInterface() override;

        static std::shared_ptr<LoraInterface> create();

        bool start() override;
        void stop()  override;
        /* loop() is intentionally a no-op — the Reticulum task calls it, but
         * all SPI access must happen on the main task. Call poll() from the
         * main loop instead. */
        void loop()  override {}
        void poll();
        void send_outgoing(const RNS::Bytes& data) override;
        std::string toString() const override { return "LoraInterface[heltec_v3]"; }

        /* Raw byte path (RNode bridge mode). */
        void set_raw_rx_callback(RawRxCallback cb) { _raw_rx = std::move(cb); }
        void send_raw(const uint8_t* data, size_t len);

        /* Runtime radio parameter adjustment. Values match RadioLib units:
         * frequency in MHz, bandwidth in kHz, SF 5..12, CR 5..8, power dBm.
         * Returns true if the underlying SX1262 call succeeded. Callers in
         * RNode mode treat a failure as ERROR_INITRADIO. */
        bool set_frequency_mhz(float mhz);
        bool set_bandwidth_khz(float khz);
        bool set_spreading_factor(int sf);
        bool set_coding_rate(int cr);
        bool set_tx_power_dbm(int dbm);
        bool set_radio_online(bool online);
        bool radio_online() const { return _radio_on; }

        /* Drain the TX queue (called from loop on the main task). */
        void drain_tx_queue();

    private:
        EspIdfHal*  _hal     = nullptr;
        SX1262*     _radio   = nullptr;
        bool        _radio_on = false;
        RawRxCallback _raw_rx;
    };

}
