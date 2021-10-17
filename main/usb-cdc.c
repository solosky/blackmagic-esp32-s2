#include <stdint.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/stream_buffer.h>
#include <tinyusb.h>
#include <tusb_cdc_acm.h>
#include <sdkconfig.h>
#include "usb-cdc.h"

static const char* TAG = "usb-cdc";
static uint8_t buffer_rx[CONFIG_USB_CDC_RX_BUFSIZE + 1];

typedef struct {
    volatile bool connected;
    volatile bool dtr;
    volatile bool rts;

    StreamBufferHandle_t rx_stream;
    volatile bool rx_stream_full;
} FuriHalVcp;

static FuriHalVcp furi_hal_vcp;

size_t usb_cdc_rx(uint8_t* buffer, size_t size) {
    return usb_cdc_rx_with_timeout(buffer, size, portMAX_DELAY);
}

size_t usb_cdc_rx_with_timeout(uint8_t* buffer, size_t size, uint32_t timeout) {
    size_t received = xStreamBufferReceive(furi_hal_vcp.rx_stream, buffer, size, timeout);

    if(furi_hal_vcp.rx_stream_full &&
       xStreamBufferSpacesAvailable(furi_hal_vcp.rx_stream) >= CONFIG_USB_CDC_RX_BUFSIZE) {
        furi_hal_vcp.rx_stream_full = false;
        ESP_LOGW(TAG, "Stream freed");
    }

    return received;
}

void usb_cdc_tx_char(uint8_t c, bool flush) {
    // TinyUSB implements buffering, no buffering is required here
    tinyusb_cdcacm_write_queue(TINYUSB_USBDEV_0, &c, 1);

    if(flush) {
        // SOME GDB MAGIC
        // We need to send an empty packet for some hosts to accept this as a complete transfer.
        uint8_t zero_byte = 0;
        tinyusb_cdcacm_write_queue(TINYUSB_USBDEV_0, &zero_byte, 1);

        // TODO: timeout size
        ESP_ERROR_CHECK_WITHOUT_ABORT(tinyusb_cdcacm_write_flush(TINYUSB_USBDEV_0, 1000));
    }
}

void usb_cdc_rx_callback(int itf, cdcacm_event_t* event) {
    uint16_t max_len = xStreamBufferSpacesAvailable(furi_hal_vcp.rx_stream);

    if(max_len > 0) {
        if(max_len > CONFIG_USB_CDC_RX_BUFSIZE) max_len = CONFIG_USB_CDC_RX_BUFSIZE;
        size_t rx_size = 0;
        esp_err_t err = tinyusb_cdcacm_read(itf, buffer_rx, max_len, &rx_size);

        if(err == ESP_OK) {
            if(rx_size > 0) {
                BaseType_t xHigherPriorityTaskWoken = pdFALSE;
                size_t ret =
                    xStreamBufferSend(furi_hal_vcp.rx_stream, buffer_rx, rx_size, portMAX_DELAY);
                ESP_ERROR_CHECK(ret != rx_size);
                // buffer_rx[rx_size] = '\0';
                // ESP_LOGI(TAG, "rx< %s", buffer_rx);
            }
        } else {
            ESP_LOGE(TAG, "Read error");
        }

    } else {
        furi_hal_vcp.rx_stream_full = true;
        ESP_LOGE(TAG, "Stream is full");
    };
}

void usb_cdc_line_state_changed_callback(int itf, cdcacm_event_t* event) {
    furi_hal_vcp.dtr = event->line_state_changed_data.dtr;
    furi_hal_vcp.rts = event->line_state_changed_data.rts;

    ESP_LOGI(TAG, "Line state changed! dtr:%d, rst:%d", furi_hal_vcp.dtr, furi_hal_vcp.rts);
}

void usb_cdc_line_coding_changed_callback(int itf, cdcacm_event_t* event) {
    uint32_t bit_rate = event->line_coding_changed_data.p_line_coding->bit_rate;
    uint8_t stop_bits = event->line_coding_changed_data.p_line_coding->stop_bits;
    uint8_t parity = event->line_coding_changed_data.p_line_coding->parity;
    uint8_t data_bits = event->line_coding_changed_data.p_line_coding->data_bits;

    ESP_LOGI(
        TAG,
        "Line coding changed! bit_rate:%d, stop_bits:%d, parity:%d, data_bits:%d",
        bit_rate,
        stop_bits,
        parity,
        data_bits);
}

//--------------------------------------------------------------------+
// Device callbacks
//--------------------------------------------------------------------+
void tud_mount_cb(void) {
    ESP_LOGI(TAG, "Mount");
    furi_hal_vcp.connected = true;
}

void tud_umount_cb(void) {
    ESP_LOGI(TAG, "Unmount");
    furi_hal_vcp.connected = false;
}

void usb_cdc_init(void) {
    furi_hal_vcp.connected = false;
    furi_hal_vcp.rx_stream = xStreamBufferCreate((CONFIG_USB_CDC_RX_BUFSIZE * 16), 1);
    furi_hal_vcp.rx_stream_full = false;

    ESP_LOGI(TAG, "USB initialization");
    tinyusb_config_t tusb_cfg = {};
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));

    tinyusb_config_cdcacm_t amc_cfg = {
        .usb_dev = TINYUSB_USBDEV_0,
        .cdc_port = TINYUSB_CDC_ACM_0,
        .rx_unread_buf_sz = 64,
        .callback_rx = &usb_cdc_rx_callback,
        .callback_rx_wanted_char = NULL,
        .callback_line_state_changed = &usb_cdc_line_state_changed_callback,
        .callback_line_coding_changed = &usb_cdc_line_coding_changed_callback};

    ESP_ERROR_CHECK(tusb_cdc_acm_init(&amc_cfg));

    ESP_LOGI(TAG, "USB initialization DONE");
}
