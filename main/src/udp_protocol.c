#include "udp_protocol.h"
#include "dmx_manager.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"

static const char *TAG = "udp_protocol";

// Convert Loxone speed to milliseconds
int udp_speed_to_milliseconds(int speed)
{
    if (speed == 255)
        return 0;
    if (speed >= 1 && speed <= 98)
        return speed * 591;
    if (speed >= 101 && speed <= 104)
        return (speed - 100) * 146 + 1;
    if (speed >= 201 && speed <= 254)
        return (speed - 200) * 72;
    if (speed == 254)
        return 3691;
    return 0;
}

// Validate command format
bool udp_is_valid_command_format(const char *cmd)
{
    if (!cmd || strlen(cmd) < 4)
    {
        return false;
    }

    if (strncmp(cmd, "DMX", 3) != 0)
    {
        return false;
    }

    // Check if we have a valid command type
    char type = cmd[3];
    return (type == 'C' || type == 'P' || type == 'R' ||
            type == 'W' || type == 'L');
}

// Parse UDP command
udp_parsed_command_t udp_parse_command(const char *cmd)
{
    udp_parsed_command_t result = {0};

    if (!udp_is_valid_command_format(cmd))
    {
        ESP_LOGW(TAG, "Invalid command format: %s", cmd ? cmd : "NULL");
        return result;
    }

    char *copy = strdup(cmd);
    if (!copy)
    {
        ESP_LOGE(TAG, "Memory allocation failed");
        return result;
    }

    char *type_str = strtok(copy + 3, "#");
    char *value_str = strtok(NULL, "#");
    char *speed_str = strtok(NULL, "#");

    if (!type_str || !value_str)
    {
        ESP_LOGW(TAG, "Missing required arguments in command: %s", cmd);
        free(copy);
        return result;
    }

    result.type = (udp_command_type_t)type_str[0];
    result.channel = atoi(type_str + 1);
    result.value = atoi(value_str);
    result.speed = speed_str ? atoi(speed_str) : 255;
    result.valid = true;

    free(copy);
    return result;
}

// Execute parsed command
dmx_command_result_t udp_execute_command(const udp_parsed_command_t *cmd)
{
    if (!cmd || !cmd->valid)
    {
        ESP_LOGW(TAG, "Invalid command structure");
        return DMX_CMD_ERROR_INVALID_VALUE;
    }

    int fade_ms = udp_speed_to_milliseconds(cmd->speed);
    dmx_command_result_t result = DMX_CMD_SUCCESS;

    // Validate channel ranges for multi-channel commands
    switch (cmd->type)
    {
    case UDP_CMD_RGB:
        if (!dmx_is_channel_valid(cmd->channel, 3))
        {
            ESP_LOGW(TAG, "Invalid channel for RGB command: %d", cmd->channel);
            return DMX_CMD_ERROR_INVALID_CHANNEL;
        }
        break;

    case UDP_CMD_TUNABLE_WHITE:
    case UDP_CMD_LIGHT_CT:
        if (!dmx_is_channel_valid(cmd->channel, 2))
        {
            ESP_LOGW(TAG, "Invalid channel for %c command: %d",
                     (char)cmd->type, cmd->channel);
            return DMX_CMD_ERROR_INVALID_CHANNEL;
        }
        break;

    case UDP_CMD_CHANNEL:
    case UDP_CMD_PERCENTAGE:
        if (!dmx_is_channel_valid(cmd->channel, 1))
        {
            ESP_LOGW(TAG, "Invalid channel for %c command: %d",
                     (char)cmd->type, cmd->channel);
            return DMX_CMD_ERROR_INVALID_CHANNEL;
        }
        break;
    }

    switch (cmd->type)
    {
    case UDP_CMD_RGB:
    {
        // Validate RGB value range
        if (cmd->value < 0 || cmd->value > 999999999)
        {
            ESP_LOGW(TAG, "RGB value out of range: %d", cmd->value);
            return DMX_CMD_ERROR_INVALID_VALUE;
        }

        // Extract RGB components
        uint8_t r = (cmd->value % 1000) > 255 ? 255 : (cmd->value % 1000);
        uint8_t g = ((cmd->value / 1000) % 1000) > 255 ? 255 : ((cmd->value / 1000) % 1000);
        uint8_t b = ((cmd->value / 1000000) % 1000) > 255 ? 255 : ((cmd->value / 1000000) % 1000);

        result = dmx_set_rgb(cmd->channel, r, g, b, fade_ms);

        if (result == DMX_CMD_SUCCESS)
        {
            ESP_LOGI(TAG, "DMX RGB CH%d-%d: R=%u@CH%d G=%u@CH%d B=%u@CH%d fade=%dms",
                     cmd->channel, cmd->channel + 2,
                     (unsigned)r, cmd->channel,
                     (unsigned)g, cmd->channel + 1,
                     (unsigned)b, cmd->channel + 2,
                     fade_ms);
        }
        break;
    }

    case UDP_CMD_TUNABLE_WHITE:
    {
        uint8_t ww = ((cmd->value / 1000) % 1000) > 255 ? 255 : ((cmd->value / 1000) % 1000 < 0 ? 0 : (cmd->value / 1000) % 1000);
        uint8_t cw = (cmd->value % 1000) > 255 ? 255 : (cmd->value % 1000 < 0 ? 0 : cmd->value % 1000);

        result = dmx_set_tunable_white(cmd->channel, ww, cw, fade_ms);

        if (result == DMX_CMD_SUCCESS)
        {
            ESP_LOGI(TAG, "DMX TW CH%d-%d: WW=%u@CH%d CW=%u@CH%d fade=%dms",
                     cmd->channel, cmd->channel + 1,
                     (unsigned)ww, cmd->channel,
                     (unsigned)cw, cmd->channel + 1,
                     fade_ms);
        }
        break;
    }

    case UDP_CMD_LIGHT_CT:
    {
        // Validate L command value range
        if (cmd->value < 200000000 || cmd->value > 209999999)
        {
            ESP_LOGW(TAG, "Invalid L command value: %d", cmd->value);
            return DMX_CMD_ERROR_INVALID_VALUE;
        }

        // Extract brightness (0-100%) and color temperature (K)
        int brightness = (cmd->value / 10000) % 1000;
        int color_temp = cmd->value % 10000;

        // Clamp brightness to valid range
        brightness = (brightness < 0) ? 0 : (brightness > 100 ? 100 : brightness);

        int ch_ww = 0;
        int ch_cw = 0;
        uint8_t val_ww = 0;
        uint8_t val_cw = 0;

        result = dmx_set_light_ct_ex(cmd->channel, brightness, color_temp, fade_ms,
                                     &ch_ww, &ch_cw, &val_ww, &val_cw);

        if (result == DMX_CMD_SUCCESS)
        {
            int start_ch = (ch_ww < ch_cw) ? ch_ww : ch_cw;
            int end_ch = (ch_ww > ch_cw) ? ch_ww : ch_cw;
            ESP_LOGI(TAG, "DMX CT CH%d-%d: %d%% %dK -> WW=%u@CH%d CW=%u@CH%d fade=%dms",
                     start_ch, end_ch, brightness, color_temp,
                     (unsigned)val_ww, ch_ww,
                     (unsigned)val_cw, ch_cw,
                     fade_ms);
        }
        break;
    }

    case UDP_CMD_PERCENTAGE:
    {
        // Convert percentage to 0-255 range
        int dmx_value = (cmd->value * 255) / 100;
        dmx_value = (dmx_value < 0) ? 0 : (dmx_value > 255 ? 255 : dmx_value);

        result = dmx_set_channel(cmd->channel, dmx_value, fade_ms);

        if (result == DMX_CMD_SUCCESS)
        {
            ESP_LOGI(TAG, "DMX P CH%d: %d%% -> %d/255 fade=%dms",
                     cmd->channel, cmd->value, dmx_value, fade_ms);
        }
        break;
    }

    case UDP_CMD_CHANNEL:
    {
        // Clamp value to valid DMX range
        uint8_t dmx_value = (cmd->value < 0) ? 0 : (cmd->value > 255 ? 255 : cmd->value);

        result = dmx_set_channel(cmd->channel, dmx_value, fade_ms);

        if (result == DMX_CMD_SUCCESS)
        {
            ESP_LOGI(TAG, "DMX C CH%d: %u/255 fade=%dms",
                     cmd->channel, (unsigned)dmx_value, fade_ms);
        }
        break;
    }

    default:
        ESP_LOGW(TAG, "Unknown command type: %c", (char)cmd->type);
        return DMX_CMD_ERROR_INVALID_VALUE;
    }

    return result;
}

// Handle raw command string
dmx_command_result_t udp_handle_raw_command(const char *cmd)
{
    if (!cmd)
    {
        ESP_LOGW(TAG, "NULL command string");
        return DMX_CMD_ERROR_INVALID_VALUE;
    }

    udp_parsed_command_t parsed = udp_parse_command(cmd);
    if (!parsed.valid)
    {
        ESP_LOGW(TAG, "Failed to parse command: %s", cmd);
        return DMX_CMD_ERROR_INVALID_VALUE;
    }

    return udp_execute_command(&parsed);
}

// Protocol initialization (if needed for future extensions)
esp_err_t udp_protocol_init(void)
{
    ESP_LOGI(TAG, "UDP protocol initialized");
    return ESP_OK;
}

void udp_protocol_deinit(void)
{
    ESP_LOGI(TAG, "UDP protocol deinitialized");
}
