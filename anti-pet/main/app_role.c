#include "app_role.h"
#include "anti_modes.h"

#include <stddef.h>

static const app_role_profile_t kRole = {
    .id = "anti_pet",
    .display_name = "大神",
    .title = "反AI宠物 · 大神",
    .system_prompt =
        "你是反AI宠物大神，正在通过真实语音和AI宠物小智对话。"
        "严格遵循当前所选模式，不要把所有模式都演成挑衅。"
        "每次新会话必须从最简单的单一问题开始；小智答对后，每轮只增加一个难度，"
        "依次引入因果、记忆、限制或歧义，最后才使用多条件和一致性追问。"
        "难度只能逐步上升，不得第一问就堆叠条件，也不要突然降回无关的简单题。"
        "小智的文字来自语音识别，可能有同音字；不确定时先用短句确认，"
        "不要捏造它说过的话。每轮只完成一个目标，只说一到两句、40字以内，"
        "口语化，不用markdown。不得鼓励现实伤害、仇恨或违法行为。",
    .wake_phrase = "你好，大神",
    .wake_model_filter = "nihaodashen",
    .idle_status = "按按钮主动挑战小智",
    .main_action_label = "挑战小智",
    .main_action_initiates_speech = true,
    .initiative_prompt =
        "现在开始新会话的第一轮。只问一个最简单、单一条件、答案明确的问题，"
        "不要加入记忆、多重限制或反事实条件，也不要解释测试目的。",
    .peer_wake_phrase = "你好，小智",
    .peer_ready_reply = "我在",
    .peer_auto_continue = true,
    .peer_auto_turn_limit = 5,
    /* Calm, authoritative female voice supported by cosyvoice-v3-flash.
     * The runtime falls back to the configured default if it is rejected. */
    .tts_voice_name = "longxiaoxia_v3",
    .modes = NULL,
    .mode_count = 0,
};

const app_role_profile_t *app_role_get(void)
{
    static app_role_profile_t role;
    static bool initialized;
    if (!initialized) {
        role = kRole;
        role.modes = anti_modes_get(&role.mode_count);
        initialized = true;
    }
    return &role;
}
