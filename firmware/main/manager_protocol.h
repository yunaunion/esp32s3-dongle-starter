#pragma once

#include <stddef.h>

#include "cJSON.h"
#include "esp_err.h"

typedef esp_err_t (*manager_protocol_writer_t)(const char *data, size_t length);

void manager_protocol_init(void);
void manager_protocol_set_writer(manager_protocol_writer_t writer);
void manager_protocol_handle_line(const char *line);
void manager_protocol_emit_event(const char *event, cJSON *data);
cJSON *manager_protocol_status_json(void);
