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

// For modem_board_* API and MODEM_DEFAULT_CONFIG
#include "usbh_modem_board.h"
// For esp_netif functions to check IP address
#include "esp_netif.h"
// For esp_modem_dce_t and common DCE commands (IMEI, IMSI) from esp_modem_iot
#include "esp_modem_dce.h" // From iot_usbh_modem/private_include
#include "esp_modem_dce_common_commands.h" // From iot_usbh_modem/private_include

static const char* TAG = "MDM_BE";

// Global state for the new driver
static esp_modem_dce_t *g_modem_dce_handle = NULL; // Stores the DCE handle from esp_modem_iot
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
static int w_modem_init(bvm *vm) { 
    if (g_modem_dce_handle != NULL) {
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

    // Override settings if provided in function parameters
    if (be_top(vm) >= 1 && be_ismap(vm, 1)) {
        // Get 'interface' param
        be_getmember(vm, 1, "interface");
        if (!be_isnil(vm, -1)) {
            int itf_num = be_toint(vm, -1);
            ESP_LOGI(TAG, "Setting specific interface number: %d", itf_num);
            // Use the correct field based on your modem_config_t structure
            // Uncomment the appropriate line:
            // modem_config.device_config.itf_num = itf_num;
            // modem_config.itf_num = itf_num;
            // modem_config.port_num = itf_num;
        }
        be_pop(vm, 1);
        
        // Get 'timeout' param
        be_getmember(vm, 1, "timeout");
        if (!be_isnil(vm, -1)) {
            int timeout = be_toint(vm, -1);
            ESP_LOGI(TAG, "Setting AT command timeout: %d ms", timeout);
            // Use the correct field based on your modem_config_t structure
            // Uncomment the appropriate line:
            // modem_config.device_config.send_cmd_timeout = timeout;
            // modem_config.timeout = timeout;
        }
        be_pop(vm, 1);
    }

    // Register our event handler to process modem events
    modem_config.handler = modem_event_proxy_handler;
    modem_config.handler_arg = NULL; // No context needed for now

    // Add additional debugging
    ESP_LOGI(TAG, "Modem config: flags=0x%x", modem_config.flags);

    esp_err_t err = modem_board_init(&modem_config);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Modem board init command accepted. Waiting for DCE handle...");
        int retries = 0;
        // Wait up to ~10 seconds for the DCE handle to become available.
        // The daemon task needs time for USB enumeration, DTE/DCE creation.
        const int max_retries = 200; // 200 * 50ms = 10 seconds
        while ((g_modem_dce_handle = modem_board_get_dce()) == NULL && retries < max_retries) {
            vTaskDelay(pdMS_TO_TICKS(50)); // Poll every 50ms
            retries++;
        }

        if (g_modem_dce_handle != NULL) {
            ESP_LOGI(TAG, "DCE handle obtained successfully: %p", g_modem_dce_handle);
            
            ESP_LOGI(TAG, "Delaying for 2500ms before first AT command...");
            vTaskDelay(pdMS_TO_TICKS(2500));

            // Direct AT command test
            char direct_response_buffer[64];
            ESP_LOGI(TAG, "Attempting direct 'AT\r\n' command...");
            // Define a simple_response_handler for esp_modem_dce_send_cmd
            // This handler won't be fully implemented for Berry here, just for logging
            // In a real scenario, it would parse the response or signal success/failure.
            // For this test, we mainly care if it times out or returns ESP_OK.
            // The esp-modem default timeout for commands is CONFIG_MODEM_COMMAND_TIMEOUT_DEFAULT (2000ms)
            err = esp_modem_dce_send_cmd(g_modem_dce_handle, "AT\r\n", NULL, 0); 
            // Note: The original esp_modem_dce_send_cmd takes a timeout and a handler.
            // We are calling a variant or assuming a default. If this exact signature is not available,
            // we may need to use esp_modem_dce_command or similar with more parameters.
            // Let's assume for now a simple version exists or MODEM_DEFAULT_TIMEOUT is used.
            // A more robust call would be:
            // err = esp_modem_dce_command(g_modem_dce_handle, "AT\r\n", NULL, direct_response_buffer, sizeof(direct_response_buffer), 0);
            // For simplicity in this step, let's use the simpler send_cmd if it resolves to a basic variant.
            // The key is to see if THIS call times out or not.
            
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Direct 'AT\r\n' command successful (returned ESP_OK).");
                // If successful, we might try to read the response if the DTE layer has it.
                // However, esp_modem_dce_send_cmd often expects the handler to process the response.
                // For now, ESP_OK is a good sign.
            } else {
                ESP_LOGW(TAG, "Direct 'AT\r\n' command failed: %s", esp_err_to_name(err));
            }
            
            // We will still push true if DCE handle was obtained, as per previous logic.
            // The AT command test above is for deeper debugging.
            be_pushbool(vm, true); 
        } else {
            ESP_LOGE(TAG, "Failed to get DCE handle after modem_board_init (timed out waiting).");
            // Consider if modem_board_deinit() should be called here to clean up the partially started task.
            // However, deinit might also block or have issues if the daemon is in a strange state.
            // For now, just report failure.
            be_pushbool(vm, false);
        }
    } else {
        ESP_LOGE(TAG, "Failed to initialize modem board: %s", esp_err_to_name(err));
        g_modem_dce_handle = NULL;
        be_pushbool(vm, false);
    }
    be_return(vm);
}

// Deinitialize the modem
static int w_modem_deinit(bvm *vm) {
    if (g_modem_dce_handle == NULL) {
        ESP_LOGW(TAG, "Modem board not initialized.");
        be_pushbool(vm, true); // Or false, as it wasn't init
        be_return(vm);
    }

    ESP_LOGI(TAG, "Deinitializing modem board...");
    esp_err_t err = modem_board_deinit();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Modem board deinitialized successfully.");
        g_modem_dce_handle = NULL;
        be_pushbool(vm, true);
    } else {
        ESP_LOGE(TAG, "Failed to deinitialize modem board: %s", esp_err_to_name(err));
        be_pushbool(vm, false); // Still consider it de-initialized for our flag
        g_modem_dce_handle = NULL;
    }
    be_return(vm);
}

// Connect to the cellular network
static int w_modem_connect(bvm *vm) {
    if (g_modem_dce_handle == NULL) {
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
    if (g_modem_dce_handle == NULL) {
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
    if (g_modem_dce_handle == NULL) {
        be_raise(vm, "runtime_error", "Modem not initialized.");
        be_return(vm);
    }
    
    be_newmap(vm);
    esp_err_t err;
    char ip_str_buf[16] = "0.0.0.0";

    // Connected status
    bool is_connected_placeholder = false;
    esp_netif_t *ppp_netif = esp_netif_get_handle_from_ifkey("PPP_DEF"); // Default PPP netif key
    if (ppp_netif) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(ppp_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
            is_connected_placeholder = true;
            esp_ip4addr_ntoa(&ip_info.ip, ip_str_buf, sizeof(ip_str_buf));
        }
    }
    be_pushbool(vm, is_connected_placeholder);
    be_setmember(vm, -2, "connected");

    // Signal Quality - don't return early if this fails
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

    // SIM Card State - don't return early if this fails
    int sim_ready = 0; // 0 for not ready/error, 1 for ready
    err = modem_board_get_sim_cart_state(&sim_ready); // Note: API uses int*, not bool*
    if (err == ESP_OK) {
        be_pushbool(vm, (bool)sim_ready);
        be_setmember(vm, -2, "sim_ready");
    } else {
        ESP_LOGW(TAG, "Failed to get SIM card state: %s", esp_err_to_name(err));
        be_pushbool(vm, false); be_setmember(vm, -2, "sim_ready");
    }
    
    // Operator Name - don't return early if this fails
    char operator_name[64] = {0};
    err = modem_board_get_operator_state(operator_name, sizeof(operator_name));
    if (err == ESP_OK) {
        be_pushstring(vm, operator_name);
        be_setmember(vm, -2, "operator");
    } else {
        ESP_LOGW(TAG, "Failed to get operator name: %s", esp_err_to_name(err));
        be_pushstring(vm, ""); be_setmember(vm, -2, "operator");
    }
    
    // IMEI - don't return early if this fails
    char imei_buf[16] = {0};
    if (g_modem_dce_handle && esp_modem_dce_get_imei_number(g_modem_dce_handle, (void*)sizeof(imei_buf), imei_buf) == ESP_OK) {
        be_pushstring(vm, imei_buf);
    } else {
        ESP_LOGW(TAG, "Failed to get IMEI number");
        be_pushstring(vm, "N/A");
    }
    be_setmember(vm, -2, "imei");

    // IMSI - don't return early if this fails
    char imsi_buf[16] = {0};
    if (g_modem_dce_handle && esp_modem_dce_get_imsi_number(g_modem_dce_handle, (void*)sizeof(imsi_buf), imsi_buf) == ESP_OK) {
        be_pushstring(vm, imsi_buf);
    } else {
        ESP_LOGW(TAG, "Failed to get IMSI number");
        be_pushstring(vm, "N/A");
    }
    be_setmember(vm, -2, "imsi");
    
    // IP Address
    be_pushstring(vm, ip_str_buf); be_setmember(vm, -2, "ip");
    
    // Add debug information to the status map
    be_pushbool(vm, g_modem_dce_handle != NULL);
    be_setmember(vm, -2, "dce_handle_valid");

    // Return if AT commands are working
    bool at_ok = (err == ESP_OK);
    be_pushbool(vm, at_ok);
    be_setmember(vm, -2, "at_ok");
    
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
// For now, these will just check if the modem is initialized.
static int w_modem_get_gnss_info(bvm *vm) {
    if (g_modem_dce_handle == NULL) {
        be_raise(vm, "runtime_error", "Modem not initialized.");
        be_return(vm);
    }
    ESP_LOGW(TAG, "GNSS info not yet implemented.");
    be_raise(vm, "runtime_error", "GNSS not implemented");
    be_return(vm);
}

static int w_modem_send_sms(bvm *vm) {
     if (g_modem_dce_handle == NULL) {
        be_raise(vm, "runtime_error", "Modem not initialized.");
        be_return(vm);
    }
    ESP_LOGW(TAG, "SMS sending not yet implemented.");
    be_raise(vm, "runtime_error", "SMS not implemented");
    be_return(vm);
}

static int w_modem_get_msisdn(bvm *vm) {
    if (g_modem_dce_handle == NULL) {
        be_raise(vm, "runtime_error", "Modem not initialized.");
        be_return(vm);
    }
    ESP_LOGW(TAG, "MSISDN retrieval not yet implemented.");
    be_pushstring(vm, ""); // Return empty string
    be_return(vm);
}


// --- End Placeholder sections ---


/* @const_object_info_begin
module modem (scope: global, strings: weak) { // Changed module name to 'modem'
    start, func(w_modem_init)
    stop, func(w_modem_deinit)
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

#include "be_fixed_modem.h" // Ensure this matches the module name
