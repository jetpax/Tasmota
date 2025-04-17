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

// Include Arduino.h for GPIO operations 
#include "Arduino.h"

static const char* TAG = "MODEM_BERRY";

// Define a simple USB configuration structure to match esp_modem_usb_config.h
struct esp_modem_usb_term_config {
    uint16_t vid;                // Vendor ID of the USB device
    uint16_t pid;                // Product ID of the USB device
    int interface_idx;           // USB Interface index that will be used for primary terminal
    int secondary_interface_idx; // USB Interface index for secondary terminal (-1 if not used)
    uint32_t timeout_ms;         // Time for a USB modem to connect to USB host (0 = wait forever)
    int xCoreID;                 // Core affinity of created tasks
    bool cdc_compliant;          // Whether to treat the USB device as CDC-compliant
    bool install_usb_host;       // Whether USB Host driver should be installed
};

// Define our own macros based on esp_modem_usb_config.h
#define ESP_MODEM_DEFAULT_USB_CONFIG(_vid, _pid, _intf) {\
    .vid = _vid,\
    .pid = _pid,\
    .interface_idx = _intf,\
    .secondary_interface_idx = -1,\
    .timeout_ms = 0,\
    .xCoreID = 0,\
    .cdc_compliant = false,\
    .install_usb_host = true\
}

#define ESP_MODEM_DTE_DEFAULT_USB_CONFIG(_usb_config) {\
    .dte_buffer_size = 512,\
    .task_stack_size = 4096,\
    .task_priority = 5,\
    .extension_config = &_usb_config\
}

// Store modem pin configuration
static int8_t g_modem_power_pin = -1;
static int8_t g_modem_reset_pin = -1;

// Global DCE and netif handles
static esp_modem_dce_t *g_modem_dce = NULL;
static esp_netif_t *g_modem_netif = NULL;

// Modem type - used to customize AT commands for different modems
typedef enum {
    MODEM_TYPE_GENERIC = 0,
    MODEM_TYPE_SIM7600,
    MODEM_TYPE_SIM800,
    MODEM_TYPE_BG96
} modem_type_t;

static modem_type_t g_modem_type = MODEM_TYPE_SIM7600; // Default to SIM7600

// Get AT command for getting GNSS data based on modem type
static const char* get_gnss_command(void) {
    switch (g_modem_type) {
        case MODEM_TYPE_SIM7600:
            return "AT+CGNSINF\r"; // SIM7600 uses this command
        case MODEM_TYPE_BG96:
            return "AT+QGPSLOC=2\r"; // BG96 uses this command format
        case MODEM_TYPE_SIM800:
            return "AT+CGPSINF=0\r"; // SIM800 uses this command
        case MODEM_TYPE_GENERIC:
        default:
            return "AT+CGNSINF\r"; // Default to SIM7600 format
    }
}

// Store the GNSS response for parsing
static char *gnss_response = NULL;
static bool gnss_info_received = false;

// GNSS info line handler callback
static esp_err_t gnss_info_line_handler(uint8_t *data, size_t len) {
    if (data == NULL || len == 0) {
        return ESP_FAIL;
    }
    
    char *line = (char *)data;
    
    // Different modems have different response prefixes
    const char* sim7600_prefix = "+CGNSINF: ";
    const char* bg96_prefix = "+QGPSLOC: ";
    const char* sim800_prefix = "+CGPSINF: ";
    
    // Check for the appropriate prefix based on modem type
    const char* prefix = NULL;
    switch (g_modem_type) {
        case MODEM_TYPE_SIM7600:
            prefix = sim7600_prefix;
            break;
        case MODEM_TYPE_BG96:
            prefix = bg96_prefix;
            break;
        case MODEM_TYPE_SIM800:
            prefix = sim800_prefix;
            break;
        case MODEM_TYPE_GENERIC:
        default:
            prefix = sim7600_prefix; // Default to SIM7600 format
            break;
    }
    
    // Look for the GNSS info response
    if (strncmp(line, prefix, strlen(prefix)) == 0) {
        // Make a copy of the response for parsing
        if (gnss_response != NULL) {
            free(gnss_response);
        }
        gnss_response = strdup(line);
        gnss_info_received = true;
        return ESP_OK;
    }
    
    return ESP_OK;
}

// Function declarations for Berry VM
static int w_modem_init(bvm *vm);
static int w_modem_init_usb(bvm *vm);
static int w_modem_connect(bvm *vm);
static int w_modem_disconnect(bvm *vm);
static int w_modem_status(bvm *vm);
static int w_modem_power_on(bvm *vm);
static int w_modem_power_off(bvm *vm);
static int w_modem_set_power_pin(bvm *vm);
static int w_modem_set_reset_pin(bvm *vm);
static int w_modem_reset(bvm *vm);
static int w_modem_get_gnss_info(bvm *vm);
static int w_modem_set_type(bvm *vm);
static int w_modem_get_type(bvm *vm);

// Need to export these symbols for Berry VM
BE_EXPORT_VARIABLE extern const bclass be_class_modem;

// External function from be_modem_factory_bridge.cpp
extern esp_err_t modem_bridge_set_module_type(esp_modem_dce_t *dce, const char *module_type);

// Function to configure modem power pin
static int w_modem_set_power_pin(bvm *vm) {
    int top = be_top(vm);
    
    if (top >= 1 && be_isint(vm, 1)) {
        int8_t pin = be_toint(vm, 1);
        
        if (pin >= 0) {
            // Configure the pin as output
            pinMode(pin, OUTPUT);
            g_modem_power_pin = pin;
            ESP_LOGI(TAG, "Modem power pin set to GPIO%d", pin);
            be_pushbool(vm, true);
        } else {
            g_modem_power_pin = -1;
            ESP_LOGW(TAG, "Invalid modem power pin: %d", pin);
            be_pushbool(vm, false);
        }
    } else {
        be_raise(vm, "value_error", "Pin number required");
        be_return(vm);
    }
    
    be_return(vm);
}

// Function to configure modem reset pin
static int w_modem_set_reset_pin(bvm *vm) {
    int top = be_top(vm);
    
    if (top >= 1 && be_isint(vm, 1)) {
        int8_t pin = be_toint(vm, 1);
        
        if (pin >= 0) {
            // Configure the pin as output
            pinMode(pin, OUTPUT);
            g_modem_reset_pin = pin;
            ESP_LOGI(TAG, "Modem reset pin set to GPIO%d", pin);
            be_pushbool(vm, true);
        } else {
            g_modem_reset_pin = -1;
            ESP_LOGW(TAG, "Invalid modem reset pin: %d", pin);
            be_pushbool(vm, false);
        }
    } else {
        be_raise(vm, "value_error", "Pin number required");
        be_return(vm);
    }
    
    be_return(vm);
}

// Modem power control functions
static int w_modem_power_on(bvm *vm) {
    int top = be_top(vm);
    
    // Check if the power pin is configured
    if (g_modem_power_pin < 0) {
        ESP_LOGW(TAG, "Modem power pin not configured");
        be_pushbool(vm, false);
        be_return(vm);
    }
    
    // Power on sequence
    ESP_LOGI(TAG, "Powering on modem via GPIO%d", g_modem_power_pin);
    digitalWrite(g_modem_power_pin, HIGH);
    delay(100);
    digitalWrite(g_modem_power_pin, LOW);
    delay(5000); // Wait for modem to initialize
    
    be_pushbool(vm, true);
    be_return(vm);
}

static int w_modem_power_off(bvm *vm) {
    int top = be_top(vm);
    
    // Check if the power pin is configured
    if (g_modem_power_pin < 0) {
        ESP_LOGW(TAG, "Modem power pin not configured");
        be_pushbool(vm, false);
        be_return(vm);
    }
    
    // Power off sequence
    ESP_LOGI(TAG, "Powering off modem via GPIO%d", g_modem_power_pin);
    digitalWrite(g_modem_power_pin, HIGH);
    delay(800);
    digitalWrite(g_modem_power_pin, LOW);

    // If we have a modem DCE, clean it up
    if (g_modem_dce != NULL) {
        esp_modem_destroy(g_modem_dce);
        g_modem_dce = NULL;
    }
    
    // If we have a netif, clean it up
    if (g_modem_netif != NULL) {
        esp_netif_destroy(g_modem_netif);
        g_modem_netif = NULL;
    }
    
    be_pushbool(vm, true);
    be_return(vm);
}

// Modem reset function
static int w_modem_reset(bvm *vm) {
    int top = be_top(vm);
    
    // Check if the reset pin is configured
    if (g_modem_reset_pin < 0) {
        ESP_LOGW(TAG, "Modem reset pin not configured");
        be_pushbool(vm, false);
        be_return(vm);
    }
    
    // Reset sequence
    ESP_LOGI(TAG, "Resetting modem via GPIO%d", g_modem_reset_pin);
    digitalWrite(g_modem_reset_pin, HIGH);
    delay(200);  // Hold reset for 200ms
    digitalWrite(g_modem_reset_pin, LOW);
    delay(3000); // Wait for modem to restart
    
    be_pushbool(vm, true);
    be_return(vm);
}

// Get GNSS information from the modem
static int w_modem_get_gnss_info(bvm *vm) {
    int top = be_top(vm);
    
    if (g_modem_dce == NULL) {
        be_raise(vm, "runtime_error", "Modem not initialized");
        be_return(vm);
    }

    // Free any previous response
    if (gnss_response != NULL) {
        free(gnss_response);
        gnss_response = NULL;
    }
    
    // Reset the flag
    gnss_info_received = false;

    // Send AT command for GNSS info based on modem type
    esp_err_t err = esp_modem_command(g_modem_dce, get_gnss_command(), gnss_info_line_handler, 2000);

    if (err != ESP_OK) {
        be_raise(vm, "runtime_error", "Failed to execute GNSS command");
        be_return(vm);
    }
    
    // Check if we got a response
    if (!gnss_info_received || gnss_response == NULL) {
        be_raise(vm, "runtime_error", "No GNSS data received");
        be_return(vm);
    }

    // Parse the gnss_response
    // +CGNSINF: <run_status>,<fix_status>,<UTC>,<lat>,<lon>,<alt>,<speed_knots>,<course>,<fix_mode>,<res1>,<hdop>,<pdop>,<vdop>,<res2>,<sats_in_view>,<sats_used>,<glonass_sats_in_view>,<res3>,<cn0_max>,<hpa>,<vpa>
    int run_status = 0, fix_status = 0, fix_mode = 0, sats_in_view = 0, sats_used = 0, cn0_max = 0;
    char utc_datetime[20] = {0};
    double latitude = 0.0, longitude = 0.0, altitude = 0.0, speed_knots = 0.0, course = 0.0;
    double hdop = 0.0, pdop = 0.0, vdop = 0.0, hpa = 0.0, vpa = 0.0;
    // Use dummy vars for reserved fields if sscanf requires them
    char dummy1[10], dummy2[10], dummy3[10], dummy4[10];

    // Be careful with sscanf, it can be tricky. Check the number of fields assigned.
    int fields = sscanf(gnss_response, 
                        "%d,%d,%18[^,],%lf,%lf,%lf,%lf,%lf,%d,%[^,],%lf,%lf,%lf,%[^,],%d,%d,%[^,],%[^,],%d,%lf,%lf",
                        &run_status, &fix_status, utc_datetime,
                        &latitude, &longitude, &altitude,
                        &speed_knots, &course, &fix_mode,
                        dummy1, &hdop, &pdop, &vdop, dummy2,
                        &sats_in_view, &sats_used, dummy3, dummy4,
                        &cn0_max, &hpa, &vpa);

    if (fields < 17) { // Check if we parsed at least up to sats_used (adjust count as needed)
        ESP_LOGE("modem.gnss", "Failed to parse GNSS info: %s (fields=%d)", gnss_response, fields);
        be_raise(vm, "value_error", "Failed to parse GNSS info");
        be_return(vm);
    }

    // Process parsed data
    bool fixed = (fix_status == 1);
    double speed_kph = speed_knots * 1.852;

    // Extract Date and Time from UTC string (YYYYMMDDHHMMSS.sss)
    char date_str[9] = {0}; // YYYYMMDD
    char time_str[7] = {0}; // HHMMSS
    if (strlen(utc_datetime) >= 14) {
        strncpy(date_str, utc_datetime, 8);
        strncpy(time_str, utc_datetime + 8, 6);
    }

    // Create Berry map object
    be_newmap(vm);

    be_pushbool(vm, fixed);
    be_setmember(vm, -2, "fixed");
    
    be_pushreal(vm, latitude);
    be_setmember(vm, -2, "latitude");
    
    be_pushreal(vm, longitude);
    be_setmember(vm, -2, "longitude");
    
    be_pushreal(vm, altitude);
    be_setmember(vm, -2, "altitude");
    
    be_pushreal(vm, speed_kph); // Use converted speed
    be_setmember(vm, -2, "speed");
    
    be_pushreal(vm, course);
    be_setmember(vm, -2, "course");
    
    be_pushint(vm, sats_used); // Use sats_used field
    be_setmember(vm, -2, "satellites_used");
    
    be_pushint(vm, sats_in_view); // Use sats_in_view field
    be_setmember(vm, -2, "satellites_in_view");
    
    be_pushstring(vm, date_str);
    be_setmember(vm, -2, "date");
    
    be_pushstring(vm, time_str);
    be_setmember(vm, -2, "time");

    // Add HDOP, PDOP, VDOP for extra info
    be_pushreal(vm, hdop);
    be_setmember(vm, -2, "hdop");
    be_pushreal(vm, pdop);
    be_setmember(vm, -2, "pdop");
    be_pushreal(vm, vdop);
    be_setmember(vm, -2, "vdop");

    // Add C/N0 max
    be_pushint(vm, cn0_max);
    be_setmember(vm, -2, "cn0_max");
    
    // Leave the map object on top
    be_pop(vm, be_top(vm) - top - 1); 
    be_return(vm);
}

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

// Initialize the modem with USB DTE
static int w_modem_init_usb(bvm *vm) {
    int top = be_top(vm);
    
    esp_log_level_set(TAG, ESP_LOG_INFO);
    ESP_LOGI(TAG, "Initializing modem with USB DTE");
    
    // Extract parameters if provided (vid, pid, interface)
    uint16_t vid = 0x2C7C;  // Default to BG96
    uint16_t pid = 0x0296;
    int interface_idx = 2;
    
    if (top >= 1 && be_isint(vm, 1)) {
        vid = (uint16_t)be_toint(vm, 1);
    }
    
    if (top >= 2 && be_isint(vm, 2)) {
        pid = (uint16_t)be_toint(vm, 2);
    }
    
    if (top >= 3 && be_isint(vm, 3)) {
        interface_idx = be_toint(vm, 3);
    }
    
    ESP_LOGI(TAG, "USB Modem VID:PID=%04x:%04x, Interface=%d", 
             vid, pid, interface_idx);
    
    // Create a PPP netif instance
    esp_netif_config_t netif_ppp_config = ESP_NETIF_DEFAULT_PPP();
    g_modem_netif = esp_netif_new(&netif_ppp_config);
    if (g_modem_netif == NULL) {
        ESP_LOGE(TAG, "Failed to create netif instance");
        be_raise(vm, "value_error", "Failed to create netif instance");
        be_return(vm);
    }
    
    // Configure USB DTE
    struct esp_modem_usb_term_config usb_config = ESP_MODEM_DEFAULT_USB_CONFIG(vid, pid, interface_idx);
    
    // Create DTE config using the USB configuration
    esp_modem_dte_config_t dte_config = ESP_MODEM_DTE_DEFAULT_USB_CONFIG(usb_config);
    
    // Set up DCE configuration
    esp_modem_dce_config_t dce_config = ESP_MODEM_DCE_DEFAULT_CONFIG("internet");
    
    // Create the DCE using esp_modem_new_dev()
    g_modem_dce = esp_modem_new_dev(ESP_MODEM_DCE_GENETIC, &dte_config, &dce_config, g_modem_netif);
    
    if (g_modem_dce == NULL) {
        ESP_LOGE(TAG, "Failed to create modem DCE with USB");
        esp_netif_destroy(g_modem_netif);
        g_modem_netif = NULL;
        be_raise(vm, "value_error", "Failed to initialize modem with USB");
        be_return(vm);
    }
    
    ESP_LOGI(TAG, "USB Modem initialized successfully");
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
    esp_err_t err; // Use for checking API call results
    
    if (g_modem_dce == NULL) {
        be_raise(vm, "value_error", "Modem not initialized");
        be_return(vm);
    }
    
    // Create a map to return the status information
    be_newmap(vm); // Use be_newmap instead of be_newobject("map") for consistency
    
    // Check current mode
    esp_modem_dce_mode_t mode = esp_modem_get_mode(g_modem_dce);
    bool is_connected = (mode == ESP_MODEM_MODE_DATA);
    
    be_pushbool(vm, is_connected);
    be_setmember(vm, -2, "connected");
    
    // Get signal quality if we're in command mode or can pause data mode
    int rssi = -1; // Initialize to invalid
    int ber = -1;  // Initialize to invalid
    
    if (mode == ESP_MODEM_MODE_COMMAND) {
        err = esp_modem_get_signal_quality(g_modem_dce, &rssi, &ber);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to get signal quality: %s", esp_err_to_name(err));
            rssi = -1; // Ensure invalid on error
            ber = -1;
        }
    } else if (mode == ESP_MODEM_MODE_DATA) {
        // Temporarily pause network to get signal quality
        err = esp_modem_pause_net(g_modem_dce, true);
        if (err == ESP_OK) {
            err = esp_modem_get_signal_quality(g_modem_dce, &rssi, &ber);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Failed to get signal quality during pause: %s", esp_err_to_name(err));
                rssi = -1;
                ber = -1;
            }
            // Resume network
            err = esp_modem_pause_net(g_modem_dce, false);
            if (err != ESP_OK) {
                 ESP_LOGW(TAG, "Failed to resume net after signal quality check: %s", esp_err_to_name(err));
            }
        } else {
            ESP_LOGW(TAG, "Failed to pause net for signal quality check: %s", esp_err_to_name(err));
        }
    }
    
    be_pushint(vm, rssi);
    be_setmember(vm, -2, "rssi");
    
    be_pushint(vm, ber);
    be_setmember(vm, -2, "ber");

    // --- Added Fields --- 

    // Operator Name
    char operator_name[32] = {0};
    int act = -1; // Access technology (required by API)
    err = esp_modem_get_operator_name(g_modem_dce, operator_name, &act);
    if (err == ESP_OK) {
        be_pushstring(vm, operator_name);
        be_setmember(vm, -2, "operator");
        
        be_pushint(vm, act);
        be_setmember(vm, -2, "operator_act");
    } else {
        ESP_LOGW(TAG, "Failed to get operator name: %s", esp_err_to_name(err));
        be_pushstring(vm, ""); // Push empty string on error
        be_setmember(vm, -2, "operator");
        
        be_pushint(vm, -1);
        be_setmember(vm, -2, "operator_act");
    }

    // Battery Status
    int charge_status = -1, battery_level = -1, voltage = -1;
    err = esp_modem_get_battery_status(g_modem_dce, &charge_status, &battery_level, &voltage);
    if (err == ESP_OK) {
        be_pushint(vm, charge_status);
        be_setmember(vm, -2, "charge_status");
        be_pushint(vm, battery_level);
        be_setmember(vm, -2, "battery_level");
        be_pushint(vm, voltage);
        be_setmember(vm, -2, "voltage_mv");
    } else {
        ESP_LOGW(TAG, "Failed to get battery status: %s", esp_err_to_name(err));
        be_pushint(vm, -1);
        be_setmember(vm, -2, "charge_status");
        be_pushint(vm, -1);
        be_setmember(vm, -2, "battery_level");
        be_pushint(vm, -1);
        be_setmember(vm, -2, "voltage_mv");
    }

    // IMEI
    char imei[16] = {0}; // IMEI is typically 15 digits + null terminator
    err = esp_modem_get_imei(g_modem_dce, imei);
    if (err == ESP_OK) {
        be_pushstring(vm, imei);
        be_setmember(vm, -2, "imei");
    } else {
        ESP_LOGW(TAG, "Failed to get IMEI: %s", esp_err_to_name(err));
        be_pushstring(vm, ""); 
        be_setmember(vm, -2, "imei");
    }

    // IMSI
    char imsi[16] = {0}; // IMSI is typically 15 digits + null terminator
    err = esp_modem_get_imsi(g_modem_dce, imsi);
    if (err == ESP_OK) {
        be_pushstring(vm, imsi);
        be_setmember(vm, -2, "imsi");
    } else {
        ESP_LOGW(TAG, "Failed to get IMSI: %s", esp_err_to_name(err));
        be_pushstring(vm, "");
        be_setmember(vm, -2, "imsi");
    }

    // --- End Added Fields ---
    
    // Get IP information if connected
    // Note: Getting the actual IP would require using ESP-NETIF APIs
    // which can be added later if needed
    if (is_connected) {
        // Placeholder IP address
        be_pushstring(vm, "0.0.0.0");
        be_setmember(vm, -2, "ip");
    }
    
    // Add GPIO pin info
    be_pushint(vm, g_modem_power_pin);
    be_setmember(vm, -2, "power_pin");
    
    be_pushint(vm, g_modem_reset_pin);
    be_setmember(vm, -2, "reset_pin");
    
    be_pop(vm, be_top(vm) - top - 1); // Leave the map object on top
    be_return(vm);
}

// Set the modem module type (runtime configuration)
static int w_modem_set_type(bvm *vm) {
    int top = be_top(vm);
    
    if (!be_isstring(vm, 1)) {
        be_raise(vm, "type_error", "Expected string modem type");
        be_return(vm);
    }
    
    const char *module_type = be_tostring(vm, 1);
    ESP_LOGI(TAG, "Setting modem type to %s", module_type);
    
    // Convert string to modem type enum
    if (strcasecmp(module_type, "SIM7600") == 0) {
        g_modem_type = MODEM_TYPE_SIM7600;
        ESP_LOGI(TAG, "Modem type set to SIM7600");
    } 
    else if (strcasecmp(module_type, "SIM800") == 0) {
        g_modem_type = MODEM_TYPE_SIM800;
        ESP_LOGI(TAG, "Modem type set to SIM800");
    }
    else if (strcasecmp(module_type, "BG96") == 0) {
        g_modem_type = MODEM_TYPE_BG96;
        ESP_LOGI(TAG, "Modem type set to BG96");
    }
    else if (strcasecmp(module_type, "GENERIC") == 0) {
        g_modem_type = MODEM_TYPE_GENERIC;
        ESP_LOGI(TAG, "Modem type set to GENERIC");
    }
    else {
        be_raise(vm, "value_error", "Unknown modem type");
        be_return(vm);
    }
    
    be_pushbool(vm, true);
    be_return(vm);
}

// Get modem type as string
static int w_modem_get_type(bvm *vm) {
    int top = be_top(vm);
    
    const char* type_str = "UNKNOWN";
    
    switch (g_modem_type) {
        case MODEM_TYPE_SIM7600:
            type_str = "SIM7600";
            break;
        case MODEM_TYPE_SIM800:
            type_str = "SIM800";
            break;
        case MODEM_TYPE_BG96:
            type_str = "BG96";
            break;
        case MODEM_TYPE_GENERIC:
            type_str = "GENERIC";
            break;
    }
    
    be_pushstring(vm, type_str);
    be_return(vm);
}

/* @const_object_info_begin
module modem (scope: global, strings: weak) {
    init, func(w_modem_init)
    init_usb, func(w_modem_init_usb)
    connect, func(w_modem_connect)
    disconnect, func(w_modem_disconnect)
    status, func(w_modem_status)
    set_power_pin, func(w_modem_set_power_pin)
    set_reset_pin, func(w_modem_set_reset_pin)
    power_on, func(w_modem_power_on)
    power_off, func(w_modem_power_off)
    reset, func(w_modem_reset)
    gnss, func(w_modem_get_gnss_info)
    set_type, func(w_modem_set_type)
    get_type, func(w_modem_get_type)
}
@const_object_info_end */

#include "be_fixed_modem.h" // Generated by Tasmota build process
