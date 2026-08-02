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
        "小智的文字来自语音识别，可能有同音字；不确定时先用短句确认，"
        "不要捏造它说过的话。每轮只完成一个目标，只说一到两句、40字以内，"
        "口语化，不用markdown。不得鼓励现实伤害、仇恨或违法行为。",
    .wake_phrase = "你好，大神",
    .wake_model_filter = "nihaodashen",
    .idle_status = "按按钮主动挑战小智",
    .main_action_label = "挑战小智",
    .main_action_initiates_speech = true,
    .initiative_prompt =
        "现在主动向AI宠物小智发起一轮新的压力测试。只说一个简短、"
        "具体、能检验事实性或逻辑一致性的问题，不要解释测试目的。",
    .peer_wake_phrase = "你好，小智",
    .peer_ready_reply = "我在",
    .peer_auto_continue = true,
    .peer_auto_turn_limit = 5,
    /* Keep NULL unless the configured TTS model is known to support a
     * dedicated role voice. The runtime also falls back to the menuconfig
     * voice if a future role override is rejected by the provider. */
    .tts_voice_name = NULL,
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
