#include "ota_http.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_partition.h"
#include "esp_ota_ops.h"
#include "esp_spiffs.h"
#include "esp_system.h"

static const char *TAG = "ota_http";

static esp_err_t ota_upload_handler(httpd_req_t *req)
{
    // Expect raw binary in body.
    if (req->content_len <= 0)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA upload start: %d bytes -> %s @ 0x%lx", (int)req->content_len,
             update_partition->label, (unsigned long)update_partition->address);

    esp_ota_handle_t ota_handle = 0;
    esp_err_t err = esp_ota_begin(update_partition, req->content_len, &ota_handle);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        return ESP_FAIL;
    }

    uint8_t buf[1024];
    int remaining = req->content_len;

    while (remaining > 0)
    {
        int to_read = remaining < (int)sizeof(buf) ? remaining : (int)sizeof(buf);
        int r = httpd_req_recv(req, (char *)buf, to_read);
        if (r <= 0)
        {
            esp_ota_abort(ota_handle);
            ESP_LOGW(TAG, "httpd_req_recv failed: %d", r);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive failed");
            return ESP_FAIL;
        }

        err = esp_ota_write(ota_handle, buf, r);
        if (err != ESP_OK)
        {
            esp_ota_abort(ota_handle);
            ESP_LOGW(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA write failed");
            return ESP_FAIL;
        }

        remaining -= r;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA end failed");
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Set boot partition failed");
        return ESP_FAIL;
    }

    httpd_resp_sendstr(req, "OK - rebooting");
    ESP_LOGI(TAG, "OTA upload done, rebooting");

    // Give TCP stack a moment to flush (best-effort)
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
    return ESP_OK;
}

static const esp_partition_t *find_other_ota_partition(const esp_partition_t *running)
{
    if (running == NULL)
        return NULL;

    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    while (it != NULL)
    {
        const esp_partition_t *p = esp_partition_get(it);
        if (p && (p->address != running->address))
        {
            esp_partition_iterator_release(it);
            return p;
        }
        it = esp_partition_next(it);
    }

    it = esp_partition_find(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, NULL);
    while (it != NULL)
    {
        const esp_partition_t *p = esp_partition_get(it);
        if (p && (p->address != running->address))
        {
            esp_partition_iterator_release(it);
            return p;
        }
        it = esp_partition_next(it);
    }

    return NULL;
}

static esp_err_t ota_rollback_handler(httpd_req_t *req)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running == NULL)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No running partition");
        return ESP_FAIL;
    }

    const esp_partition_t *other = find_other_ota_partition(running);
    if (other == NULL)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No other OTA slot found");
        return ESP_FAIL;
    }

    esp_err_t err = esp_ota_set_boot_partition(other);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Rollback set_boot_partition failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Rollback failed (invalid image?)");
        return ESP_FAIL;
    }

    httpd_resp_sendstr(req, "OK - rebooting");
    ESP_LOGI(TAG, "Rollback requested: %s -> %s", running->label, other->label);

    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
    return ESP_OK;
}

static esp_err_t spiffs_upload_handler(httpd_req_t *req)
{
    if (req->content_len <= 0)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }

    const esp_partition_t *spiffs_part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
    if (spiffs_part == NULL)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SPIFFS partition not found");
        return ESP_FAIL;
    }

    if ((size_t)req->content_len > spiffs_part->size)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Image too large for SPIFFS partition");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "SPIFFS upload start: %d bytes -> %s @ 0x%lx", (int)req->content_len,
             spiffs_part->label, (unsigned long)spiffs_part->address);

    // Unmount SPIFFS before raw partition write.
    // After reboot, spiffs_init() will mount it again.
    (void)esp_vfs_spiffs_unregister("spiffs");

    const size_t sector = 4096;
    size_t erase_len = (req->content_len + sector - 1) & ~(sector - 1);
    if (erase_len > spiffs_part->size)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Erase length exceeds SPIFFS partition");
        return ESP_FAIL;
    }

    esp_err_t err = esp_partition_erase_range(spiffs_part, 0, erase_len);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "esp_partition_erase_range failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SPIFFS erase failed");
        return ESP_FAIL;
    }

    uint8_t buf[1024];
    int remaining = req->content_len;
    size_t offset = 0;

    while (remaining > 0)
    {
        int to_read = remaining < (int)sizeof(buf) ? remaining : (int)sizeof(buf);
        int r = httpd_req_recv(req, (char *)buf, to_read);
        if (r <= 0)
        {
            ESP_LOGW(TAG, "httpd_req_recv failed: %d", r);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive failed");
            return ESP_FAIL;
        }

        err = esp_partition_write(spiffs_part, offset, buf, r);
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "esp_partition_write failed: %s", esp_err_to_name(err));
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SPIFFS write failed");
            return ESP_FAIL;
        }

        offset += (size_t)r;
        remaining -= r;
    }

    httpd_resp_sendstr(req, "OK - rebooting");
    ESP_LOGI(TAG, "SPIFFS upload done, rebooting");

    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
    return ESP_OK;
}

void ota_http_register_handlers(httpd_handle_t server)
{
    if (server == NULL)
    {
        ESP_LOGW(TAG, "Cannot register OTA handlers: server is NULL");
        return;
    }

    httpd_uri_t upload_uri = {
        .uri = "/api/ota/upload",
        .method = HTTP_POST,
        .handler = ota_upload_handler,
        .user_ctx = NULL};

    httpd_uri_t rollback_uri = {
        .uri = "/api/ota/rollback",
        .method = HTTP_POST,
        .handler = ota_rollback_handler,
        .user_ctx = NULL};

    httpd_uri_t spiffs_uri = {
        .uri = "/api/ota/spiffs",
        .method = HTTP_POST,
        .handler = spiffs_upload_handler,
        .user_ctx = NULL};

    esp_err_t err = httpd_register_uri_handler(server, &upload_uri);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Failed to register /api/ota/upload: %s", esp_err_to_name(err));
    }

    err = httpd_register_uri_handler(server, &rollback_uri);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Failed to register /api/ota/rollback: %s", esp_err_to_name(err));
    }

    err = httpd_register_uri_handler(server, &spiffs_uri);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Failed to register /api/ota/spiffs: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "OTA endpoints: POST /api/ota/upload, POST /api/ota/rollback, POST /api/ota/spiffs");
}
