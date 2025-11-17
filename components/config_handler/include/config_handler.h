#pragma once

#include "esp_http_server.h"

void start_rest_server(void);
void wifi_rest_register_handlers(httpd_handle_t server);
