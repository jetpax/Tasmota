/*
  be_modem_lib.c - Berry module for USB WAN modem

  assumes USB modem exposes multiple interfaces:
  - the AT interface is used for setting up the modem and PPP and retreiving status.
  - the PPP interface is used for PPP operations, including IP address allocation.

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
            
            // NMEA queue init was here - removed
            be_pushbool(vm, true); 
        } else {
            ESP_LOGE(TAG, "Failed to get DCE handle after modem_board_init (timed out waiting).");
            // NMEA queue cleanup was here - removed
            be_pushbool(vm, false);
        }
    } else {
        ESP_LOGE(TAG, "Failed to initialize modem board: %s", esp_err_to_name(err));
        g_modem_dce_handle = NULL;
        // NMEA queue cleanup was here - removed
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
        // NMEA queue deinit was here - removed
        be_pushbool(vm, true);
    } else {
        ESP_LOGE(TAG, "Failed to deinitialize modem board: %s", esp_err_to_name(err));
        be_pushbool(vm, false); // Still consider it de-initialized for our flag
        g_modem_dce_handle = NULL;
        // NMEA queue cleanup was here - removed
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

// Get modem dynamic status (connection, IP, signal, registration)
static int w_modem_status(bvm *vm) {
    if (g_modem_dce_handle == NULL) {
        be_raise(vm, "runtime_error", "Modem not initialized.");
        return -1;
    }
    
    be_newmap(vm);
    esp_err_t err;
    char ip_str_buf[16] = "0.0.0.0";
    char gw_str_buf[16] = "0.0.0.0";
    char dns1_str_buf[16] = "0.0.0.0";
    char dns2_str_buf[16] = "0.0.0.0";
    bool is_connected = false;

    // PPP Connection status, IP Address, Gateway
    esp_netif_t *ppp_netif = esp_netif_get_handle_from_ifkey("PPP_DEF");
    if (ppp_netif) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(ppp_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
            is_connected = true;
            esp_ip4addr_ntoa(&ip_info.ip, ip_str_buf, sizeof(ip_str_buf));
            esp_ip4addr_ntoa(&ip_info.gw, gw_str_buf, sizeof(gw_str_buf));
        }

        // DNS Servers
        esp_netif_dns_info_t dns_info;
        if (esp_netif_get_dns_info(ppp_netif, ESP_NETIF_DNS_MAIN, &dns_info) == ESP_OK) {
            if (dns_info.ip.type == ESP_IPADDR_TYPE_V4) {
                esp_ip4addr_ntoa(&dns_info.ip.u_addr.ip4, dns1_str_buf, sizeof(dns1_str_buf));
            } // Add V6 handling if needed
        }
        if (esp_netif_get_dns_info(ppp_netif, ESP_NETIF_DNS_BACKUP, &dns_info) == ESP_OK) {
            if (dns_info.ip.type == ESP_IPADDR_TYPE_V4) {
                esp_ip4addr_ntoa(&dns_info.ip.u_addr.ip4, dns2_str_buf, sizeof(dns2_str_buf));
            } // Add V6 handling if needed
        }
        // Could also try ESP_NETIF_DNS_FALLBACK if needed
    }
    be_pushbool(vm, is_connected);
    be_setmember(vm, -2, "connected");
    be_pushstring(vm, ip_str_buf);
    be_setmember(vm, -2, "ip");
    be_pushstring(vm, gw_str_buf);
    be_setmember(vm, -2, "gateway");
    be_pushstring(vm, dns1_str_buf);
    be_setmember(vm, -2, "dns1");
    be_pushstring(vm, dns2_str_buf);
    be_setmember(vm, -2, "dns2");

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
        be_pushint(vm, -999); // Use a distinct error value for rssi
        be_setmember(vm, -2, "rssi");
        be_pushint(vm, -999); // Use a distinct error value for ber
        be_setmember(vm, -2, "ber");
    }

    // Network Registration State (AT+CREG?)
    at_cmd_handler_ctx_t creg_ctx;
    memset(&creg_ctx, 0, sizeof(at_cmd_handler_ctx_t));
    char creg_cmd[] = "AT+CREG?\r";
    int reg_n = -1, reg_stat = -1;

    err = esp_modem_dce_generic_command(g_modem_dce_handle, creg_cmd, CONFIG_MODEM_COMMAND_TIMEOUT_DEFAULT, at_cmd_response_handler, &creg_ctx);
    if (err == ESP_OK && creg_ctx.command_status == ESP_OK && creg_ctx.response_buffer) {
        // Expected response: +CREG: <n>,<stat>[,<lac>,<ci>[,<AcT>]]
        // Or just +CREG: <stat> if <n> was set to 0 by a previous AT+CREG=<n>
        char *p = strstr(creg_ctx.response_buffer, "+CREG:");
        if (p) {
            p += strlen("+CREG:");
            while (*p == ' ' || *p == '\t') p++; // Skip whitespace
            // Try parsing two values first, then one if that fails
            if (sscanf(p, "%d,%d", &reg_n, &reg_stat) == 2) {
                // Successfully parsed <n> and <stat>
            } else if (sscanf(p, "%d", &reg_stat) == 1) {
                // Successfully parsed only <stat> (assuming n=0 was set previously)
                reg_n = 0; // Assume n was previously set to 0
            } else {
                ESP_LOGW(TAG, "Could not parse CREG response: %s", creg_ctx.response_buffer);
                reg_stat = -1; // Parse error
            }
        } else {
            ESP_LOGW(TAG, "+CREG: prefix not found in response: %s", creg_ctx.response_buffer);
            reg_stat = -1; // Prefix not found
        }
        free(creg_ctx.response_buffer);
    } else {
        ESP_LOGW(TAG, "AT+CREG? failed. IDF err: %s, Modem status: %s", esp_err_to_name(err), esp_err_to_name(creg_ctx.command_status));
        if (creg_ctx.response_buffer) free(creg_ctx.response_buffer);
        reg_stat = -1; // Command failed
    }
    be_pushint(vm, reg_stat);
    be_setmember(vm, -2, "reg_status");
    // Optional: Push n as well if needed: be_pushint(vm, reg_n); be_setmember(vm, -2, "reg_mode");

    be_return(vm); // Return the map
}

// Get modem static information (IMEI, IMSI, MSISDN, ICCID, etc.)
static int w_modem_info(bvm *vm) {
    if (g_modem_dce_handle == NULL) {
        be_raise(vm, "runtime_error", "Modem not initialized.");
        return -1;
    }

    be_newmap(vm);
    esp_err_t err;
    char buffer[128]; // General purpose buffer for AT command responses

    // IMEI
    if (esp_modem_dce_get_imei_number(g_modem_dce_handle, (void*)sizeof(buffer), buffer) == ESP_OK) {
        be_pushstring(vm, buffer);
    } else {
        ESP_LOGW(TAG, "Failed to get IMEI via DCE function.");
        // Fallback to AT+GSN or AT+CGSN? Most modules support one.
        // For now, push N/A if DCE function fails.
        be_pushstring(vm, "N/A");
    }
    be_setmember(vm, -2, "imei");

    // IMSI
    if (esp_modem_dce_get_imsi_number(g_modem_dce_handle, (void*)sizeof(buffer), buffer) == ESP_OK) {
        be_pushstring(vm, buffer);
    } else {
        ESP_LOGW(TAG, "Failed to get IMSI via DCE function.");
        be_pushstring(vm, "N/A");
    }
    be_setmember(vm, -2, "imsi");

    // MSISDN (AT+CNUM)
    at_cmd_handler_ctx_t cnum_ctx;
    memset(&cnum_ctx, 0, sizeof(at_cmd_handler_ctx_t));
    char cnum_cmd[] = "AT+CNUM\r";
    err = esp_modem_dce_generic_command(g_modem_dce_handle, cnum_cmd, CONFIG_MODEM_COMMAND_TIMEOUT_OPERATOR, at_cmd_response_handler, &cnum_ctx);
    if (err == ESP_OK && cnum_ctx.command_status == ESP_OK && cnum_ctx.response_buffer) {
        // Expected: +CNUM: ["<alpha>"],"<number>",<type>[,...]
        char *num_start = strstr(cnum_ctx.response_buffer, "\",\""); // Look for <empty_alpha>,"number"
        if (num_start) {
            num_start += 3; // Skip past ","
            char *num_end = strchr(num_start, '\"');
            if (num_end) {
                *num_end = '\0';
                be_pushstring(vm, num_start);
            } else { be_pushstring(vm, ""); ESP_LOGW(TAG, "Could not parse CNUM number end quote"); }
        } else {
             // Try looking for format: +CNUM: <alpha>,<number>,<type>
            num_start = strstr(cnum_ctx.response_buffer, "+CNUM:");
            if (num_start) {
                num_start += strlen("+CNUM:");
                while(*num_start && *num_start == ' ') num_start++; // skip spaces
                char *alpha_end = strchr(num_start, ',');
                if (alpha_end) {
                    num_start = alpha_end + 1;
                    char *number_end = strchr(num_start, ',');
                    if (number_end) {
                        *number_end = '\0';
                        // Remove quotes if any around the number itself
                        char *s = num_start, *d = num_start;
                        while(*s) {
                            if (*s != '\"') *d++ = *s;
                            s++;
                        }
                        *d = '\0';
                        be_pushstring(vm, num_start);
                    } else { be_pushstring(vm, ""); ESP_LOGW(TAG, "Could not parse CNUM number end comma"); }
                } else { be_pushstring(vm, ""); ESP_LOGW(TAG, "Could not parse CNUM alpha end comma"); }
            } else { be_pushstring(vm, ""); ESP_LOGW(TAG, "CNUM response format not recognized"); }
        }
        free(cnum_ctx.response_buffer);
    } else {
        ESP_LOGW(TAG, "AT+CNUM failed. IDF err: %s, Modem status: %s", esp_err_to_name(err), esp_err_to_name(cnum_ctx.command_status));
        if(cnum_ctx.response_buffer) free(cnum_ctx.response_buffer);
        be_pushstring(vm, "");
    }
    be_setmember(vm, -2, "msisdn");

    // ICCID (AT+CCID)
    at_cmd_handler_ctx_t ccid_ctx;
    memset(&ccid_ctx, 0, sizeof(at_cmd_handler_ctx_t));
    char ccid_cmd[] = "AT+CCID\r";
    err = esp_modem_dce_generic_command(g_modem_dce_handle, ccid_cmd, CONFIG_MODEM_COMMAND_TIMEOUT_DEFAULT, at_cmd_response_handler, &ccid_ctx);
    if (err == ESP_OK && ccid_ctx.command_status == ESP_OK && ccid_ctx.response_buffer) {
        // Expected: +CCID: <iccid_val>  or just <iccid_val>
        char *iccid_val = ccid_ctx.response_buffer;
        if (strstr(iccid_val, "+CCID:")) {
            iccid_val = strstr(iccid_val, "+CCID:") + strlen("+CCID:");
            while (*iccid_val == ' ' || *iccid_val == '\t') iccid_val++; // Skip whitespace
        }
        // Remove trailing newlines/CRs that at_cmd_response_handler might have added
        size_t len = strlen(iccid_val);
        while (len > 0 && (iccid_val[len-1] == '\n' || iccid_val[len-1] == '\r')) {
            iccid_val[--len] = '\0';
        }
        // Remove OK if it's on the same line (some modems do this for CCID)
        char *ok_ptr = strstr(iccid_val, "OK");
        if (ok_ptr) { 
            // check if OK is preceded by only whitespace
            char *check_ptr = ok_ptr -1;
            while (check_ptr >= iccid_val && (*check_ptr == ' ' || *check_ptr == '\t')) {
                check_ptr--;
            }
            if (check_ptr < iccid_val || *check_ptr == '\n' || *check_ptr == '\r') { // OK is on its own line or start
                 // This case is handled by at_cmd_response_handler which should strip OK
            } else { // OK is appended to the CCID value, strip it
                *ok_ptr = '\0'; 
                len = strlen(iccid_val);
                while (len > 0 && (iccid_val[len-1] == ' ' || iccid_val[len-1] == '\t')) { // Trim trailing space before OK
                     iccid_val[--len] = '\0';
                }
            }
        }
        be_pushstring(vm, iccid_val);
        free(ccid_ctx.response_buffer);
    } else {
        ESP_LOGW(TAG, "AT+CCID failed. IDF err: %s, Modem status: %s", esp_err_to_name(err), esp_err_to_name(ccid_ctx.command_status));
        if(ccid_ctx.response_buffer) free(ccid_ctx.response_buffer);
        be_pushstring(vm, "");
    }
    be_setmember(vm, -2, "iccid");

    // Operator Name
    err = modem_board_get_operator_state(buffer, sizeof(buffer));
    if (err == ESP_OK) {
        be_pushstring(vm, buffer);
    } else {
        ESP_LOGW(TAG, "Failed to get operator name: %s", esp_err_to_name(err));
        be_pushstring(vm, "");
    }
    be_setmember(vm, -2, "operator");

    // SIM Card State
    int sim_ready_int = 0; 
    err = modem_board_get_sim_cart_state(&sim_ready_int);
    if (err == ESP_OK) {
        be_pushbool(vm, (bool)sim_ready_int);
    } else {
        ESP_LOGW(TAG, "Failed to get SIM card state: %s", esp_err_to_name(err));
        be_pushbool(vm, false);
    }
    be_setmember(vm, -2, "sim_ready");
    
    // DCE Handle Valid
    be_pushbool(vm, g_modem_dce_handle != NULL);
    be_setmember(vm, -2, "dce_handle_valid");

    // Modem Type (as configured in Berry lib)
    const char* type_str = "UNKNOWN";
    switch (g_modem_type) {
        case MODEM_TYPE_SIM7600: type_str = "SIM7600"; break;
        case MODEM_TYPE_SIM800:  type_str = "SIM800"; break;
        case MODEM_TYPE_BG96:    type_str = "BG96"; break;
        case MODEM_TYPE_GENERIC: type_str = "GENERIC"; break;
    }
    be_pushstring(vm, type_str);
    be_setmember(vm, -2, "modem_type");

    be_return(vm); // Return the map
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
    // Corrected return statement for w_modem_get_type
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
module modem (scope: global, strings: weak) {
    start, func(w_modem_init)
    stop, func(w_modem_deinit)
    connect, func(w_modem_connect)
    disconnect, func(w_modem_disconnect)
    status, func(w_modem_status)       // Now focused on dynamic status
    info, func(w_modem_info)           // New function for static info
    
    set_type, func(w_modem_set_type)
    get_type, func(w_modem_get_type)
    send_sms, func(w_modem_send_sms)

    at_command, func(w_modem_at_command)
}
@const_object_info_end */

#include "be_fixed_modem.h" // Ensure this matches the module name
