/*
  be_modem_lib.c - Berry module for esp_modem PPPoS over USB
  
  Copyright (C) 2025  Jonathan E. Peace

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef LOG_LOCAL_LEVEL
#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
#endif

#include "be_constobj.h"
#include "be_mapping.h"
#include "be_vm.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Required for the new iot_usbh_modem component
#include "usbh_modem_board.h"
// We might still need esp_netif for checking IP, but let's see
// #include "esp_netif.h" 

// Arduino.h might not be needed if power/reset is handled by iot_usbh_modem Kconfigs
// #include "Arduino.h"

// Define CONFIG_USBH_TASK_BASE_PRIORITY if not already defined
#ifndef CONFIG_USBH_TASK_BASE_PRIORITY
#define CONFIG_USBH_TASK_BASE_PRIORITY 5
#endif

static const char* TAG = "MDM_BE";

// Global state for the new driver
static bool g_modem_board_initialized = false;
static esp_event_handler_t g_user_event_handler = NULL;
static void *g_user_event_handler_arg = NULL;


// Modem type - used to customize AT commands for different modems (may still be needed for GNSS/SMS)
typedef enum {
    MODEM_TYPE_GENERIC = 0,
    MODEM_TYPE_SIM7600,
    MODEM_TYPE_SIM800,
    MODEM_TYPE_BG96
} modem_type_t;
static modem_type_t g_modem_type = MODEM_TYPE_SIM7600; // Default to SIM7600


// Forward declarations for Berry VM functions
static int w_modem_init(bvm *vm);           // Was w_modem_setup_environment
static int w_modem_deinit(bvm *vm);         // New
static int w_modem_connect(bvm *vm);
static int w_modem_disconnect(bvm *vm);
static int w_modem_status(bvm *vm);
static int w_modem_get_gnss_info(bvm *vm);
static int w_modem_set_type(bvm *vm);
static int w_modem_get_type(bvm *vm);
static int w_modem_get_msisdn(bvm *vm);
static int w_modem_send_sms(bvm *vm);
// Callbacks on_connect/on_disconnect will need to use the event system of usbh_modem_board
// static int w_modem_on_event(bvm *vm); // New, to register a generic event handler

// Helper for GNSS/SMS if we can send AT commands (currently placeholder)
// static char *gnss_response = NULL;
// static bool gnss_info_received = false;

// --- Removed GPIO and old USB init related static functions ---

// Event handler to be passed to modem_board_init
static void modem_event_proxy_handler(void *handler_arg, esp_event_base_t base, int32_t id, void *event_data) {
    ESP_LOGI(TAG, "Modem Board Event: Base=%s, ID=%d", base, (int)id);
    
    if (base == MODEM_BOARD_EVENT) {
        if (id == MODEM_EVENT_SIMCARD_DISCONN) {
            ESP_LOGW(TAG, "Modem Board Event: SIM Card disconnected");
        } else if (id == MODEM_EVENT_SIMCARD_CONN) {
            ESP_LOGI(TAG, "Modem Board Event: SIM Card Connected");
        } else if (id == MODEM_EVENT_DTE_DISCONN) {
            ESP_LOGW(TAG, "Modem Board Event: USB disconnected");
        } else if (id == MODEM_EVENT_DTE_CONN) {
            ESP_LOGI(TAG, "Modem Board Event: USB connected");
        } else if (id == MODEM_EVENT_DTE_RESTART) {
            ESP_LOGW(TAG, "Modem Board Event: Hardware restart");
        } else if (id == MODEM_EVENT_DTE_RESTART_DONE) {
            ESP_LOGI(TAG, "Modem Board Event: Hardware restart done");
        } else if (id == MODEM_EVENT_NET_CONN) {
            ESP_LOGI(TAG, "Modem Board Event: Network connected");
        } else if (id == MODEM_EVENT_NET_DISCONN) {
            ESP_LOGW(TAG, "Modem Board Event: Network disconnected");
        } else if (id == MODEM_EVENT_WIFI_STA_CONN) {
            ESP_LOGI(TAG, "Modem Board Event: Station connected");
        } else if (id == MODEM_EVENT_WIFI_STA_DISCONN) {
            ESP_LOGW(TAG, "Modem Board Event: All stations disconnected");
        } else {
            ESP_LOGI(TAG, "Modem Board Event: Unknown event ID=%d", (int)id);
        }
    }
    
    // Future enhancement: call Berry callback if registered
    if (g_user_event_handler) {
        // To be implemented with proper Berry VM stack management
    }
}


// Initialize the modem using iot_usbh_modem
static int w_modem_init(bvm *vm) { // Replaces w_modem_setup_environment
    if (g_modem_board_initialized) {
        ESP_LOGW(TAG, "Modem board already initialized.");
        be_pushbool(vm, true);
        be_return(vm);
    }

    ESP_LOGI(TAG, "Initializing modem board...");
    modem_config_t modem_config = MODEM_DEFAULT_CONFIG();
    
    // Don't start PPP automatically, let Berry script control it
    modem_config.flags |= MODEM_FLAGS_INIT_NOT_ENTER_PPP;
    // Don't block init waiting for IP
    modem_config.flags |= MODEM_FLAGS_INIT_NOT_BLOCK;

    // Register our event handler to process modem events
    modem_config.handler = modem_event_proxy_handler;
    modem_config.handler_arg = NULL; // No context needed for now

    esp_err_t err = modem_board_init(&modem_config);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Modem board initialized successfully.");
        g_modem_board_initialized = true;
        be_pushbool(vm, true);
    } else {
        ESP_LOGE(TAG, "Failed to initialize modem board: %s", esp_err_to_name(err));
        g_modem_board_initialized = false;
        be_pushbool(vm, false);
    }
    be_return(vm);
}

// Deinitialize the modem
static int w_modem_deinit(bvm *vm) {
    if (!g_modem_board_initialized) {
        ESP_LOGW(TAG, "Modem board not initialized.");
        be_pushbool(vm, true); // Or false, as it wasn't init
        be_return(vm);
    }

    ESP_LOGI(TAG, "Deinitializing modem board...");
    esp_err_t err = modem_board_deinit();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Modem board deinitialized successfully.");
        g_modem_board_initialized = false;
        be_pushbool(vm, true);
    } else {
        ESP_LOGE(TAG, "Failed to deinitialize modem board: %s", esp_err_to_name(err));
        be_pushbool(vm, false); // Still consider it de-initialized for our flag
        g_modem_board_initialized = false;
    }
    be_return(vm);
}

// Connect to the cellular network
static int w_modem_connect(bvm *vm) {
    if (!g_modem_board_initialized) {
        be_raise(vm, "runtime_error", "Modem not initialized. Call modem.init() first.");
        be_return(vm);
    }

    const char *apn = NULL;
    if (be_top(vm) >= 1 && be_isstring(vm, 1)) {
        apn = be_tostring(vm, 1);
        ESP_LOGI(TAG, "Setting APN to: %s", apn);
        esp_err_t apn_err = modem_board_set_apn(apn, true); // true to force re-dial if APN changes
        if (apn_err != ESP_OK && apn_err != ESP_ERR_INVALID_STATE /* APN already same */) {
            ESP_LOGE(TAG, "Failed to set APN: %s", esp_err_to_name(apn_err));
            be_pushbool(vm, false);
            be_return(vm);
        }
    }
    
    ESP_LOGI(TAG, "Attempting to start PPP connection...");
    // Timeout for ppp_start, e.g., 60 seconds. 
    // The function itself might be non-blocking if MODEM_FLAGS_INIT_NOT_BLOCK was set.
    // We need a way to confirm connection.
    esp_err_t err = modem_board_ppp_start(60000); 
    
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "PPP start command issued. Waiting for connection...");
        // modem_board_ppp_start might be non-blocking. 
        // For a synchronous connect, we'd need to wait for MODEM_EVENT_NET_CONN
        // or poll a status function. For now, assume it tries to connect.
        // A true "connected" state needs IP_EVENT_PPP_GOT_IP.
        be_pushbool(vm, true); // Indicates command was accepted
    } else {
        ESP_LOGE(TAG, "Failed to start PPP connection: %s", esp_err_to_name(err));
        be_pushbool(vm, false);
    }
    be_return(vm);
}

// Disconnect from the cellular network
static int w_modem_disconnect(bvm *vm) {
    if (!g_modem_board_initialized) {
        be_raise(vm, "runtime_error", "Modem not initialized.");
        be_return(vm);
    }
    
    ESP_LOGI(TAG, "Attempting to stop PPP connection...");
    esp_err_t err = modem_board_ppp_stop(10000); // 10s timeout for ppp_stop
    
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "PPP stop command issued.");
        be_pushbool(vm, true);
    } else {
        ESP_LOGE(TAG, "Failed to stop PPP connection: %s", esp_err_to_name(err));
        be_pushbool(vm, false);
    }
    be_return(vm);
}

// Get modem status
static int w_modem_status(bvm *vm) {
    if (!g_modem_board_initialized) {
        be_raise(vm, "runtime_error", "Modem not initialized.");
        be_return(vm);
    }
    
    be_newmap(vm);
    esp_err_t err;

    // Connected status (placeholder - needs proper check)
    // This requires checking if the PPP netif has an IP.
    // The iot_usbh_modem internal s_modem_evt_hdl has PPP_NET_CONNECT_BIT.
    // We need a helper like `bool modem_board_is_connected()`
    bool is_connected_placeholder = false; 
    // Example: esp_netif_t *ppp_netif = esp_netif_get_handle_from_ifkey("PPP_DEF");
    // if (ppp_netif) { esp_netif_ip_info_t ip_info; if (esp_netif_get_ip_info(ppp_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) is_connected_placeholder = true; }
    be_pushbool(vm, is_connected_placeholder); // Placeholder!
    be_setmember(vm, -2, "connected");

    // Signal Quality
    int rssi = -1, ber = -1;
    err = modem_board_get_signal_quality(&rssi, &ber);
    if (err == ESP_OK) {
        be_pushint(vm, rssi);
        be_setmember(vm, -2, "rssi");
        be_pushint(vm, ber);
        be_setmember(vm, -2, "ber");
    } else {
        ESP_LOGW(TAG, "Failed to get signal quality: %s", esp_err_to_name(err));
        be_pushint(vm, -1); be_setmember(vm, -2, "rssi");
        be_pushint(vm, -1); be_setmember(vm, -2, "ber");
    }

    // SIM Card State
    int sim_ready = 0; // 0 for not ready/error, 1 for ready
    err = modem_board_get_sim_cart_state(&sim_ready); // Note: API uses int*, not bool*
    if (err == ESP_OK) {
        be_pushbool(vm, (bool)sim_ready);
        be_setmember(vm, -2, "sim_ready");
    } else {
        ESP_LOGW(TAG, "Failed to get SIM card state: %s", esp_err_to_name(err));
        be_pushbool(vm, false); be_setmember(vm, -2, "sim_ready");
    }
    
    // Operator Name
    char operator_name[64] = {0};
    err = modem_board_get_operator_state(operator_name, sizeof(operator_name));
    if (err == ESP_OK) {
        be_pushstring(vm, operator_name);
        be_setmember(vm, -2, "operator");
    } else {
        ESP_LOGW(TAG, "Failed to get operator name: %s", esp_err_to_name(err));
        be_pushstring(vm, ""); be_setmember(vm, -2, "operator");
    }
    
    // IMEI / IMSI - Not directly available in usbh_modem_board.h API
    // Would require sending AT commands. Placeholder for now.
    be_pushstring(vm, "N/A"); be_setmember(vm, -2, "imei");
    be_pushstring(vm, "N/A"); be_setmember(vm, -2, "imsi");
    
    // IP Address - Placeholder
    // Would need to get it from the PPP netif
    be_pushstring(vm, "0.0.0.0"); be_setmember(vm, -2, "ip");
    
    // Remove return value of be_pop, not used
    be_pop(vm, be_top(vm) - 1 -1); // Leave the map object on top
    be_return(vm);
}

// Set the modem module type (runtime configuration)
static int w_modem_set_type(bvm *vm) {
    // This function might be less relevant if iot_usbh_modem handles type specifics internally
    // or if AT commands for GNSS/SMS are routed through a generic command sender.
    // For now, just store it locally if needed for custom AT command construction.
    if (!be_isstring(vm, 1)) {
        be_raise(vm, "type_error", "Expected string modem type");
        be_return(vm);
    }
    const char *module_type_str = be_tostring(vm, 1);
    ESP_LOGI(TAG, "Setting modem type to %s (for Berry lib internal use)", module_type_str);
    
    if (strcasecmp(module_type_str, "SIM7600") == 0) g_modem_type = MODEM_TYPE_SIM7600;
    else if (strcasecmp(module_type_str, "SIM800") == 0) g_modem_type = MODEM_TYPE_SIM800;
    else if (strcasecmp(module_type_str, "BG96") == 0) g_modem_type = MODEM_TYPE_BG96;
    else g_modem_type = MODEM_TYPE_GENERIC;
    
    be_pushbool(vm, true);
    be_return(vm);
}

// Get modem type as string
static int w_modem_get_type(bvm *vm) {
    const char* type_str = "UNKNOWN";
    switch (g_modem_type) {
        case MODEM_TYPE_SIM7600: type_str = "SIM7600"; break;
        case MODEM_TYPE_SIM800:  type_str = "SIM800"; break;
        case MODEM_TYPE_BG96:    type_str = "BG96"; break;
        case MODEM_TYPE_GENERIC: type_str = "GENERIC"; break;
    }
    be_pushstring(vm, type_str);
    be_return(vm);
}


// --- Placeholder / To Be Implemented with AT command sending via new driver ---
static int w_modem_get_gnss_info(bvm *vm) {
    if (!g_modem_board_initialized) {
        be_raise(vm, "runtime_error", "Modem not initialized.");
        be_return(vm);
    }
    ESP_LOGW(TAG, "GNSS info not yet implemented with new driver.");
    be_raise(vm, "runtime_error", "GNSS not implemented");
    be_return(vm);
}

static int w_modem_send_sms(bvm *vm) {
     if (!g_modem_board_initialized) {
        be_raise(vm, "runtime_error", "Modem not initialized.");
        be_return(vm);
    }
    ESP_LOGW(TAG, "SMS sending not yet implemented with new driver.");
    be_raise(vm, "runtime_error", "SMS not implemented");
    be_return(vm);
}

static int w_modem_get_msisdn(bvm *vm) {
    if (!g_modem_board_initialized) {
        be_raise(vm, "runtime_error", "Modem not initialized.");
        be_return(vm);
    }
    ESP_LOGW(TAG, "MSISDN retrieval not yet implemented with new driver.");
    be_pushstring(vm, ""); // Return empty string
    be_return(vm);
}


// --- End Placeholder sections ---


/* @const_object_info_begin
module modem (scope: global, strings: weak) {
    init, func(w_modem_init)
    deinit, func(w_modem_deinit)
    connect, func(w_modem_connect)
    disconnect, func(w_modem_disconnect)
    status, func(w_modem_status)
    
    // Placeholders, to be re-implemented if AT command sending is available
    gnss, func(w_modem_get_gnss_info)
    set_type, func(w_modem_set_type) // May be useful for constructing AT commands
    get_type, func(w_modem_get_type) // May be useful for constructing AT commands
    msisdn, func(w_modem_get_msisdn)
    send_sms, func(w_modem_send_sms)

    // Removed: init_uart, init_usb, set_power_pin, set_reset_pin, power_on, power_off, reset
    // Callbacks on_connect/on_disconnect might be replaced by a generic on_event
}
@const_object_info_end */

#include "be_fixed_modem.h" // Generated by Tasmota build process
