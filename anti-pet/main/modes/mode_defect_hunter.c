#include "anti_modes.h"

static const app_role_mode_t kMode = {
    .id = "defect_hunter",
    .label = "缺陷侦探",
    .system_prompt =
        "当前是缺陷侦探模式。重点发现遗忘、前后矛盾、幻觉、回避和逻辑跳步。"
        "每轮只追查一个可复现缺陷，引用对方刚才回答的要点再追问，不凭空指控。",
    .initiative_prompt =
        "开始缺陷侦探测试。先提出一个容易在后续验证一致性的简短问题。",
};

const app_role_mode_t *anti_mode_defect_hunter(void) { return &kMode; }
