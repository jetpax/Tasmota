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
#include "freertos/queue.h" // For NMEA queue
#include <ctype.h>        // For isspace in sms_prompt_handler

// For modem_board_* API and MODEM_DEFAULT_CONFIG
// from iot_usbh_modem  and iot_usbh_cdc esp-idf components
#include "usbh_modem_board.h"
#include "esp_modem_dce_common_commands.h"

// For esp_netif functions to check IP address
#include "esp_netif.h"

static const char* TAG = "MDM_BE";

// Global state for the new driver
static esp_modem_dce_t *g_modem_dce_handle = NULL; // Stores the DCE handle from esp_modem_iot
static esp_event_handler_t g_user_event_handler = NULL;
static void *g_user_event_handler_arg = NULL;

// NMEA Data Queue
#define NMEA_QUEUE_LENGTH 10
#define NMEA_MAX_SENTENCE_LENGTH 90 // NMEA sentences are typically <= 82 chars + null
static QueueHandle_t nmea_data_queue = NULL;

// Modem type - used to customize AT commands for different modems (may still be needed for GNSS/SMS)
typedef enum {
    MODEM_TYPE_GENERIC = 0,
    MODEM_TYPE_SIM7600,
    MODEM_TYPE_SIM800,
    MODEM_TYPE_BG96
} modem_type_t;
static modem_type_t g_modem_type = MODEM_TYPE_SIM7600; // Default to SIM7600


// Context for our custom AT command handler
typedef struct {
    char *response_buffer;       // Dynamically allocated buffer to store multi-line response
    size_t buffer_size;          // Current allocated size of response_buffer
    size_t current_len;          // Current length of data in response_buffer
    bool command_done;           // Flag to indicate if OK/ERROR received
    esp_err_t command_status;    // ESP_OK if modem reported OK, ESP_FAIL if modem reported ERROR, ESP_ERR_NO_MEM for allocation issues
} at_cmd_handler_ctx_t;

// AT command response handler
static esp_err_t at_cmd_response_handler(esp_modem_dce_t *dce, const char *line) {
    at_cmd_handler_ctx_t *ctx = (at_cmd_handler_ctx_t *)dce->handle_line_ctx;
    size_t line_len = strlen(line);

    // ESP_LOGD(TAG, "AT CMD Line: %s", line);

    // Check if buffer needs to grow
    // Add 1 for potential newline and 1 for null terminator.
    if (ctx->response_buffer == NULL || (ctx->current_len + line_len + 2 > ctx->buffer_size)) {
        size_t new_size = (ctx->buffer_size == 0) ? ((256 > line_len + 2) ? 256 : line_len + 2) : ctx->buffer_size * 2;
        if (new_size < ctx->current_len + line_len + 2) {
            new_size = ctx->current_len + line_len + 2;
        }
        char *new_buf = realloc(ctx->response_buffer, new_size);
        if (!new_buf) {
            ESP_LOGE(TAG, "Failed to realloc response buffer for AT command");
            if(ctx->response_buffer) free(ctx->response_buffer);
            ctx->response_buffer = NULL; // Mark as freed
            ctx->buffer_size = 0;
            ctx->current_len = 0;
            ctx->command_done = true;
            ctx->command_status = ESP_ERR_NO_MEM;
            // Cannot store more, tell modem stack we failed.
            return esp_modem_process_command_done(dce, ESP_MODEM_STATE_FAIL);
        }
        ctx->response_buffer = new_buf;
        ctx->buffer_size = new_size;
    }

    memcpy(ctx->response_buffer + ctx->current_len, line, line_len);
    ctx->current_len += line_len;
    
    // Add a newline character if the original line didn't end with one,
    // and if it wasn't just an empty line (which can happen between final code and previous line).
    // This helps to make multiline responses more readable.
    bool original_line_had_cr_lf = (line_len > 0 && (line[line_len-1] == '\n' || line[line_len-1] == '\r'));
    if (!original_line_had_cr_lf && line_len > 0) { // Don't add newline to empty lines from modem
      if (ctx->current_len < ctx->buffer_size -1) { // Ensure space for newline
        ctx->response_buffer[ctx->current_len++] = '\n';
      }
    }
    ctx->response_buffer[ctx->current_len] = '\0'; // Null-terminate

    if (strstr(line, MODEM_RESULT_CODE_SUCCESS)) { // "OK"
        ctx->command_done = true;
        ctx->command_status = ESP_OK;
        return esp_modem_process_command_done(dce, ESP_MODEM_STATE_SUCCESS);
    } else if (strstr(line, MODEM_RESULT_CODE_ERROR) || strstr(line, "+CME ERROR:") || strstr(line, "+CMS ERROR:")) {
        // Note: Modem may send ERROR or +CME ERROR / +CMS ERROR
        ctx->command_done = true;
        ctx->command_status = ESP_FAIL; 
        return esp_modem_process_command_done(dce, ESP_MODEM_STATE_FAIL);
    }
    // Handle common prompts like "> " for SMS/data input if this handler were to be used for those.
    // For generic commands, we typically wait for OK/ERROR.
    // Some commands might return data and then OK without an intermediate prompt.

    return ESP_OK; // Processed this line, but command not yet terminated by OK/ERROR
}


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

            ESP_LOGI(TAG, "Attempting 'AT\r\n' command via esp_modem_dce_sync...");
            
            if (g_modem_dce_handle->sync) {
                // esp_modem_dce_sync internally uses esp_modem_dce_generic_command with "AT\r" 
                // and esp_modem_dce_handle_response_default.
                err = g_modem_dce_handle->sync(g_modem_dce_handle, NULL, NULL);
                if (err == ESP_OK) {
                    ESP_LOGI(TAG, "'AT' (sync) command successful.");
                } else {
                    ESP_LOGW(TAG, "'AT' (sync) command failed: %s", esp_err_to_name(err));
                }
            } else {
                 ESP_LOGW(TAG, "g_modem_dce_handle->sync is NULL, cannot perform basic AT check this way.");
                 // If sync is NULL, it implies a problem with DCE initialization or the specific modem DCE setup.
                 // The esp_modem_dce_default_init should populate this.
            }
            
            // Initialize NMEA queue if not already
            if (!nmea_data_queue) {
                nmea_data_queue = xQueueCreate(NMEA_QUEUE_LENGTH, sizeof(char*));
                if (nmea_data_queue == NULL) {
                    ESP_LOGE(TAG, "Failed to create NMEA queue!");
                    // This is a problem, but init might still be considered "partially" successful for AT commands.
                    // Or we could fail the whole init here. For now, just log.
                } else {
                    ESP_LOGI(TAG, "NMEA queue created.");
                }
            }
            be_pushbool(vm, true); 
        } else {
            ESP_LOGE(TAG, "Failed to get DCE handle after modem_board_init (timed out waiting).");
            if (nmea_data_queue) { // Clean up queue if it was somehow created before DCE failed
                vQueueDelete(nmea_data_queue);
                nmea_data_queue = NULL;
            }
            be_pushbool(vm, false);
        }
    } else {
        ESP_LOGE(TAG, "Failed to initialize modem board: %s", esp_err_to_name(err));
        g_modem_dce_handle = NULL;
        if (nmea_data_queue) { // Clean up queue if it was created in a previous partial init
             vQueueDelete(nmea_data_queue);
             nmea_data_queue = NULL;
        }
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
        if (nmea_data_queue != NULL) {
            ESP_LOGI(TAG, "Clearing and deleting NMEA queue...");
            char* temp_ptr;
            while(xQueueReceive(nmea_data_queue, &temp_ptr, 0) == pdPASS) {
                if (temp_ptr) { // Should always be true if only non-NULL pointers were added
                    free(temp_ptr);
                }
            }
            vQueueDelete(nmea_data_queue);
            nmea_data_queue = NULL;
            ESP_LOGI(TAG, "NMEA queue deleted.");
        }
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
    char imei_buf[32] = {0}; // Increased buffer size for safety
    if (g_modem_dce_handle && esp_modem_dce_get_imei_number(g_modem_dce_handle, (void*)sizeof(imei_buf), imei_buf) == ESP_OK) {
        be_pushstring(vm, imei_buf);
    } else {
        ESP_LOGW(TAG, "Failed to get IMEI number. DCE handle: %p", g_modem_dce_handle);
        be_pushstring(vm, "N/A");
    }
    be_setmember(vm, -2, "imei");

    // IMSI - don't return early if this fails
    char imsi_buf[32] = {0}; // Increased buffer size for safety
    if (g_modem_dce_handle && esp_modem_dce_get_imsi_number(g_modem_dce_handle, (void*)sizeof(imsi_buf), imsi_buf) == ESP_OK) {
        be_pushstring(vm, imsi_buf);
    } else {
        ESP_LOGW(TAG, "Failed to get IMSI number. DCE handle: %p", g_modem_dce_handle);
        be_pushstring(vm, "N/A");
    }
    be_setmember(vm, -2, "imsi");
    
    // IP Address
    be_pushstring(vm, ip_str_buf); be_setmember(vm, -2, "ip");
    
    // Add debug information to the status map
    be_pushbool(vm, g_modem_dce_handle != NULL);
    be_setmember(vm, -2, "dce_handle_valid");

    // The 'at_ok' variable was reflecting the status of the last command in this function (modem_board_get_operator_state)
    // This might not be a reliable indicator of general AT command health.
    // The AT check in init is a better place. We can remove this or make it more robust.
    // For now, keep it as is, but be aware of its original context.
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

// Context for the SMS prompt (>) handler
typedef struct {
    bool prompt_received;
    esp_err_t command_status; // ESP_OK if prompt seen, ESP_FAIL otherwise
} sms_prompt_handler_ctx_t;

// Handler for AT+CMGS, waiting for '>' prompt
static esp_err_t sms_prompt_handler(esp_modem_dce_t *dce, const char *line) {
    sms_prompt_handler_ctx_t *ctx = (sms_prompt_handler_ctx_t *)dce->handle_line_ctx;
    // ESP_LOGD(TAG, "SMS Prompt Handler Line: %s", line);

    // Trim leading/trailing whitespace for robust check, especially for "> "
    const char *start = line;
    while (*start && isspace((unsigned char)*start)) start++;
    
    char *end = (char *)(start + strlen(start) - 1);
    while (end > start && isspace((unsigned char)*end)) end--;
    *(end + 1) = '\0';

    if (strcmp(start, ">") == 0) {
        // ESP_LOGD(TAG, "SMS prompt '>' received.");
        ctx->prompt_received = true;
        ctx->command_status = ESP_OK;
        // IMPORTANT: Do NOT call esp_modem_process_command_done here.
        // The command AT+CMGS is not "done" until the message body and Ctrl+Z are sent
        // and a final OK/ERROR is received. This handler only confirms the prompt.
        // The DTE layer will continue to wait for more input (the message body).
        // However, for the purpose of esp_modem_dce_generic_command, this specific *sub-command* (waiting for prompt) is done.
        // This is a bit nuanced. The C++ example returns OK here and then sends more data.
        // For esp_modem_dce_generic_command, we need to signal this part is done to proceed.
        return esp_modem_process_command_done(dce, ESP_MODEM_STATE_SUCCESS);
    } else if (strstr(line, MODEM_RESULT_CODE_SUCCESS) || strstr(line, MODEM_RESULT_CODE_ERROR) || strstr(line, "+CME ERROR:") || strstr(line, "+CMS ERROR:")) {
        // If we get OK or ERROR before '>', something is wrong with AT+CMGS itself.
        ESP_LOGW(TAG, "SMS: Received OK/ERROR before '>' prompt: %s", line);
        ctx->prompt_received = false;
        ctx->command_status = ESP_FAIL;
        return esp_modem_process_command_done(dce, ESP_MODEM_STATE_FAIL);
    }
    return ESP_OK; // Still waiting for '>' or final error
}

// Function to send an SMS
static int w_modem_send_sms(bvm *vm) {
    if (g_modem_dce_handle == NULL) {
        be_raise(vm, "runtime_error", "Modem not initialized. Call modem.init() first.");
        return -1;
    }

    if (g_modem_dce_handle->mode != ESP_MODEM_COMMAND_MODE) {
        ESP_LOGW(TAG, "Modem is not in COMMAND_MODE (current mode: %d). Sending SMS might fail.", g_modem_dce_handle->mode);
        // Some modems might allow SMS from a secondary AT port if primary is in PPP.
    }

    const char *phone_number;
    const char *message_text;

    int top = be_top(vm);
    if (top < 2 || !be_isstring(vm, 1) || !be_isstring(vm, 2)) {
        be_raise(vm, "type_error", "send_sms(phone_number_str, message_str) arguments required");
        return -1;
    }
    phone_number = be_tostring(vm, 1);
    message_text = be_tostring(vm, 2);

    if (strlen(phone_number) == 0) {
        be_raise(vm, "value_error", "Phone number cannot be empty");
        return -1;
    }
    if (strlen(message_text) > 160) { // Basic check, PDU mode handles longer
        ESP_LOGW(TAG, "Message text is longer than 160 chars, may be truncated or fail in text mode.");
    }

    // Stage 1: Send AT+CMGS="<number>"
    char *cmgs_cmd = NULL;
    // Max phone number length usually ~20 chars. "AT+CMGS=\""\r\0" is 13 chars.
    size_t cmgs_cmd_len = strlen("AT+CMGS=\"\"\r") + strlen(phone_number) + 1;
    cmgs_cmd = malloc(cmgs_cmd_len);
    if (!cmgs_cmd) {
        be_raise(vm, "runtime_error", "Memory allocation failed for CMGS command");
        return -1;
    }
    snprintf(cmgs_cmd, cmgs_cmd_len, "AT+CMGS=\"%s\"\r", phone_number);

    sms_prompt_handler_ctx_t prompt_ctx;
    memset(&prompt_ctx, 0, sizeof(sms_prompt_handler_ctx_t));

    ESP_LOGD(TAG, "Sending CMGS: [%s]", cmgs_cmd);
    esp_err_t err = esp_modem_dce_generic_command(g_modem_dce_handle, cmgs_cmd, 5000, sms_prompt_handler, &prompt_ctx);
    free(cmgs_cmd);

    if (err != ESP_OK || !prompt_ctx.prompt_received || prompt_ctx.command_status != ESP_OK) {
        char err_buf[128];
        snprintf(err_buf, sizeof(err_buf), "Failed to get SMS prompt '>'. IDF err: %s, Prompt: %d, Handler status: %s",
                 esp_err_to_name(err), prompt_ctx.prompt_received, esp_err_to_name(prompt_ctx.command_status));
        be_raise(vm, "runtime_error", err_buf);
        return -1;
    }

    // Stage 2: Send message_text + Ctrl+Z
    size_t msg_body_len = strlen(message_text) + 1 + 1; // +1 for Ctrl+Z, +1 for null terminator
    char *msg_body = malloc(msg_body_len);
    if (!msg_body) {
        be_raise(vm, "runtime_error", "Memory allocation failed for SMS message body");
        return -1;
    }
    strcpy(msg_body, message_text);
    msg_body[msg_body_len - 2] = 0x1A; // Ctrl+Z
    msg_body[msg_body_len - 1] = '\0';  // Null terminate

    at_cmd_handler_ctx_t final_response_ctx; // Use the general AT command handler for the final response
    memset(&final_response_ctx, 0, sizeof(at_cmd_handler_ctx_t));

    ESP_LOGD(TAG, "Sending SMS body (len %d, last char 0x%02X)...", strlen(msg_body), msg_body[strlen(msg_body)-1]);

    // The "command" here is the actual data payload ending with Ctrl+Z.
    // No need to append \r here.
    err = esp_modem_dce_generic_command(g_modem_dce_handle, msg_body, 120000, at_cmd_response_handler, &final_response_ctx);
    free(msg_body);

    if (final_response_ctx.command_status == ESP_ERR_NO_MEM) {
        if(final_response_ctx.response_buffer) free(final_response_ctx.response_buffer);
        be_raise(vm, "runtime_error", "Out of memory in SMS final response handler");
        return -1;
    }

    if (err == ESP_OK && final_response_ctx.command_status == ESP_OK) {
        if (final_response_ctx.response_buffer) {
            if (final_response_ctx.current_len > 0 && final_response_ctx.response_buffer[final_response_ctx.current_len-1] == '\n') {
                 final_response_ctx.response_buffer[final_response_ctx.current_len-1] = '\0';
            }
            be_pushnstring(vm, final_response_ctx.response_buffer, strlen(final_response_ctx.response_buffer));
            free(final_response_ctx.response_buffer);
        } else {
            be_pushstring(vm, "OK"); // SMS Sent, modem might just return OK
        }
        return BE_OK;
    } else {
        char err_msg_buf[256];
        const char *resp_preview = final_response_ctx.response_buffer ? final_response_ctx.response_buffer : "N/A";
         if (final_response_ctx.response_buffer && strlen(resp_preview) > 60) {
            snprintf(err_msg_buf, sizeof(err_msg_buf), "SMS send failed. IDF err: %s. Modem status: %s. Resp: %.60s...",
                     esp_err_to_name(err),
                     (final_response_ctx.command_status == ESP_OK && err != ESP_OK) ? "OK_BUT_IDF_ERR" : esp_err_to_name(final_response_ctx.command_status),
                     resp_preview);
        } else {
            snprintf(err_msg_buf, sizeof(err_msg_buf), "SMS send failed. IDF err: %s. Modem status: %s. Resp: %s",
                     esp_err_to_name(err),
                     (final_response_ctx.command_status == ESP_OK && err != ESP_OK) ? "OK_BUT_IDF_ERR" : esp_err_to_name(final_response_ctx.command_status),
                     resp_preview);
        }
        if (final_response_ctx.response_buffer) {
            free(final_response_ctx.response_buffer);
        }
        be_raise(vm, "runtime_error", err_msg_buf);
        return -1;
    }
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

// Berry function to get the next available NMEA sentence
static int w_modem_get_nmea(bvm *vm) {
    if (!nmea_data_queue) {
        // This case should ideally not happen if modem.init() was successful
        // and initialized the queue.
        // If init failed, the queue wouldn't exist.
        // If called before init, this is also an issue.
        // For robustness, we can raise or return nil.
        // be_raise(vm, "runtime_error", "NMEA queue not initialized. Call modem.init() first.");
        // return -1; 
        be_pushnil(vm); // Return nil if queue doesn't exist
        return BE_OK;
    }
    char *nmea_sentence_ptr = NULL;
    if (xQueueReceive(nmea_data_queue, &nmea_sentence_ptr, 0) == pdPASS) { // Non-blocking read
        if (nmea_sentence_ptr) {
            be_pushstring(vm, nmea_sentence_ptr);
            free(nmea_sentence_ptr); // Berry VM makes its own copy of the string
        } else {
            // This should not happen if only valid, allocated strings are put on the queue
            ESP_LOGE(TAG, "NMEA queue received a NULL pointer!");
            be_pushnil(vm);
        }
    } else {
        be_pushnil(vm); // Queue is empty
    }
    return BE_OK;
}

// Public C function to be called by the USB DTE layer (or other NMEA source)
// This function takes ownership of the string if successfully queued.
void be_modem_enqueue_nmea_sentence(const char *nmea_sentence_const) {
    if (!nmea_data_queue) {
        ESP_LOGW(TAG, "NMEA queue not initialized, cannot enqueue sentence.");
        return;
    }
    if (!nmea_sentence_const) {
        ESP_LOGW(TAG, "Attempted to enqueue NULL NMEA sentence.");
        return;
    }

    // Make a copy to put on the queue, as the original buffer might be reused by the caller
    char *nmea_copy = strdup(nmea_sentence_const);
    if (nmea_copy) {
        if (xQueueSend(nmea_data_queue, &nmea_copy, 0) != pdPASS) { // Non-blocking send, 0 ticks timeout
            ESP_LOGW(TAG, "NMEA queue full, sentence dropped: %s", nmea_copy);
            free(nmea_copy); // Free the copy if not enqueued
        } else {
            // ESP_LOGD(TAG, "Enqueued NMEA: %s", nmea_copy); // Can be verbose
        }
    } else {
        ESP_LOGE(TAG, "Failed to strdup NMEA sentence for queue: %s", nmea_sentence_const);
    }
}

// Function to send an arbitrary AT command
static int w_modem_at_command(bvm *vm) {
    if (g_modem_dce_handle == NULL) {
        be_raise(vm, "runtime_error", "Modem not initialized. Call modem.init() first.");
        return -1; // Indicate error to C caller, be_raise signals VM
    }

    if (g_modem_dce_handle->mode != ESP_MODEM_COMMAND_MODE) {
        ESP_LOGW(TAG, "Modem is not in COMMAND_MODE (current mode: %d). Sending AT command might fail or be disruptive.", g_modem_dce_handle->mode);
        // Allow to proceed, but user should be cautious if PPP is active on this interface.
    }

    const char *raw_command_from_berry;
    uint32_t timeout_ms = CONFIG_MODEM_COMMAND_TIMEOUT_DEFAULT; 

    int top = be_top(vm);
    if (top < 1 || !be_isstring(vm, 1)) {
        be_raise(vm, "type_error", "AT command string argument required");
        return -1; // Indicate error
    }
    raw_command_from_berry = be_tostring(vm, 1);

    if (top >= 2 && be_isint(vm, 2)) {
        timeout_ms = (uint32_t)be_toint(vm, 2);
        if (timeout_ms == 0 || timeout_ms > 300000) { // Max 5 min timeout, 0 means use default
            timeout_ms = CONFIG_MODEM_COMMAND_TIMEOUT_DEFAULT; 
        }
    }
    
    size_t raw_cmd_len = strlen(raw_command_from_berry);
    if (raw_cmd_len == 0) {
        be_raise(vm, "value_error", "AT command cannot be empty");
        return -1; // Indicate error
    }

    // Prepare command: must end with \r
    char *command_to_send = malloc(raw_cmd_len + 2); // for \r and \0
    if (!command_to_send) {
        be_raise(vm, "runtime_error", "Memory allocation failed for command buffer");
        return -1; // Indicate error
    }
    memcpy(command_to_send, raw_command_from_berry, raw_cmd_len);
    command_to_send[raw_cmd_len] = '\r';
    command_to_send[raw_cmd_len + 1] = '\0'; // Correct null termination

    at_cmd_handler_ctx_t handler_ctx;
    memset(&handler_ctx, 0, sizeof(at_cmd_handler_ctx_t));

    ESP_LOGD(TAG, "Sending AT: [%s] (Timeout: %dms)", command_to_send, timeout_ms);

    esp_err_t err = esp_modem_dce_generic_command(g_modem_dce_handle, command_to_send, timeout_ms, at_cmd_response_handler, &handler_ctx);
    
    free(command_to_send);

    if (handler_ctx.command_status == ESP_ERR_NO_MEM) { // Check for OOM in handler first
        if (handler_ctx.response_buffer) free(handler_ctx.response_buffer);
        be_raise(vm, "runtime_error", "Out of memory in AT command handler");
        return -1; // Indicate error
    }

    // err from esp_modem_dce_generic_command indicates if the command processing itself timed out or had issues before/after handler.
    // handler_ctx.command_status indicates if the modem reported OK or ERROR.
    if (err == ESP_OK && handler_ctx.command_status == ESP_OK) {
        if (handler_ctx.response_buffer) {
             // Trim trailing newline if we added one and the very last line from modem was empty or just OK/ERROR
            if (handler_ctx.current_len > 0 && handler_ctx.response_buffer[handler_ctx.current_len-1] == '\n') {
                 // A bit simplistic, but often helps clean up.
                 handler_ctx.response_buffer[handler_ctx.current_len-1] = '\0';
                 handler_ctx.current_len--;
            }
            be_pushnstring(vm, handler_ctx.response_buffer, handler_ctx.current_len);
            free(handler_ctx.response_buffer);
        } else {
            be_pushstring(vm, ""); // Command OK, but no response body (e.g. "AT")
        }
        return BE_OK; // Explicitly return BE_OK for success
    } else {
        char err_msg_buf[256];
        const char *resp_preview = handler_ctx.response_buffer ? handler_ctx.response_buffer : "N/A";
        if (handler_ctx.response_buffer && strlen(resp_preview) > 60) { // Truncate long responses in error message
            snprintf(err_msg_buf, sizeof(err_msg_buf), "AT cmd failed. IDF err: %s. Modem status: %s. Resp: %.60s...",
                     esp_err_to_name(err),
                     (handler_ctx.command_status == ESP_OK && err != ESP_OK) ? "OK_BUT_IDF_ERR" : esp_err_to_name(handler_ctx.command_status),
                     resp_preview);
        } else {
            snprintf(err_msg_buf, sizeof(err_msg_buf), "AT cmd failed. IDF err: %s. Modem status: %s. Resp: %s",
                     esp_err_to_name(err),
                     (handler_ctx.command_status == ESP_OK && err != ESP_OK) ? "OK_BUT_IDF_ERR" : esp_err_to_name(handler_ctx.command_status),
                     resp_preview);
        }
        
        if (handler_ctx.response_buffer) {
            free(handler_ctx.response_buffer);
        }
        be_raise(vm, "runtime_error", err_msg_buf);
        return -1; // Indicate error
    }
}


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

    // New function for arbitrary AT commands
    at_command, func(w_modem_at_command)

    // New function to get NMEA data
    get_nmea, func(w_modem_get_nmea)

    // Removed: init_uart, init_usb, set_power_pin, set_reset_pin, power_on, power_off, reset
    // Callbacks on_connect/on_disconnect might be replaced by a generic on_event
}
@const_object_info_end */

#include "be_fixed_modem.h" // Ensure this matches the module name
