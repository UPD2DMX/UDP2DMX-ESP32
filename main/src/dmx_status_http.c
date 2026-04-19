#include "dmx_status_http.h"

#include "dmx_manager.h"

#include <stdio.h>
#include <inttypes.h>

#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "dmx_status_http";

static esp_err_t dmx_status_page_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");

    httpd_resp_sendstr_chunk(req,
                            "<!doctype html><html><head><meta charset='utf-8'>"
                            "<meta name='viewport' content='width=device-width, initial-scale=1'>"
                            "<title>DMX Status</title></head><body>"
                            "<h1>DMX Status</h1>");

    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);
    const uint32_t any_last = dmx_get_last_change_ms();

    char line[160];
    if (any_last == 0)
    {
        snprintf(line, sizeof(line), "<p>Letzte Änderung (global): -</p>");
    }
    else
    {
        snprintf(line, sizeof(line), "<p>Letzte Änderung (global): %" PRIu32 " ms seit Boot (vor %" PRIu32 " ms)</p>", any_last, (uint32_t)(now - any_last));
    }
    httpd_resp_sendstr_chunk(req, line);

    httpd_resp_sendstr_chunk(req,
                            "<p>Anzeige: nur Kanäle, die seit Boot geändert wurden.</p>"
                            "<p>Format: Kanal: Wert (0-255), letzte Änderung (ms seit Boot / ms her)</p>"
                            "<pre>");

    int changed_count = 0;
    for (int ch = 1; ch <= DMX_UNIVERSE_SIZE; ch++)
    {
        const uint32_t last = dmx_get_channel_last_change_ms(ch);
        if (last == 0)
        {
            continue;
        }

        const uint8_t value = dmx_get_channel_value(ch);
        snprintf(line, sizeof(line), "%03d: %3u  last: %" PRIu32 " ms (vor %" PRIu32 " ms)\n", ch, (unsigned)value, last, (uint32_t)(now - last));
        httpd_resp_sendstr_chunk(req, line);
        changed_count++;
    }

    if (changed_count == 0)
    {
        httpd_resp_sendstr_chunk(req, "(keine Kanaländerungen seit Boot)\n");
    }

    httpd_resp_sendstr_chunk(req, "</pre></body></html>");
    return httpd_resp_sendstr_chunk(req, NULL);
}

void dmx_status_http_register_handlers(httpd_handle_t server)
{
    if (server == NULL)
    {
        ESP_LOGW(TAG, "Cannot register DMX status handler: server is NULL");
        return;
    }

    httpd_uri_t status_uri = {
        .uri = "/dmx/status",
        .method = HTTP_GET,
        .handler = dmx_status_page_handler,
        .user_ctx = NULL};

    esp_err_t err = httpd_register_uri_handler(server, &status_uri);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Failed to register /dmx/status: %s", esp_err_to_name(err));
    }
    else
    {
        ESP_LOGI(TAG, "DMX status page available at /dmx/status");
    }
}
