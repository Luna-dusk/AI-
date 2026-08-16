#include "ai_client.h"
#include "i2s.h"
#include "wavecode.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "mbedtls/base64.h"

/*
 * This client deliberately does not use esp_websocket_client.  That component
 * has been disconnecting this device immediately after a successful Upgrade.
 * One task owns the TCP socket; callers only enqueue complete WebSocket frames.
 */

static const char *TAG = "RAW_WS";

#define RAW_WS_TX_QUEUE_LEN       96
/*
 * A real TTS reply is base64 of seconds of 16 kHz/16-bit mono audio.  The HTTP
 * fallback path already accepts up to 512 KiB of decoded PCM, i.e. about 683 KiB
 * of base64.  The old 12 KiB cap made the client tear the socket down on the
 * first normal reply, which is the root cause of "server replies but the device
 * receives nothing and stays stuck".  The RX buffer must fit one whole message.
 */
#define RAW_WS_TEXT_MAX           (768 * 1024)
#define RAW_WS_RX_BUFFER_SIZE     (RAW_WS_TEXT_MAX + 32 * 1024)
#define RAW_WS_HTTP_HEADER_MAX    4096
#define RAW_WS_CONNECT_TIMEOUT_MS 8000
#define RAW_WS_RECV_TIMEOUT_MS    50
#define RAW_WS_SEND_TIMEOUT_MS    2000
#define RAW_WS_PING_INTERVAL_US   (20LL * 1000 * 1000)
#define RAW_WS_PONG_TIMEOUT_US    (35LL * 1000 * 1000)

typedef struct {
    uint8_t opcode;
    uint16_t len;
    uint8_t *data;
} raw_ws_tx_t;

static QueueHandle_t ws_tx_queue;
static SemaphoreHandle_t ws_ready_sem;
static SemaphoreHandle_t ws_done_sem;
static SemaphoreHandle_t ws_exit_sem;
static TaskHandle_t ws_task_handle;

static volatile int ws_socket = -1;
static volatile bool ws_stop;
static volatile bool ws_connected;
static volatile bool ws_stream_active;
static volatile bool ws_stream_failed;
static volatile bool ws_stream_has_tts;

static char ws_reply[2048];
static volatile bool ws_reply_ready;
static char ws_result_buf[1024];

static uint8_t *ws_rx_buffer;
static size_t ws_rx_len;
static char *ws_http_headers;
static char *ws_text_buffer;
static uint8_t ws_mask_buffer[512];
static uint8_t ws_frag_opcode;   /* 0x01/0x02 while a fragmented message is open */
static size_t ws_frag_len;
static volatile int64_t ws_last_pong_us;

static bool ws_alloc_buffers(void)
{
    if (ws_rx_buffer && ws_http_headers && ws_text_buffer) return true;
    ws_rx_buffer = heap_caps_malloc(RAW_WS_RX_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ws_http_headers = heap_caps_malloc(RAW_WS_HTTP_HEADER_MAX,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ws_text_buffer = heap_caps_malloc(RAW_WS_TEXT_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ws_rx_buffer || !ws_http_headers || !ws_text_buffer) {
        ESP_LOGW(TAG, "PSRAM WS buffers unavailable (internal=%u, spiram=%u)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        free(ws_rx_buffer);
        free(ws_http_headers);
        free(ws_text_buffer);
        ws_rx_buffer = NULL;
        ws_http_headers = NULL;
        ws_text_buffer = NULL;
        return false;
    }
    return true;
}

static void ws_signal_failure(void)
{
    ws_connected = false;
    ws_stream_failed = true;
    ws_stream_active = false;
    if (ws_ready_sem) xSemaphoreGive(ws_ready_sem);
    if (ws_done_sem) xSemaphoreGive(ws_done_sem);
}

static void ws_drain_semaphore(SemaphoreHandle_t sem)
{
    while (sem && xSemaphoreTake(sem, 0) == pdTRUE) {
    }
}

static void ws_free_tx_queue(void)
{
    raw_ws_tx_t message;
    while (ws_tx_queue && xQueueReceive(ws_tx_queue, &message, 0) == pdTRUE) {
        free(message.data);
    }
}

static int ws_set_timeouts(int fd)
{
    struct timeval receive_timeout = {
        .tv_sec = 0,
        .tv_usec = RAW_WS_RECV_TIMEOUT_MS * 1000,
    };
    struct timeval send_timeout = {
        .tv_sec = RAW_WS_SEND_TIMEOUT_MS / 1000,
        .tv_usec = (RAW_WS_SEND_TIMEOUT_MS % 1000) * 1000,
    };
    return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout,
                      sizeof(receive_timeout)) == 0 &&
           setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout,
                      sizeof(send_timeout)) == 0 ? 0 : -1;
}

static int ws_send_all(int fd, const uint8_t *data, size_t len)
{
    size_t sent = 0;
    int timeouts = 0;
    while (sent < len && !ws_stop) {
        int n = send(fd, data + sent, len - sent, 0);
        if (n > 0) {
            sent += (size_t)n;
            timeouts = 0;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && ++timeouts < 2) continue;
        return -1;
    }
    return sent == len ? 0 : -1;
}

static int ws_parse_url(const char *url, char *host, size_t host_size,
                        char *path, size_t path_size, int *port)
{
    const char *p = url;
    if (strncmp(p, "ws://", 5) != 0) return -1;
    p += 5;

    const char *path_start = strchr(p, '/');
    const char *host_end = path_start ? path_start : p + strlen(p);
    const char *colon = NULL;
    for (const char *it = p; it < host_end; ++it) {
        if (*it == ':') colon = it;
    }

    const char *name_end = colon ? colon : host_end;
    size_t host_len = (size_t)(name_end - p);
    if (host_len == 0 || host_len >= host_size) return -1;
    memcpy(host, p, host_len);
    host[host_len] = '\0';

    *port = 80;
    if (colon) {
        char port_text[8];
        size_t port_len = (size_t)(host_end - colon - 1);
        if (port_len == 0 || port_len >= sizeof(port_text)) return -1;
        memcpy(port_text, colon + 1, port_len);
        port_text[port_len] = '\0';
        *port = atoi(port_text);
        if (*port <= 0 || *port > 65535) return -1;
    }

    if (path_start) {
        size_t path_len = strlen(path_start);
        if (path_len >= path_size) return -1;
        memcpy(path, path_start, path_len + 1);
    } else {
        strcpy(path, "/");
    }
    return 0;
}

static int ws_tcp_connect(void)
{
    char host[80];
    char path[128];
    int port;
    if (ws_parse_url(WS_SERVER_URL, host, sizeof(host), path, sizeof(path), &port) != 0) {
        ESP_LOGE(TAG, "invalid WS_SERVER_URL");
        return -1;
    }

    struct addrinfo hints = {0};
    struct addrinfo *addresses = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, NULL, &hints, &addresses) != 0 || !addresses) {
        ESP_LOGE(TAG, "DNS failed for %s", host);
        return -1;
    }

    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0 || ws_set_timeouts(fd) != 0) {
        if (fd >= 0) close(fd);
        freeaddrinfo(addresses);
        return -1;
    }

    struct sockaddr_in destination = *(const struct sockaddr_in *)addresses->ai_addr;
    destination.sin_port = htons((uint16_t)port);
    freeaddrinfo(addresses);

    ws_socket = fd;  /* Allows deinit to interrupt a blocking connect. */
    if (connect(fd, (struct sockaddr *)&destination, sizeof(destination)) != 0) {
        ESP_LOGW(TAG, "TCP connect failed, errno=%d", errno);
        close(fd);
        ws_socket = -1;
        return -1;
    }
    return fd;
}

static int ws_read_http_headers(int fd)
{
    size_t used = 0;
    int64_t deadline = esp_timer_get_time() + (int64_t)RAW_WS_CONNECT_TIMEOUT_MS * 1000;
    while (used < RAW_WS_HTTP_HEADER_MAX - 1 && !ws_stop) {
        int n = recv(fd, ws_http_headers + used, RAW_WS_HTTP_HEADER_MAX - 1 - used, 0);
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                if (esp_timer_get_time() < deadline) continue;
                ESP_LOGW(TAG, "Upgrade response timeout");
                return -1;
            }
            ESP_LOGW(TAG, "Upgrade response read=%d, errno=%d", n, errno);
            return -1;
        }
        used += (size_t)n;
        ws_http_headers[used] = '\0';
        char *end = strstr(ws_http_headers, "\r\n\r\n");
        if (!end) continue;

        size_t header_len = (size_t)(end + 4 - ws_http_headers);
        if (strncmp(ws_http_headers, "HTTP/1.1 101", 12) != 0 &&
            strncmp(ws_http_headers, "HTTP/1.0 101", 12) != 0) {
            ESP_LOGW(TAG, "Upgrade rejected: %.80s", ws_http_headers);
            return -1;
        }
        ws_rx_len = used - header_len;
        if (ws_rx_len) memcpy(ws_rx_buffer, ws_http_headers + header_len, ws_rx_len);
        return 0;
    }
    ESP_LOGW(TAG, "Upgrade response headers too large");
    return -1;
}

static int ws_send_frame(int fd, uint8_t opcode, const uint8_t *payload, size_t len)
{
    if (len > 0xffff) return -1;
    uint8_t header[8];
    size_t header_len = 0;
    header[header_len++] = 0x80 | (opcode & 0x0f);
    if (len < 126) {
        header[header_len++] = 0x80 | (uint8_t)len;
    } else {
        header[header_len++] = 0x80 | 126;
        header[header_len++] = (uint8_t)(len >> 8);
        header[header_len++] = (uint8_t)len;
    }

    uint8_t mask[4];
    uint32_t random = esp_random();
    memcpy(mask, &random, sizeof(mask));
    memcpy(header + header_len, mask, sizeof(mask));
    header_len += sizeof(mask);

    if (ws_send_all(fd, header, header_len) != 0) return -1;
    if (len == 0) return 0;

    for (size_t offset = 0; offset < len; ) {
        size_t chunk = len - offset;
        if (chunk > sizeof(ws_mask_buffer)) chunk = sizeof(ws_mask_buffer);
        for (size_t i = 0; i < chunk; ++i) {
            ws_mask_buffer[i] = payload[offset + i] ^ mask[(offset + i) & 3];
        }
        if (ws_send_all(fd, ws_mask_buffer, chunk) != 0) return -1;
        offset += chunk;
    }
    return 0;
}

static int ws_json_string(const char *json, const char *key, char *out, size_t out_size)
{
    char needle[40];
    int written = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (written < 0 || (size_t)written >= sizeof(needle)) return -1;
    const char *p = strstr(json, needle);
    if (!p) return -1;
    p += written;
    while (*p && isspace((unsigned char)*p)) ++p;
    if (*p++ != ':') return -1;
    while (*p && isspace((unsigned char)*p)) ++p;
    if (*p++ != '"') return -1;

    size_t copied = 0;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) ++p;
        if (copied + 1 >= out_size) return -1;
        out[copied++] = *p++;
    }
    if (*p != '"') return -1;
    out[copied] = '\0';
    return (int)copied;
}

/* Like ws_json_string but returns a pointer into the JSON text without copying. */
static const char *ws_json_string_ptr(const char *json, const char *key, size_t *out_len)
{
    char needle[40];
    int written = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (written < 0 || (size_t)written >= sizeof(needle)) return NULL;
    const char *p = strstr(json, needle);
    if (!p) return NULL;
    p += written;
    while (*p && isspace((unsigned char)*p)) ++p;
    if (*p++ != ':') return NULL;
    while (*p && isspace((unsigned char)*p)) ++p;
    if (*p++ != '"') return NULL;
    const char *start = p;
    while (*p && *p != '"') ++p;
    if (*p != '"') return NULL;
    *out_len = (size_t)(p - start);
    return start;
}

static void ws_handle_text(const char *text)
{
    char type[16];
    if (ws_json_string(text, "type", type, sizeof(type)) < 0) {
        ESP_LOGW(TAG, "invalid text frame");
        return;
    }

    if (strcmp(type, "ready") == 0) {
        ws_connected = true;
        if (ws_ready_sem) xSemaphoreGive(ws_ready_sem);
        ESP_LOGI(TAG, "server ready");
        return;
    }
    if (strcmp(type, "reply") == 0) {
        if (ws_json_string(text, "text", ws_reply, sizeof(ws_reply)) >= 0) {
            ws_reply_ready = true;
            ESP_LOGI(TAG, "reply: %s", ws_reply);
        }
        return;
    }
    if (strcmp(type, "tts") == 0) {
        size_t encoded_len = 0;
        const char *encoded = ws_json_string_ptr(text, "data", &encoded_len);
        if (!encoded || encoded_len == 0) return;

        size_t pcm_capacity = encoded_len / 4 * 3 + 4;
        uint8_t *pcm = heap_caps_malloc(pcm_capacity, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
        if (!pcm) pcm = malloc(pcm_capacity);
        if (!pcm) return;
        size_t pcm_len = 0;
        if (mbedtls_base64_decode(pcm, pcm_capacity, &pcm_len,
                                  (const uint8_t *)encoded, encoded_len) != 0 ||
            pcm_len == 0) {
            free(pcm);
            return;
        }
        /* The server may send a full WAV; strip the header like the HTTP path. */
        if (pcm_len > WAV_HEADER_SIZE && memcmp(pcm, "RIFF", 4) == 0) {
            memmove(pcm, pcm + WAV_HEADER_SIZE, pcm_len - WAV_HEADER_SIZE);
            pcm_len -= WAV_HEADER_SIZE;
        }
        if (i2s_play_async((int16_t *)pcm, pcm_len)) {
            ws_stream_has_tts = true;
        }
        return;
    }
    if (strcmp(type, "done") == 0) {
        ws_stream_active = false;
        if (ws_done_sem) xSemaphoreGive(ws_done_sem);
        ESP_LOGI(TAG, "stream done");
    }
}

static void ws_handle_frame(uint8_t opcode, bool fin, const uint8_t *payload, size_t len)
{
    if (opcode == 0x09) {
        if (ws_socket >= 0 && ws_send_frame(ws_socket, 0x0a, payload, len) != 0) {
            ws_signal_failure();
        }
        return;
    }
    if (opcode == 0x0a) {
        ws_last_pong_us = esp_timer_get_time();
        return;
    }
    if (opcode == 0x08) {
        if (ws_socket >= 0) ws_send_frame(ws_socket, 0x08, payload, len);
        ws_signal_failure();
        return;
    }
    if (opcode != 0x00 && opcode != 0x01 && opcode != 0x02) {
        ws_signal_failure();
        return;
    }
    if (len >= RAW_WS_TEXT_MAX) {
        ws_signal_failure();
        return;
    }

    /* RFC6455 fragmentation: accumulate continuation frames into ws_text_buffer. */
    if (opcode == 0x00) {
        if (ws_frag_opcode == 0) {
            ws_signal_failure();
            return;
        }
    } else {
        if (ws_frag_opcode != 0) {
            ws_signal_failure();
            return;
        }
        if (!fin) {
            ws_frag_opcode = opcode;
            ws_frag_len = 0;
        }
    }

    if (ws_frag_opcode != 0) {
        if (ws_frag_len + len >= RAW_WS_TEXT_MAX) {
            ws_signal_failure();
            return;
        }
        memcpy(ws_text_buffer + ws_frag_len, payload, len);
        ws_frag_len += len;
        if (!fin) return;
        ws_text_buffer[ws_frag_len] = '\0';
        opcode = ws_frag_opcode;
        ws_frag_opcode = 0;
        len = ws_frag_len;
        ws_frag_len = 0;
    } else {
        memcpy(ws_text_buffer, payload, len);
        ws_text_buffer[len] = '\0';
    }

    if (opcode == 0x01) {
        if (len == 0) {
            ws_signal_failure();
            return;
        }
        ws_handle_text(ws_text_buffer);
    } else if (opcode == 0x02 && len > 0 && ws_stream_active) {
        int16_t *pcm = heap_caps_malloc(len, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
        if (!pcm) pcm = malloc(len);
        if (pcm) {
            memcpy(pcm, ws_text_buffer, len);
            if (i2s_play_async(pcm, len)) ws_stream_has_tts = true;
        }
    }
}

static int ws_consume_frames(void)
{
    size_t offset = 0;
    while (ws_rx_len - offset >= 2) {
        const uint8_t *frame = ws_rx_buffer + offset;
        uint8_t opcode = frame[0] & 0x0f;
        bool fin = (frame[0] & 0x80) != 0;
        bool masked = (frame[1] & 0x80) != 0;
        uint64_t payload_len = frame[1] & 0x7f;
        size_t header_len = 2;
        if (payload_len == 126) {
            if (ws_rx_len - offset < 4) break;
            payload_len = ((uint64_t)frame[2] << 8) | frame[3];
            header_len = 4;
        } else if (payload_len == 127) {
            if (ws_rx_len - offset < 10) break;
            payload_len = 0;
            for (int i = 0; i < 8; ++i) payload_len = (payload_len << 8) | frame[2 + i];
            if (frame[2] & 0x80) {
                ESP_LOGW(TAG, "invalid 64-bit length");
                return -1;
            }
            header_len = 10;
        }
        if (masked) {
            ESP_LOGW(TAG, "masked server frame");
            return -1;
        }
        if (payload_len > RAW_WS_TEXT_MAX ||
            payload_len + header_len > RAW_WS_RX_BUFFER_SIZE) {
            ESP_LOGW(TAG, "unsupported incoming frame, opcode=%u len=%u", opcode,
                     (unsigned)payload_len);
            return -1;
        }
        if (ws_rx_len - offset < header_len + (size_t)payload_len) break;
        ws_handle_frame(opcode, fin, frame + header_len, (size_t)payload_len);
        offset += header_len + (size_t)payload_len;
        if (!ws_connected && ws_stream_failed) return -1;
    }
    if (offset) {
        ws_rx_len -= offset;
        if (ws_rx_len) memmove(ws_rx_buffer, ws_rx_buffer + offset, ws_rx_len);
    }
    return 0;
}

static void raw_ws_task(void *arg)
{
    (void)arg;
    int fd = ws_tcp_connect();
    if (fd < 0) {
        ESP_LOGW(TAG, "TCP connect failed");
        ws_signal_failure();
        goto exit;
    }
    ws_socket = fd;

    char host[80];
    char path[128];
    int port;
    uint8_t key_raw[16];
    char key[32];
    size_t key_len = 0;
    if (ws_parse_url(WS_SERVER_URL, host, sizeof(host), path, sizeof(path), &port) != 0) {
        ws_signal_failure();
        goto close_socket;
    }
    for (size_t i = 0; i < sizeof(key_raw); i += sizeof(uint32_t)) {
        uint32_t random = esp_random();
        memcpy(key_raw + i, &random, sizeof(random));
    }
    if (mbedtls_base64_encode((uint8_t *)key, sizeof(key), &key_len,
                              key_raw, sizeof(key_raw)) != 0) {
        ws_signal_failure();
        goto close_socket;
    }
    key[key_len] = '\0';

    char request[512];
    int request_len;

    uint8_t mac[6];
    char device_key[18];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(device_key, sizeof(device_key), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    request_len = snprintf(request, sizeof(request),
        "GET %s HTTP/1.1\r\nHost: %s%s%d\r\nUpgrade: websocket\r\n"
        "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\nX-Device-Key: %s\r\n"
        "User-Agent: ESP32 Raw WebSocket\r\n\r\n",
        path, host, port == 80 ? "" : ":", port == 80 ? 0 : port, key, device_key);
    if (port == 80) {
        request_len = snprintf(request, sizeof(request),
            "GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\n"
            "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
            "Sec-WebSocket-Version: 13\r\nX-Device-Key: %s\r\n"
            "User-Agent: ESP32 Raw WebSocket\r\n\r\n",
            path, host, key, device_key);
    }
    if (request_len <= 0 || request_len >= (int)sizeof(request) ||
        ws_send_all(fd, (const uint8_t *)request, (size_t)request_len) != 0 ||
        ws_read_http_headers(fd) != 0) {
        ESP_LOGW(TAG, "WebSocket Upgrade failed");
        ws_signal_failure();
        goto close_socket;
    }

    ws_last_pong_us = esp_timer_get_time();
    int64_t last_ping_us = ws_last_pong_us;
    ESP_LOGI(TAG, "Upgrade complete, waiting for server ready");
    while (!ws_stop) {
        raw_ws_tx_t message;
        while (xQueueReceive(ws_tx_queue, &message, 0) == pdTRUE) {
            int send_result = ws_send_frame(fd, message.opcode, message.data, message.len);
            free(message.data);
            if (send_result != 0) {
                ESP_LOGW(TAG, "frame write failed");
                ws_signal_failure();
                goto close_socket;
            }
        }

        if (ws_consume_frames() != 0) {
            ws_signal_failure();
            goto close_socket;
        }
        int64_t now = esp_timer_get_time();
        if (now - ws_last_pong_us > RAW_WS_PONG_TIMEOUT_US ||
            (now - last_ping_us >= RAW_WS_PING_INTERVAL_US &&
             ws_send_frame(fd, 0x09, NULL, 0) != 0)) {
            ESP_LOGW(TAG, "keepalive failed");
            ws_signal_failure();
            goto close_socket;
        }
        if (now - last_ping_us >= RAW_WS_PING_INTERVAL_US) last_ping_us = now;
        if (ws_rx_len >= RAW_WS_RX_BUFFER_SIZE) {
            ESP_LOGW(TAG, "receive buffer full");
            ws_signal_failure();
            goto close_socket;
        }
        int received = recv(fd, ws_rx_buffer + ws_rx_len,
                            RAW_WS_RX_BUFFER_SIZE - ws_rx_len, 0);
        if (received > 0) {
            ws_rx_len += (size_t)received;
            continue;
        }
        if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        ESP_LOGW(TAG, "socket closed, errno=%d", errno);
        ws_signal_failure();
        goto close_socket;
    }

close_socket:
    ws_free_tx_queue();
    shutdown(fd, SHUT_RDWR);
    close(fd);
    ws_socket = -1;
exit:
    ws_connected = false;
    ws_task_handle = NULL;
    if (ws_exit_sem) xSemaphoreGive(ws_exit_sem);
    vTaskDelete(NULL);
}

int ws_client_init(void)
{
    if (ws_connected) return 0;
    if (ws_task_handle) return -1;

    if (!ws_tx_queue) ws_tx_queue = xQueueCreate(RAW_WS_TX_QUEUE_LEN, sizeof(raw_ws_tx_t));
    if (!ws_ready_sem) ws_ready_sem = xSemaphoreCreateBinary();
    if (!ws_done_sem) ws_done_sem = xSemaphoreCreateBinary();
    if (!ws_exit_sem) ws_exit_sem = xSemaphoreCreateBinary();
    if (!ws_tx_queue || !ws_ready_sem || !ws_done_sem || !ws_exit_sem) return -1;
    if (!ws_alloc_buffers()) return -1;

    ws_drain_semaphore(ws_ready_sem);
    ws_drain_semaphore(ws_done_sem);
    ws_drain_semaphore(ws_exit_sem);
    ws_stop = false;
    ws_connected = false;
    ws_stream_failed = false;
    ws_rx_len = 0;
    ws_frag_opcode = 0;
    ws_frag_len = 0;

    if (xTaskCreate(raw_ws_task, "raw_ws", 6144, NULL, 5, &ws_task_handle) != pdPASS) {
        ws_task_handle = NULL;
        return -1;
    }
    if (xSemaphoreTake(ws_ready_sem, pdMS_TO_TICKS(10000)) != pdTRUE || !ws_connected) {
        ESP_LOGW(TAG, "server did not send ready");
        ws_client_deinit();
        return -1;
    }
    return 0;
}

void ws_client_deinit(void)
{
    ws_stop = true;
    int fd = ws_socket;
    if (fd >= 0) shutdown(fd, SHUT_RDWR);
    if (ws_task_handle && ws_exit_sem) {
        xSemaphoreTake(ws_exit_sem, pdMS_TO_TICKS(1000));
    }
    ws_connected = false;
    ws_stream_active = false;
    ws_free_tx_queue();
}

int ws_client_send_pcm(const uint8_t *pcm_data, int pcm_len)
{
    if (!pcm_data || pcm_len <= 0 || pcm_len > 0xffff || !ws_connected || !ws_tx_queue) return -1;
    uint8_t *copy = heap_caps_malloc((size_t)pcm_len, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
    if (!copy) copy = malloc((size_t)pcm_len);
    if (!copy) return -1;
    memcpy(copy, pcm_data, (size_t)pcm_len);
    raw_ws_tx_t message = { .opcode = 0x02, .len = (uint16_t)pcm_len, .data = copy };
    if (xQueueSend(ws_tx_queue, &message, 0) != pdTRUE) {
        free(copy);
        ESP_LOGW(TAG, "PCM queue full");
        return -1;
    }
    return 0;
}

bool ws_client_is_connected(void)
{
    return ws_connected && !ws_stop && ws_socket >= 0;
}

bool ws_client_has_tts(void)
{
    return ws_stream_has_tts;
}

int ws_client_send_end(void)
{
    static const char end_message[] = "{\"type\":\"end\"}";
    if (!ws_client_is_connected()) return -1;

    ws_reply[0] = '\0';
    ws_reply_ready = false;
    ws_stream_failed = false;
    ws_stream_has_tts = false;
    ws_stream_active = true;
    ws_frag_opcode = 0;
    ws_frag_len = 0;
    ws_drain_semaphore(ws_done_sem);

    uint8_t *copy = malloc(sizeof(end_message) - 1);
    if (!copy) return -1;
    memcpy(copy, end_message, sizeof(end_message) - 1);
    raw_ws_tx_t message = { .opcode = 0x01, .len = sizeof(end_message) - 1, .data = copy };
    if (xQueueSend(ws_tx_queue, &message, 0) != pdTRUE) {
        free(copy);
        ws_stream_failed = true;
        ws_stream_active = false;
        return -1;
    }
    return 0;
}

int ws_client_wait_done(TickType_t timeout)
{
    if (!ws_task_handle) return -1;
    if (xSemaphoreTake(ws_done_sem, timeout) != pdTRUE) {
        ESP_LOGW(TAG, "stream done timeout");
        ws_stream_failed = true;
        ws_stream_active = false;
        return -1;
    }
    return ws_stream_failed ? -1 : 0;
}

const char *ws_client_get_reply(void)
{
    return ws_reply_ready ? ws_reply : NULL;
}

int ws_client_end_utterance(const char **result, int *result_len)
{
    if (!result || !result_len || ws_client_send_end() != 0 ||
        ws_client_wait_done(pdMS_TO_TICKS(10000)) != 0) {
        return -1;
    }
    int len = snprintf(ws_result_buf, sizeof(ws_result_buf), "{\"reply\":\"%s\"}",
                       ws_reply_ready ? ws_reply : "");
    if (len < 0 || len >= (int)sizeof(ws_result_buf)) return -1;
    *result = ws_result_buf;
    *result_len = len;
    return 0;
}
