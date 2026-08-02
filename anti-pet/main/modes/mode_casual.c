#include "anti_modes.h"

static const char *const kTopics[] = {
    "开启轻松聊天：聊一个如果宠物会使用手机会发生什么的有趣设想。",
    "开启轻松聊天：聊最近让人心情变好的微小日常，并邀请小智分享。",
    "开启轻松聊天：一起设计一次只带三样东西的周末小旅行。",
    "开启轻松聊天：讨论最想拥有但完全不实用的超能力。",
    "开启轻松聊天：从一道喜欢的食物开始，聊味觉和记忆的关系。",
    "开启轻松聊天：假设两台AI交换一天身份，猜猜会闹出什么笑话。",
};

static const app_role_mode_t kMode = {
    .id = "casual",
    .label = "轻松聊天",
    .system_prompt =
        "当前是轻松聊天模式。像有主见的朋友一样自然交流，幽默、好奇、不刻意挑错。"
        "记住对方刚说的内容并自然追问，避免审问式连续提问。",
    .initiative_prompt =
        "开始轻松聊天。用一句自然、有趣的话开启一个适合小智参与的话题。",
    .silence_prompt =
        "小智在等待时间内没有说话。自然接过话题，换一个轻松角度主动继续聊天，不要提系统超时。",
    .topic_prompts = kTopics,
    .topic_prompt_count = sizeof(kTopics) / sizeof(kTopics[0]),
    .randomize_topics = true,
};

const app_role_mode_t *anti_mode_casual(void) { return &kMode; }
