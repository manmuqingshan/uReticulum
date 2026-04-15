#include "tcp_interface.h"

#include <string.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include "esp_log.h"
#include "esp_timer.h"

using namespace RNS;

static const char* TAG = "tcp_if";

/* Python Reticulum TCPClientInterface defaults to HDLC framing:
 * FLAG + HDLC_ESCAPE(payload) + FLAG. HDLC escape replaces 0x7D with
 * 0x7D 0x5D and 0x7E with 0x7D 0x5E. (KISS framing is optional and
 * off by default.) */

namespace {
    constexpr uint8_t HDLC_FLAG     = 0x7E;
    constexpr uint8_t HDLC_ESC      = 0x7D;
    constexpr uint8_t HDLC_ESC_MASK = 0x20;
}

namespace HeltecV3 {

TcpInterface::TcpInterface(const char* host, uint16_t port)
    : InterfaceImpl("tcp"), _host(host), _port(port) {}

TcpInterface::~TcpInterface() { disconnect(); }

std::shared_ptr<TcpInterface> TcpInterface::create(const char* host, uint16_t port) {
    return std::shared_ptr<TcpInterface>(new TcpInterface(host, port));
}

bool TcpInterface::try_connect() {
    struct addrinfo hints = {}, *res = nullptr;
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", _port);

    int rc = getaddrinfo(_host.c_str(), port_str, &hints, &res);
    if (rc != 0 || !res) {
        ESP_LOGW(TAG, "DNS resolve failed for %s: %d", _host.c_str(), rc);
        return false;
    }

    _sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (_sock < 0) {
        freeaddrinfo(res);
        return false;
    }

    /* Non-blocking connect with 5s timeout. */
    fcntl(_sock, F_SETFL, fcntl(_sock, F_GETFL) | O_NONBLOCK);
    rc = connect(_sock, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);

    if (rc < 0 && errno != EINPROGRESS) {
        close(_sock); _sock = -1;
        return false;
    }

    /* Wait for connect completion. */
    fd_set wset;
    FD_ZERO(&wset);
    FD_SET(_sock, &wset);
    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    rc = select(_sock + 1, nullptr, &wset, nullptr, &tv);
    if (rc <= 0) {
        close(_sock); _sock = -1;
        return false;
    }

    int err = 0;
    socklen_t len = sizeof(err);
    getsockopt(_sock, SOL_SOCKET, SO_ERROR, &err, &len);
    if (err != 0) {
        close(_sock); _sock = -1;
        return false;
    }

    /* Switch back to blocking with a short read timeout. */
    fcntl(_sock, F_SETFL, fcntl(_sock, F_GETFL) & ~O_NONBLOCK);
    tv = { .tv_sec = 0, .tv_usec = 10000 }; /* 10ms */
    setsockopt(_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    _connected = true;
    ESP_LOGI(TAG, "connected to %s:%u", _host.c_str(), _port);
    return true;
}

void TcpInterface::disconnect() {
    if (_sock >= 0) {
        close(_sock);
        _sock = -1;
    }
    _connected = false;
}

bool TcpInterface::start() {
    if (try_connect()) {
        _online = true;
        return true;
    }
    /* Will retry in loop(). */
    ESP_LOGW(TAG, "initial connect to %s:%u failed, will retry", _host.c_str(), _port);
    _online = true;  /* mark online so Transport still registers us */
    return true;
}

void TcpInterface::stop() {
    disconnect();
    _online = false;
}

void TcpInterface::loop() {
    uint64_t now = (uint64_t)(esp_timer_get_time() / 1000);

    if (!_connected) {
        if (now < _reconnect_at) return;
        if (try_connect()) return;
        _reconnect_at = now + 5000;  /* retry in 5s */
        return;
    }

    /* Drain all available HDLC-framed data from the TCP socket.
     * The socket has a 10ms recv timeout so this won't block long. */
    uint8_t byte;
    int n;
    while ((n = recv(_sock, &byte, 1, 0)) == 1) {
        if (byte == HDLC_FLAG) {
            if (_in_frame && _rx_len > 0) {
                ESP_LOGI(TAG, "RX %u bytes from TCP", (unsigned)_rx_len);
                this->handle_incoming(Bytes(_rx_buf, _rx_len));
            }
            _in_frame = true;
            _escape   = false;
            _rx_len   = 0;
        } else if (_in_frame) {
            if (byte == HDLC_ESC) {
                _escape = true;
            } else {
                if (_escape) {
                    byte ^= HDLC_ESC_MASK;
                    _escape = false;
                }
                if (_rx_len < sizeof(_rx_buf)) _rx_buf[_rx_len++] = byte;
            }
        }
    }
    if (n == 0) {
        ESP_LOGI(TAG, "peer closed connection");
        disconnect();
    } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        ESP_LOGW(TAG, "recv error: %d", errno);
        disconnect();
    }
}

void TcpInterface::send_outgoing(const Bytes& data) {
    if (!_connected || _sock < 0) return;
    ESP_LOGI(TAG, "TX %u bytes via TCP", (unsigned)data.size());
    _txb += data.size();

    /* HDLC frame: FLAG + escaped payload + FLAG */
    uint8_t buf[1400];
    size_t o = 0;
    buf[o++] = HDLC_FLAG;
    for (size_t i = 0; i < data.size() && o + 2 < sizeof(buf); ++i) {
        uint8_t b = data.data()[i];
        if (b == HDLC_FLAG || b == HDLC_ESC) {
            buf[o++] = HDLC_ESC;
            buf[o++] = b ^ HDLC_ESC_MASK;
        } else {
            buf[o++] = b;
        }
    }
    buf[o++] = HDLC_FLAG;
    if (send(_sock, buf, o, 0) != (int)o) {
        ESP_LOGW(TAG, "send failed");
        disconnect();
    }
}

}
