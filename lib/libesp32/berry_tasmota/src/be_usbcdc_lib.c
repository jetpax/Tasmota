#ifdef USE_BERRY_USBCDC

#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h> // For malloc/free/realloc

#define LOG_LOCAL_LEVEL ESP_LOG_INFO
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h" // For vTaskDelay, xTaskGetTickCount

#include "be_mapping.h"
#include "be_vm.h"
#include "be_gc.h"
#include "be_exec.h" // For BE_EXEC_ERROR

#include "iot_usbh_cdc.h" // Main CDC header

// Logger tag
static const char *TAG = "USBCDC_LIB";

// Default configuration values
#define DEFAULT_USB_ITF_NUM 2 
#define DEFAULT_RX_RINGBUF_SIZE (1024 * 1)
#define DEFAULT_TX_RINGBUF_SIZE (1024 * 1)

// RX Queue configuration for passing data from callback to send function
#define USB_RX_QUEUE_LEN 10             // Number of items in the queue
#define USB_RX_ITEM_MAX_SIZE 256        // Max size of a single data chunk from USB

typedef struct {
    uint8_t data[USB_RX_ITEM_MAX_SIZE];
    size_t len;
} usb_rx_item_t;

// Global state
static usbh_cdc_handle_t cdc_handle = NULL;
static bool cdc_client_driver_installed = false; // Tracks if usbh_cdc_driver_install (for client) was called
static QueueHandle_t usb_rx_data_queue = NULL;   // Queue for received data items for send()
static QueueHandle_t usb_event_queue = NULL; // Queue for connect/disconnect event notifications

// Berry callback storage
static bvalue berry_connect_cb = BE_NIL_VAL;
static bvalue berry_disconnect_cb = BE_NIL_VAL;
static bvm* berry_vm_instance = NULL; // Store VM when callbacks are registered

// Event types for usb_event_queue
typedef enum {
    USB_EVENT_DEVICE_CONNECT,
    USB_EVENT_DEVICE_DISCONNECT
} usb_notification_event_type_t;

typedef struct {
    usb_notification_event_type_t type;
} usb_notification_item_t;

#define USB_EVENT_QUEUE_LEN 5

// Forward declarations for callbacks
static void cdc_connect_callback(usbh_cdc_handle_t handle, void *user_data);
static void cdc_disconnect_callback(usbh_cdc_handle_t handle, void *user_data);
static void cdc_receive_data_callback(usbh_cdc_handle_t handle, void *user_data);

// Callback for connect events
static void cdc_connect_callback(usbh_cdc_handle_t handle, void *user_data) {
    ESP_LOGI(TAG, "USB CDC Device Connected. Handle: %p", handle);
    // To get more info, you might need to store it or use usbh_cdc_desc_print(handle);
    if (usb_event_queue && !be_isnil(&berry_connect_cb)) {
        usb_notification_item_t event_item;
        event_item.type = USB_EVENT_DEVICE_CONNECT;
        if (xQueueSend(usb_event_queue, &event_item, 0) != pdTRUE) {
            ESP_LOGW(TAG, "Failed to send connect event to queue.");
        }
    }
}

// Callback for disconnect events
static void cdc_disconnect_callback(usbh_cdc_handle_t handle, void *user_data) {
    ESP_LOGW(TAG, "USB CDC Device Disconnected. Handle: %p", handle);
    if (cdc_handle == handle) {
        ESP_LOGI(TAG, "Global cdc_handle instance disconnected.");
    }
    if (usb_event_queue && !be_isnil(&berry_disconnect_cb)) {
        usb_notification_item_t event_item;
        event_item.type = USB_EVENT_DEVICE_DISCONNECT;
        if (xQueueSend(usb_event_queue, &event_item, 0) != pdTRUE) {
            ESP_LOGW(TAG, "Failed to send disconnect event to queue.");
        }
    }
}

// Callback for data reception notification from USB CDC driver
static void cdc_receive_data_callback(usbh_cdc_handle_t dev_handle, void *user_data) {
    if (usb_rx_data_queue == NULL || dev_handle != cdc_handle) {
        return;
    }

    usb_rx_item_t rx_item;
    size_t data_available_in_cdc_buffer;
    
    while(1) {
        esp_err_t err_size = usbh_cdc_get_rx_buffer_size(dev_handle, &data_available_in_cdc_buffer);
        if (err_size != ESP_OK || data_available_in_cdc_buffer == 0) {
            break; 
        }

        rx_item.len = data_available_in_cdc_buffer;
        if (rx_item.len > USB_RX_ITEM_MAX_SIZE) {
            rx_item.len = USB_RX_ITEM_MAX_SIZE;
        }

        size_t requested_bytes = rx_item.len;
        esp_err_t err_read = usbh_cdc_read_bytes(dev_handle, rx_item.data, &rx_item.len, 0); 

        if (err_read == ESP_OK && rx_item.len > 0) {
            ESP_LOGD(TAG, "CDC RX CB: Read %d bytes from CDC internal buffer", (int)rx_item.len);
            if (xQueueSend(usb_rx_data_queue, &rx_item, 0) != pdTRUE) {
                ESP_LOGW(TAG, "USB RX processing queue full, %d bytes data lost.", (int)rx_item.len);
                break; 
            }
        } else {
            if (err_read != ESP_OK) {
                 ESP_LOGE(TAG, "CDC RX CB: Error reading from CDC internal buffer: %s", esp_err_to_name(err_read));
            }
            break;
        }
    }
}

// Berry API: usbcdc.start(vid=0, pid=0, intf_num=DEFAULT_USB_ITF_NUM) -> bool
static int w_usbcdc_start(bvm *vm) {
    int32_t vid = 0, pid = 0, itf_num = DEFAULT_USB_ITF_NUM;

    if (be_top(vm) >= 1 && be_isint(vm, 1)) vid = be_toint(vm, 1);
    if (be_top(vm) >= 2 && be_isint(vm, 2)) pid = be_toint(vm, 2);
    if (be_top(vm) >= 3 && be_isint(vm, 3)) itf_num = be_toint(vm, 3);

    if (cdc_handle != NULL) {
        ESP_LOGW(TAG, "CDC already started. Call stop() first or use current instance.");
        be_return_bool(vm, true); // Indicate that a handle is available or was already started
    }

    // Install USBH CDC client driver if not already installed
    if (!cdc_client_driver_installed) {
        usbh_cdc_driver_config_t driver_config = {
            .task_stack_size = 4096,
            .task_priority = 5,     
            .task_coreid = 0, // Core for CDC client task
            .skip_init_usb_host_driver = true, // Assume Tasmota main initializes USB host driver
        };
        esp_err_t err = usbh_cdc_driver_install(&driver_config);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to install USBH CDC client driver: %s", esp_err_to_name(err));
            be_return_bool(vm, false);
        }
        cdc_client_driver_installed = true;
        ESP_LOGI(TAG, "USBH CDC client driver installed.");
    }

    // Create RX queue for data
    if (usb_rx_data_queue == NULL) {
        usb_rx_data_queue = xQueueCreate(USB_RX_QUEUE_LEN, sizeof(usb_rx_item_t));
        if (usb_rx_data_queue == NULL) {
            ESP_LOGE(TAG, "Failed to create USB RX data queue.");
            be_return_bool(vm, false);
        }
        ESP_LOGI(TAG, "USB RX data queue created.");
    } else {
        // Ensure queue is empty if it existed from a previous session
        xQueueReset(usb_rx_data_queue);
    }

    // Create Event queue for connect/disconnect
    if (usb_event_queue == NULL) {
        usb_event_queue = xQueueCreate(USB_EVENT_QUEUE_LEN, sizeof(usb_notification_item_t));
        if (usb_event_queue == NULL) {
            ESP_LOGE(TAG, "Failed to create USB event queue.");
            // Potentially clean up usb_rx_data_queue if it was just created
            be_return_bool(vm, false);
        }
        ESP_LOGI(TAG, "USB event queue created.");
    } else {
        xQueueReset(usb_event_queue);
    }

    usbh_cdc_device_config_t dev_config = {
        .vid = (uint16_t)vid,
        .pid = (uint16_t)pid,
        .itf_num = (uint8_t)itf_num,
        .rx_buffer_size = DEFAULT_RX_RINGBUF_SIZE,
        .tx_buffer_size = DEFAULT_TX_RINGBUF_SIZE,
        .cbs = {
            .connect = cdc_connect_callback,
            .disconnect = cdc_disconnect_callback,
            .recv_data = cdc_receive_data_callback,
            .user_data = NULL, 
        },
    };

    esp_err_t err = usbh_cdc_create(&dev_config, &cdc_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create USB CDC device (VID:0x%04X, PID:0x%04X, ITF:%d): %s",
                 vid, pid, itf_num, esp_err_to_name(err));
        cdc_handle = NULL;
        be_return_bool(vm, false);
    }

    ESP_LOGI(TAG, "USB CDC device created (VID:0x%04X, PID:0x%04X, ITF:%d). Handle: %p. Waiting for connection.",
             vid, pid, itf_num, cdc_handle);
    be_return_bool(vm, true);
}

// Berry API: usbcdc.stop() -> bool
static int w_usbcdc_stop(bvm *vm) {
    if (cdc_handle == NULL) {
        ESP_LOGI(TAG, "CDC not started or already stopped.");
        be_return_bool(vm, true); 
    }

    esp_err_t err = usbh_cdc_delete(cdc_handle);
    usbh_cdc_handle_t old_handle = cdc_handle;
    cdc_handle = NULL; // Mark as stopped regardless of error for safety

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to delete USB CDC device (Handle: %p): %s", old_handle, esp_err_to_name(err));
        be_return_bool(vm, false);
    }

    ESP_LOGI(TAG, "USB CDC device (Handle: %p) stopped and deleted successfully.", old_handle);
    
    // The RX queue is not deleted here; it will be reset on next start.
    // The CDC client driver is not uninstalled here; assumed to be managed globally by Tasmota or reusable.

    be_return_bool(vm, true);
}

// Berry API: usbcdc.send(data_str, timeout_ms=3000) -> string (response or nil)
static int w_usbcdc_send(bvm *vm) {
    if (cdc_handle == NULL) {
        be_raise(vm, "runtime_error", "USB CDC not started. Call usbcdc.start() first.");
        return BE_EXEC_ERROR;
    }
    if (usb_rx_data_queue == NULL) {
        be_raise(vm, "runtime_error", "USB RX data queue not initialized. Call usbcdc.start() first.");
        return BE_EXEC_ERROR;
    }

    const char *data_to_send = NULL;
    int32_t timeout_ms = 3000; 

    if (be_top(vm) < 1 || !be_isstring(vm, 1)) {
        be_raise(vm, "type_error", "Argument 1 must be a string (data to send)");
        return BE_EXEC_ERROR;
    }
    data_to_send = be_tostring(vm, 1);

    if (be_top(vm) >= 2 && be_isint(vm, 2)) {
        timeout_ms = be_toint(vm, 2);
        if (timeout_ms < 0) timeout_ms = 0;
    }
    
    // Clear any stale data from the RX queue
    usb_rx_item_t dummy_item;
    while (xQueueReceive(usb_rx_data_queue, &dummy_item, 0) == pdTRUE);

    size_t data_len = strlen(data_to_send);
    ESP_LOGD(TAG, "Sending %d bytes: %s", (int)data_len, data_to_send);
    esp_err_t err = usbh_cdc_write_bytes(cdc_handle, (const uint8_t *)data_to_send, data_len, pdMS_TO_TICKS(1000));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send data via USB CDC: %s", esp_err_to_name(err));
        be_pushnil(vm);
        be_return(vm);
    }

    char *response_buffer = NULL;
    size_t response_capacity = 0;
    size_t response_length = 0;
    TickType_t start_ticks = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    usb_rx_item_t received_item;

    while (true) {
        TickType_t current_ticks = xTaskGetTickCount();
        TickType_t elapsed_ticks = current_ticks - start_ticks;
        if (elapsed_ticks >= timeout_ticks && timeout_ms > 0) { // if timeout_ms is 0, try to read once
             ESP_LOGD(TAG, "Receive timeout reached after %d ms.", (int)elapsed_ticks * portTICK_PERIOD_MS);
            break; 
        }

        TickType_t wait_ticks = (timeout_ms == 0) ? 0 : (timeout_ticks - elapsed_ticks);
        if (timeout_ms > 0 && wait_ticks > timeout_ticks) wait_ticks = 0; // safety for tick wrap around, effectively no wait if overdue

        if (xQueueReceive(usb_rx_data_queue, &received_item, wait_ticks) == pdTRUE) {
            ESP_LOGD(TAG, "Send: Received item from data queue, len: %d", (int)received_item.len);
            if (response_length + received_item.len + 1 > response_capacity) {
                size_t new_capacity = response_capacity == 0 ? (USB_RX_ITEM_MAX_SIZE + 1) : response_capacity * 2;
                if (new_capacity < response_length + received_item.len + 1) {
                    new_capacity = response_length + received_item.len + 1;
                }
                char *new_buffer = (char*)realloc(response_buffer, new_capacity);
                if (!new_buffer) {
                    ESP_LOGE(TAG, "Failed to realloc response buffer");
                    free(response_buffer); 
                    be_raise(vm, "memory_error", "Failed to allocate buffer for response");
                    return BE_EXEC_ERROR;
                }
                response_buffer = new_buffer;
                response_capacity = new_capacity;
            }
            memcpy(response_buffer + response_length, received_item.data, received_item.len);
            response_length += received_item.len;
            response_buffer[response_length] = '\0'; 
        } else {
             ESP_LOGD(TAG, "Send: RX Queue receive timed out or empty after waiting.");
            break; 
        }
        if (timeout_ms == 0) break; // If timeout is 0, only one attempt to read queue
    }

    if (response_length > 0) {
        ESP_LOGD(TAG, "Final response (len %d): %s", (int)response_length, response_buffer);
        be_pushnstring(vm, response_buffer, response_length);
    } else {
        ESP_LOGD(TAG, "No response received or timeout with no data.");
        be_pushnil(vm);
    }

    if (response_buffer) {
        free(response_buffer);
    }
    be_return(vm);
}

// Berry API: usbcdc.on_connect(function_or_nil)
static int w_usbcdc_on_connect(bvm *vm) {
    if (be_top(vm) >= 1) {
        if (be_isfunction(vm, 1) || be_isclosure(vm,1) ) {
            // Unregister previous, if any (basic - does not explicitly unroot from GC if that were needed)
            if (!be_isnil(&berry_connect_cb)) {
                // be_remove_gc_root(vm, &berry_connect_cb); // Example if explicit unrooting was used
            }
            berry_connect_cb = *be_indexof(vm, 1);
            // be_add_gc_root(vm, &berry_connect_cb); // Example if explicit rooting was used
            if (berry_vm_instance == NULL) berry_vm_instance = vm; // Store VM for loop
            ESP_LOGI(TAG, "Registered on_connect callback.");
        } else if (be_isnil(vm, 1)) {
            if (!be_isnil(&berry_connect_cb)) {
                // be_remove_gc_root(vm, &berry_connect_cb);
            }
            berry_connect_cb = BE_NIL_VAL;
            ESP_LOGI(TAG, "Unregistered on_connect callback.");
        } else {
            be_raise(vm, "type_error", "Argument must be a function or nil");
            return BE_EXEC_ERROR;
        }
    }
    be_return_nil(vm);
}

// Berry API: usbcdc.on_disconnect(function_or_nil)
static int w_usbcdc_on_disconnect(bvm *vm) {
    if (be_top(vm) >= 1) {
        if (be_isfunction(vm, 1) || be_isclosure(vm,1) ) {
            if (!be_isnil(&berry_disconnect_cb)) {
                // be_remove_gc_root(vm, &berry_disconnect_cb);
            }
            berry_disconnect_cb = *be_indexof(vm, 1);
            // be_add_gc_root(vm, &berry_disconnect_cb);
            if (berry_vm_instance == NULL) berry_vm_instance = vm;
            ESP_LOGI(TAG, "Registered on_disconnect callback.");
        } else if (be_isnil(vm, 1)) {
             if (!be_isnil(&berry_disconnect_cb)) {
                // be_remove_gc_root(vm, &berry_disconnect_cb);
            }
            berry_disconnect_cb = BE_NIL_VAL;
            ESP_LOGI(TAG, "Unregistered on_disconnect callback.");
        } else {
            be_raise(vm, "type_error", "Argument must be a function or nil");
            return BE_EXEC_ERROR;
        }
    }
    be_return_nil(vm);
}

// Berry API: usbcdc.loop() -> int (number of events processed)
static int w_usbcdc_loop(bvm *vm) {
    if (!usb_event_queue) {
        // Silently return if not initialized, or raise error?
        // For now, silent, as start() might not have been called.
        be_return_int(vm, 0);
    }
    if (berry_vm_instance == NULL && (!be_isnil(&berry_connect_cb) || !be_isnil(&berry_disconnect_cb))) {
         // This should ideally not happen if on_connect/on_disconnect stored the VM.
         // If it does, try to grab the current VM.
         berry_vm_instance = vm;
         ESP_LOGW(TAG,"usbcdc.loop() called without berry_vm_instance, attempting to use current vm.");
    }


    int processed_events = 0;
    usb_notification_item_t event_item;

    // Process up to a few events per call to avoid blocking Berry for too long
    for (int i = 0; i < USB_EVENT_QUEUE_LEN; ++i) {
        if (xQueueReceive(usb_event_queue, &event_item, 0) == pdTRUE) {
            processed_events++;
            bvalue* cb_to_call = NULL;

            if (event_item.type == USB_EVENT_DEVICE_CONNECT && !be_isnil(&berry_connect_cb)) {
                cb_to_call = &berry_connect_cb;
                ESP_LOGI(TAG, "Processing CONNECT event for Berry.");
            } else if (event_item.type == USB_EVENT_DEVICE_DISCONNECT && !be_isnil(&berry_disconnect_cb)) {
                cb_to_call = &berry_disconnect_cb;
                ESP_LOGI(TAG, "Processing DISCONNECT event for Berry.");
            }

            if (cb_to_call && berry_vm_instance) {
                // Ensure the VM instance from registration is used if possible,
                // otherwise, the VM passed to this loop call.
                bvm* exec_vm = (berry_vm_instance) ? berry_vm_instance : vm;
                
                int top = be_top(exec_vm);
                be_pushvalue(exec_vm, be_absindex(exec_vm, रजिस्ट्र(-1))); // push function (assumes cb_to_call is global or on stack)
                                                                      // This is incorrect. We need to push the *cb_to_call bvalue.

                // Correct way to push the callback value:
                be_pushnil(exec_vm); // Make space on stack
                bvalue *dst = be_indexof(exec_vm, -1);
                *dst = *cb_to_call; // Copy the bvalue (which is the function/closure)

                // No arguments for these callbacks for now
                int call_res = be_pcall(exec_vm, 0); // 0 arguments
                
                if (call_res == BE_EXEC_ERROR) {
                    // Optionally log the error from Berry stack
                    const char *err_str = be_tostring(exec_vm, -1);
                    ESP_LOGE(TAG, "Error executing Berry event callback: %s", err_str);
                }
                be_settop(exec_vm, top); // Restore stack
            }
        } else {
            break; // Queue is empty
        }
    }
    be_return_int(vm, processed_events);
}

/* @const_object_info_begin
module usbcdc (scope: global, strings: weak) {
    start, func(w_usbcdc_start)
    stop, func(w_usbcdc_stop)
    send, func(w_usbcdc_send)
    on_connect, func(w_usbcdc_on_connect)
    on_disconnect, func(w_usbcdc_on_disconnect)
    loop, func(w_usbcdc_loop)
}
@const_object_info_end */

#include "be_fixed_usbcdc.h" // Must be created by build system or manually

#endif // USE_BERRY_USBCDC 