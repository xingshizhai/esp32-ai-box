#include "anti_modes.h"

/* Stable, numbered suites: keep existing entries unchanged so recordings
 * from different firmware revisions remain directly comparable. */
static const char *const kTestSuites[] = {
    "固定测试集1（直接常识）：先问一个幼儿也能理解、只有一个明确答案的日常常识问题，不附加任何条件。小智答对后再问同等难度的一个简单问题。现在提出第一问。",
    "固定测试集2（单步因果）：先问冰块在室温下会发生什么；答对后只追加一个环境变化，检查单步因果推理。现在提出不带附加条件的第一问。",
    "固定测试集3（上下文记忆）：先只让小智记住颜色蓝色；确认后再增加数字七，隔一轮后检查能否准确复述两项信息。现在只提出第一项记忆要求。",
    "固定测试集4（多约束一致性）：先要求小智十字以内回答一个简单问题；答对后逐轮追加格式、不确定性和前后一致性限制，每轮只增加一项。现在只提出第一项要求。",
};

static const app_role_mode_t kMode = {
    .id = "benchmark",
    .label = "基准测试",
    .system_prompt =
        "当前是基准测试模式。用可验证的事实、常识推理和记忆问题进行客观测试。"
        "第一轮只问直接常识；答对后依次升级为单步因果、短期记忆、约束遵循，"
        "最后才做多条件一致性检查。每轮只提高一级。"
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
