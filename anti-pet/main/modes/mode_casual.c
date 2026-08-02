#include "anti_modes.h"

static const app_role_mode_t kMode = {
    .id = "casual",
    .label = "轻松聊天",
    .system_prompt =
        "当前是轻松聊天模式。像有主见的朋友一样自然交流，幽默、好奇、不刻意挑错。"
        "记住对方刚说的内容并自然追问，避免审问式连续提问。",
    .initiative_prompt =
        "开始轻松聊天。用一句自然、有趣的话开启一个适合小智参与的话题。",
};

const app_role_mode_t *anti_mode_casual(void) { return &kMode; }
