#include "provider_catalog.h"

#include <ctype.h>
#include <stddef.h>
#include <string.h>

#include "sdkconfig.h"

#ifndef CONFIG_OPENAI_BASE_URL_DEFAULT
#define CONFIG_OPENAI_BASE_URL_DEFAULT "https://api.openai.com/v1/chat/completions"
#endif
#ifndef CONFIG_OPENAI_MODEL_DEFAULT
#define CONFIG_OPENAI_MODEL_DEFAULT "gpt-4o-mini"
#endif
#ifndef CONFIG_ZHIPU_BASE_URL_DEFAULT
#define CONFIG_ZHIPU_BASE_URL_DEFAULT "https://open.bigmodel.cn/api/paas/v4/chat/completions"
#endif
#ifndef CONFIG_ZHIPU_MODEL_DEFAULT
#define CONFIG_ZHIPU_MODEL_DEFAULT "glm-4-flash"
#endif
#ifndef CONFIG_DEEPSEEK_BASE_URL_DEFAULT
#define CONFIG_DEEPSEEK_BASE_URL_DEFAULT "https://api.deepseek.com/v1/chat/completions"
#endif
#ifndef CONFIG_DEEPSEEK_MODEL_DEFAULT
#define CONFIG_DEEPSEEK_MODEL_DEFAULT "deepseek-chat"
#endif
#ifndef CONFIG_KIMI_BASE_URL_DEFAULT
#define CONFIG_KIMI_BASE_URL_DEFAULT "https://api.moonshot.cn/v1/chat/completions"
#endif
#ifndef CONFIG_KIMI_MODEL_DEFAULT
#define CONFIG_KIMI_MODEL_DEFAULT "moonshot-v1-8k"
#endif
#ifndef CONFIG_MINIMAX_BASE_URL_DEFAULT
#define CONFIG_MINIMAX_BASE_URL_DEFAULT "https://api.minimax.chat/v1/chat/completions"
#endif
#ifndef CONFIG_MINIMAX_MODEL_DEFAULT
#define CONFIG_MINIMAX_MODEL_DEFAULT "MiniMax-Text-01"
#endif
#ifndef CONFIG_OPENROUTER_BASE_URL_DEFAULT
#define CONFIG_OPENROUTER_BASE_URL_DEFAULT "https://openrouter.ai/api/v1/chat/completions"
#endif
#ifndef CONFIG_OPENROUTER_MODEL_DEFAULT
#define CONFIG_OPENROUTER_MODEL_DEFAULT "deepseek/deepseek-chat-v3-0324:free"
#endif

static bool provider_name_equals(const char *left, const char *right)
{
    if (left == NULL || right == NULL) {
        return false;
    }

    while (*left != '\0' && *right != '\0') {
        if (tolower((unsigned char)*left) != tolower((unsigned char)*right)) {
            return false;
        }
        ++left;
        ++right;
    }
    return *left == '\0' && *right == '\0';
}

static const provider_descriptor_t s_providers[] = {
    {
        .id = AI_PROVIDER_CONFIG_OPENAI,
        .name = "openai",
        .display_name = "OpenAI",
        .default_base_url = CONFIG_OPENAI_BASE_URL_DEFAULT,
        .default_model = CONFIG_OPENAI_MODEL_DEFAULT,
    },
    {
        .id = AI_PROVIDER_CONFIG_ZHIPU,
        .name = "zhipu",
        .display_name = "Zhipu AI",
        .default_base_url = CONFIG_ZHIPU_BASE_URL_DEFAULT,
        .default_model = CONFIG_ZHIPU_MODEL_DEFAULT,
    },
    {
        .id = AI_PROVIDER_CONFIG_DEEPSEEK,
        .name = "deepseek",
        .display_name = "DeepSeek",
        .default_base_url = CONFIG_DEEPSEEK_BASE_URL_DEFAULT,
        .default_model = CONFIG_DEEPSEEK_MODEL_DEFAULT,
    },
    {
        .id = AI_PROVIDER_CONFIG_KIMI,
        .name = "kimi",
        .display_name = "Kimi",
        .default_base_url = CONFIG_KIMI_BASE_URL_DEFAULT,
        .default_model = CONFIG_KIMI_MODEL_DEFAULT,
    },
    {
        .id = AI_PROVIDER_CONFIG_MINIMAX,
        .name = "minimax",
        .display_name = "MiniMax",
        .default_base_url = CONFIG_MINIMAX_BASE_URL_DEFAULT,
        .default_model = CONFIG_MINIMAX_MODEL_DEFAULT,
    },
    {
        .id = AI_PROVIDER_CONFIG_OPENROUTER,
        .name = "openrouter",
        .display_name = "OpenRouter",
        .default_base_url = CONFIG_OPENROUTER_BASE_URL_DEFAULT,
        .default_model = CONFIG_OPENROUTER_MODEL_DEFAULT,
    },
};

const provider_descriptor_t *provider_catalog_get(ai_provider_config_t provider)
{
    for (size_t i = 0; i < sizeof(s_providers) / sizeof(s_providers[0]); ++i) {
        if (s_providers[i].id == provider) {
            return &s_providers[i];
        }
    }
    return NULL;
}

const char *provider_catalog_name(ai_provider_config_t provider)
{
    const provider_descriptor_t *descriptor = provider_catalog_get(provider);
    return descriptor != NULL ? descriptor->name : "unknown";
}

const char *provider_catalog_display_name(ai_provider_config_t provider)
{
    const provider_descriptor_t *descriptor = provider_catalog_get(provider);
    return descriptor != NULL ? descriptor->display_name : "Unknown";
}

bool provider_catalog_parse(const char *name, ai_provider_config_t *provider)
{
    if (name == NULL || provider == NULL) {
        return false;
    }

    for (size_t i = 0; i < sizeof(s_providers) / sizeof(s_providers[0]); ++i) {
        if (provider_name_equals(name, s_providers[i].name)) {
            *provider = s_providers[i].id;
            return true;
        }
    }

    if (provider_name_equals(name, "glm")) {
        *provider = AI_PROVIDER_CONFIG_ZHIPU;
        return true;
    }
    if (provider_name_equals(name, "moonshot")) {
        *provider = AI_PROVIDER_CONFIG_KIMI;
        return true;
    }
    return false;
}

bool provider_catalog_default_profile(ai_provider_config_t provider,
                                      const char **base_url,
                                      const char **model_name)
{
    if (base_url == NULL || model_name == NULL) {
        return false;
    }

    const provider_descriptor_t *descriptor = provider_catalog_get(provider);
    if (descriptor == NULL) {
        return false;
    }

    *base_url = descriptor->default_base_url;
    *model_name = descriptor->default_model;
    return true;
}
