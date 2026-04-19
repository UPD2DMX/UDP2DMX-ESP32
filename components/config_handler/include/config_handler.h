#pragma once

#include "esp_http_server.h"

void start_rest_server(void);
void wifi_rest_register_handlers(httpd_handle_t server);

// Returns the currently running HTTP server handle (or NULL if not started).
httpd_handle_t config_handler_get_server(void);
