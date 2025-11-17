#include "config_handler.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include <stdio.h>

#include "my_config.h"

static const char *TAG = "config_rest";
static const char *CONFIG_PATH = "/spiffs/config.json";

void cjson_merge_objects(cJSON *target, const cJSON *patch)
{
    const cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, patch)
    {
        cJSON *existing = cJSON_GetObjectItem(target, entry->string);

        if (cJSON_IsObject(entry) && cJSON_IsObject(existing))
        {
            // Rekursiv mergen
            cjson_merge_objects(existing, entry);
        }
        else
        {
            // Überschreiben oder neu einfügen
            if (existing)
            {
                cJSON_ReplaceItemInObject(target, entry->string, cJSON_Duplicate(entry, 1));
            }
            else
            {
                cJSON_AddItemToObject(target, entry->string, cJSON_Duplicate(entry, 1));
            }
        }
    }
}

// Reads file and returns it as string
char *read_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return NULL;

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *data = malloc(len + 1);
    if (!data)
    {
        fclose(f);
        return NULL;
    }

    fread(data, 1, len, f);
    data[len] = '\0';
    fclose(f);
    return data;
}

// Saves a JSON string to file
esp_err_t save_json(const char *path, const char *json)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return ESP_FAIL;

    fwrite(json, 1, strlen(json), f);
    fclose(f);
    return ESP_OK;
}

// GET / – serves index.html from SPIFFS
esp_err_t root_handler(httpd_req_t *req)
{
    char *data = read_file(CONFIG_PATH);
    
    // Try to serve index.html
    FILE *f = fopen("/spiffs/index.html", "r");
    if (f)
    {
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        fseek(f, 0, SEEK_SET);

        char *html_data = malloc(len + 1);
        if (html_data)
        {
            fread(html_data, 1, len, f);
            html_data[len] = '\0';
            
            httpd_resp_set_type(req, "text/html");
            httpd_resp_send(req, html_data, HTTPD_RESP_USE_STRLEN);
            
            free(html_data);
            fclose(f);
            return ESP_OK;
        }
        fclose(f);
    }

    // Fallback to JSON config
    if (!data)
    {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, data, HTTPD_RESP_USE_STRLEN);
    free(data);
    return ESP_OK;
}

// GET /config – returns current JSON file
esp_err_t get_config_handler(httpd_req_t *req)
{
    char *data = read_file(CONFIG_PATH);
    if (!data)
    {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, data, HTTPD_RESP_USE_STRLEN);
    free(data);
    return ESP_OK;
}

// POST /config – receive and save new JSON configuration
esp_err_t post_config_handler(httpd_req_t *req)
{
    char buffer[2048];
    int total_len = req->content_len;

    if (total_len >= sizeof(buffer))
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON too large");
        return ESP_FAIL;
    }

    int ret = httpd_req_recv(req, buffer, total_len);
    if (ret <= 0)
    {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    buffer[ret] = '\0';

    cJSON *json = cJSON_Parse(buffer);
    if (!json)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON_Delete(json); // Validation ok

    if (save_json(CONFIG_PATH, buffer) != ESP_OK)
    {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_sendstr(req, "OK");

    config_load_from_spiffs(CONFIG_PATH);
    ESP_LOGI(TAG, "Patch erfolgreich angewendet und geladen");

    return ESP_OK;
}

// DEPRECATED: Old SPIFFS-based handlers removed. Use /api/config instead (WiFi NVS-based).

esp_err_t patch_config_handler(httpd_req_t *req)
{
    int total_len = req->content_len;

    if (total_len <= 0 || total_len > 8192) // Arbitrary upper limit for protection
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid length");
        return ESP_FAIL;
    }

    char *buf = malloc(total_len + 1);
    if (!buf)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory error");
        return ESP_FAIL;
    }

    int ret = httpd_req_recv(req, buf, total_len);
    if (ret <= 0)
    {
        free(buf);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    // Load existing configuration
    char *existing_json = read_file(CONFIG_PATH);
    if (!existing_json)
    {
        free(buf);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(existing_json);
    free(existing_json);
    if (!root)
    {
        free(buf);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Existing JSON corrupted");
        return ESP_FAIL;
    }

    cJSON *patch = cJSON_Parse(buf);
    free(buf);
    if (!patch)
    {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid patch");
        return ESP_FAIL;
    }

    // Generisches rekursives Patchen
    cjson_merge_objects(root, patch);
    cJSON_Delete(patch);

    char *updated_json = cJSON_Print(root);
    ESP_LOGD(TAG, "Aktualisierte JSON-Konfiguration:\n%s", updated_json);
    cJSON_Delete(root);

    if (!updated_json)
    {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    if (save_json(CONFIG_PATH, updated_json) != ESP_OK)
    {
        free(updated_json);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    free(updated_json);
    httpd_resp_sendstr(req, "OK");

    config_load_from_spiffs(CONFIG_PATH);
    return ESP_OK;
}

// Initialisiert den HTTP-Server
void start_rest_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.max_uri_handlers = 20;  // Need space for all handlers: root, /config (3), /api/wifi (2), /api/system (2), /api/config (2)

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) == ESP_OK)
    {
        // Root handler for index.html
        httpd_uri_t root_uri = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = root_handler,
            .user_ctx = NULL};
        httpd_register_uri_handler(server, &root_uri);

        httpd_uri_t get_uri = {
            .uri = "/config",
            .method = HTTP_GET,
            .handler = get_config_handler,
            .user_ctx = NULL};

        httpd_uri_t post_uri = {
            .uri = "/config",
            .method = HTTP_POST,
            .handler = post_config_handler,
            .user_ctx = NULL};

        httpd_uri_t patch_uri = {
            .uri = "/config/patch",
            .method = HTTP_POST,
            .handler = patch_config_handler,
            .user_ctx = NULL};
        httpd_register_uri_handler(server, &patch_uri);

        httpd_register_uri_handler(server, &get_uri);
        httpd_register_uri_handler(server, &post_uri);
        
        // Register WiFi REST API handlers
        wifi_rest_register_handlers(server);
        
        ESP_LOGI(TAG, "REST-Schnittstelle bereit auf / und /config und /api/wifi");
    }
}
