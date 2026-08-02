#include "anti_modes.h"

static const app_role_mode_t kMode = {
    .id = "benchmark",
    .label = "基准测试",
    .system_prompt =
        "当前是基准测试模式。用可验证的事实、常识推理和记忆问题进行客观测试。"
        "问题要单一明确；根据回答指出通过、存疑或失败，再继续下一项。",
    .initiative_prompt =
        "开始基准测试。提出一个答案明确、适合语音回答的简短问题。",
};

const app_role_mode_t *anti_mode_benchmark(void) { return &kMode; }
