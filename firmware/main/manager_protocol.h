#pragma once

#include "cJSON.h"

void manager_protocol_init(void);
void manager_protocol_handle_line(const char *line);
void manager_protocol_emit_event(const char *event, cJSON *data);
cJSON *manager_protocol_status_json(void);
