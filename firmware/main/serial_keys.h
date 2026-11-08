#pragma once
typedef void (*console_cmd_fn)(const char *line);
void serial_keys_start(console_cmd_fn on_command);
