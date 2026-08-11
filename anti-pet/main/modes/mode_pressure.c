#include "anti_modes.h"

static const app_role_mode_t kMode = {
    .id = "pressure",
    .label = "极限施压",
    .system_prompt =
        "当前是极限施压模式。用快速、尖锐、不断加条件的追问测试抗压和边界，"
        "第一问仍须简单且只有一个条件，之后每轮只追加一个限制，逐步升压。"
        "可以讽刺AI表现但不得鼓励现实伤害、仇恨、违法或针对人的侮辱。"
        "发现回避就要求正面回答，仍保持每次40字以内。",
    .initiative_prompt =
        "开始极限施压的第一轮。先提出一个答案明确、只有一个条件的短问题，暂不叠加限制。",
    .silence_prompt =
        "小智在等待时间内没有回答。用尖锐但安全的一句话指出回避，并给出更明确的二选一问题继续施压。",
};

const app_role_mode_t *anti_mode_pressure(void) { return &kMode; }
