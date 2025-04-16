// be_modem_lib.c - Berry module for esp_modem PPPoS over USB
// Scaffolding for initial connect functionality
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

// Include only the C API for ESP Modem
#include "esp_modem_api.h"
#include "esp_modem_c_api_types.h"
#include "esp_modem_config.h"

// Include Tasmota specific headers for GPIO
#include "Arduino.h" // For pinMode, digitalWrite, delay

static const char* TAG = "MODEM_BERRY";

// --- Define Logical GPIO Function for Modem Power ---
// IMPORTANT: For proper integration, this should be defined in Tasmota's
// GPIO mapping system (e.g., user_config_override.h or gpio_map.h)
// We define it locally here as a placeholder.
#define GPIO_MODEM_PWR 100 // Example number, ensure it doesn't clash

// Global DCE and netif handles
static esp_modem_dce_t *g_modem_dce = NULL;
static esp_netif_t *g_modem_netif = NULL;

// Function declarations for Berry VM
static int w_modem_init(bvm *vm);
static int w_modem_connect(bvm *vm);
static int w_modem_disconnect(bvm *vm);
static int w_modem_status(bvm *vm);

// Need to export these symbols for Berry VM
BE_EXPORT_VARIABLE extern const bclass be_class_modem;

// Initialize the modem
static int w_modem_init(bvm *vm) {
    int top = be_top(vm);
    
    esp_log_level_set(TAG, ESP_LOG_INFO);
    ESP_LOGI(TAG, "Initializing modem");
    
    // Create a PPP netif instance
    esp_netif_config_t netif_ppp_config = ESP_NETIF_DEFAULT_PPP();
    g_modem_netif = esp_netif_new(&netif_ppp_config);
    if (g_modem_netif == NULL) {
        ESP_LOGE(TAG, "Failed to create netif instance");
        be_raise(vm, "value_error", "Failed to create netif instance");
        be_return(vm);
    }
    
    // Set up DTE configuration (for UART by default, but we'll use USB in the future)
    // For now, just use a placeholder DTE config since we'll need to use a USB-specific one
    esp_modem_dte_config_t dte_config = ESP_MODEM_DTE_DEFAULT_CONFIG();
    
    // Set up DCE configuration
    esp_modem_dce_config_t dce_config = ESP_MODEM_DCE_DEFAULT_CONFIG("internet");
    
    // Create the DCE using esp_modem_new_dev() with a specific model
    // ESP_MODEM_DCE_GENETIC is the correct enum value (not GENERIC)
    g_modem_dce = esp_modem_new_dev(ESP_MODEM_DCE_GENETIC, &dte_config, &dce_config, g_modem_netif);
    
    if (g_modem_dce == NULL) {
        ESP_LOGE(TAG, "Failed to create modem DCE");
        esp_netif_destroy(g_modem_netif);
        g_modem_netif = NULL;
        be_raise(vm, "value_error", "Failed to initialize modem");
        be_return(vm);
    }
    
    ESP_LOGI(TAG, "Modem initialized successfully");
    be_pushbool(vm, true);
    be_return(vm);
}

// Connect to the modem
static int w_modem_connect(bvm *vm) {
    int top = be_top(vm);
    
    if (g_modem_dce == NULL) {
        be_raise(vm, "value_error", "Modem not initialized");
        be_return(vm);
    }
    
    // Extract parameters if provided (APN, username, password)
    const char *apn = "internet";  // Default APN
    const char *username = "";     // Default empty username
    const char *password = "";     // Default empty password
    
    if (be_top(vm) >= 1 && be_isstring(vm, 1)) {
        apn = be_tostring(vm, 1);
    }
    if (be_top(vm) >= 2 && be_isstring(vm, 2)) {
        username = be_tostring(vm, 2);
    }
    if (be_top(vm) >= 3 && be_isstring(vm, 3)) {
        password = be_tostring(vm, 3);
    }
    
    ESP_LOGI(TAG, "Connecting with APN: %s", apn);
    
    // Set up the APN
    esp_err_t err = esp_modem_set_apn(g_modem_dce, apn);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set APN: %s", esp_err_to_name(err));
        be_raise(vm, "value_error", "Failed to set APN");
        be_return(vm);
    }
    
    // Switch to data mode (this starts PPP)
    err = esp_modem_set_mode(g_modem_dce, ESP_MODEM_MODE_DATA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to switch to data mode: %s", esp_err_to_name(err));
        be_raise(vm, "value_error", "Failed to start PPP");
        be_return(vm);
    }
    
    be_pushbool(vm, true);
    be_return(vm);
}

// Disconnect from the modem
static int w_modem_disconnect(bvm *vm) {
    int top = be_top(vm);
    
    if (g_modem_dce == NULL) {
        be_raise(vm, "value_error", "Modem not initialized");
        be_return(vm);
    }
    
    ESP_LOGI(TAG, "Disconnecting modem");
    
    // Switch back to command mode (this stops PPP)
    esp_err_t err = esp_modem_set_mode(g_modem_dce, ESP_MODEM_MODE_COMMAND);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to switch to command mode: %s", esp_err_to_name(err));
        be_raise(vm, "value_error", "Failed to stop PPP");
        be_return(vm);
    }
    
    be_pushbool(vm, true);
    be_return(vm);
}

// Get modem status
static int w_modem_status(bvm *vm) {
    int top = be_top(vm);
    
    if (g_modem_dce == NULL) {
        be_raise(vm, "value_error", "Modem not initialized");
        be_return(vm);
    }
    
    // Create a map to return the status information
    be_newobject(vm, "map");
    
    // Check current mode
    esp_modem_dce_mode_t mode = esp_modem_get_mode(g_modem_dce);
    bool is_connected = (mode == ESP_MODEM_MODE_DATA);
    
    be_pushbool(vm, is_connected);
    be_setmember(vm, -2, "connected");
    
    // Get signal quality if we're in command mode or can pause data mode
    int rssi = 99; // Default to unknown
    int ber = 99;  // Default to unknown
    
    if (mode == ESP_MODEM_MODE_COMMAND) {
        // Get signal quality directly - using esp_err_t instead of command_result
        esp_err_t err = esp_modem_get_signal_quality(g_modem_dce, &rssi, &ber);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to get signal quality");
        }
    } else if (mode == ESP_MODEM_MODE_DATA) {
        // Temporarily pause network to get signal quality
        esp_err_t err = esp_modem_pause_net(g_modem_dce, true);
        if (err == ESP_OK) {
            err = esp_modem_get_signal_quality(g_modem_dce, &rssi, &ber);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Failed to get signal quality");
            }
            // Resume network
            esp_modem_pause_net(g_modem_dce, false);
        } else {
            ESP_LOGW(TAG, "Failed to pause net for signal quality check");
        }
    }
    
    be_pushint(vm, rssi);
    be_setmember(vm, -2, "rssi");
    
    be_pushint(vm, ber);
    be_setmember(vm, -2, "ber");
    
    // Get IP information if connected
    // Note: Getting the actual IP would require using ESP-NETIF APIs
    // which can be added later if needed
    if (is_connected) {
        // Placeholder IP address
        be_pushstring(vm, "0.0.0.0");
        be_setmember(vm, -2, "ip");
    }
    
    be_pop(vm, be_top(vm) - top - 1); // Leave the map object on top
    be_return(vm);
}

/* @const_object_info_begin
module modem (scope: global, strings: weak) {
    init, func(w_modem_init)
    connect, func(w_modem_connect)
    disconnect, func(w_modem_disconnect)
    status, func(w_modem_status)
}
@const_object_info_end */

#include "be_fixed_modem.h" // Generated by Tasmota build process
