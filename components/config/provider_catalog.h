#pragma once

#include <stdbool.h>

#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    ai_provider_config_t id;
    const char *name;
    const char *display_name;
    const char *default_base_url;
    const char *default_model;
} provider_descriptor_t;

const provider_descriptor_t *provider_catalog_get(ai_provider_config_t provider);
const char *provider_catalog_name(ai_provider_config_t provider);
const char *provider_catalog_display_name(ai_provider_config_t provider);
bool provider_catalog_parse(const char *name, ai_provider_config_t *provider);
bool provider_catalog_default_profile(ai_provider_config_t provider,
                                      const char **base_url,
                                      const char **model_name);

#ifdef __cplusplus
}
#endif
