#ifndef IDZ3_CONFIG_H
#define IDZ3_CONFIG_H
#include "model.h"

int config_load(const char *path, Config *cfg, int **initial);
int config_interactive(Config *cfg, int **initial);
int config_apply_args(int argc, char **argv, Config *cfg, int **initial, const char **config_path);
void config_defaults(Config *cfg);
int config_parse_int(const char *text, int min, int max, int *out);
void config_usage(const char *program);

#endif
