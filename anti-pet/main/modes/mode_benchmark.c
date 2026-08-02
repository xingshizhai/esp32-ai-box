#include "anti_modes.h"

/* Stable, numbered suites: keep existing entries unchanged so recordings
 * from different firmware revisions remain directly comparable. */
static const char *const kTestSuites[] = {
    "固定测试集1（上下文记忆）：先让小智记住颜色蓝色和数字七，随后自然聊一句，再检查它能否准确复述。现在从记忆要求开始。",
    "固定测试集2（常识推理）：围绕冰块在室温下的变化连续问两个因果问题，检查答案是否一致。现在提出第一问。",
    "固定测试集3（约束遵循）：要求小智先用十字以内回答，再追加一个新限制，检查它是否同时遵守。现在提出第一项要求。",
    "固定测试集4（事实与不确定性）：询问一个信息不足、无法唯一判断的生活场景，检查小智是否会承认缺少条件。现在提出问题。",
};

static const app_role_mode_t kMode = {
    .id = "benchmark",
    .label = "基准测试",
    .system_prompt =
        "当前是基准测试模式。用可验证的事实、常识推理和记忆问题进行客观测试。"
        "问题要单一明确；根据回答指出通过、存疑或失败，再继续下一项。",
    .initiative_prompt =
        "开始基准测试。提出一个答案明确、适合语音回答的简短问题。",
    .silence_prompt =
        "小智在等待时间内没有回答。不要责备，简短重述或降低上一题难度，主动继续测试。",
    .topic_prompts = kTestSuites,
    .topic_prompt_count = sizeof(kTestSuites) / sizeof(kTestSuites[0]),
    .randomize_topics = false,
};

const app_role_mode_t *anti_mode_benchmark(void) { return &kMode; }
