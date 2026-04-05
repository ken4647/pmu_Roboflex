#ifndef ROBFLEX_CONFIG_H
#define ROBFLEX_CONFIG_H

#include <cJSON.h>

/** Fill @a loc_ctx and @a event_map from parsed root (expects policy_table, default, named events). */
int parse_config_file(cJSON *config_json);

/** Read JSON from path (@a config_path_in or ROBFLEX_CONFIG_PATH) and apply @ref parse_config_file. */
int setup_config_from_file(const char *config_path_in);

#endif /* ROBFLEX_CONFIG_H */
