#include "vc_joins.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

using namespace std;

namespace simplify
{
    namespace
    {
        string code(string text)
        {
            // Keep user-supplied text inside one inline-code span.
            for (char& c : text)
                if (c == '`' || c == '\n' || c == '\r') c = ' ';
            return "`" + text + "`";
        }
    }

    vc_joins::presence_snapshot vc_joins::describe(const dpp::presence& presence)
    {
        const char* label = "OFFLINE";
        switch (presence.status())
        {
            case dpp::ps_online:
                label = presence.mobile_status() == dpp::ps_online ? "ONLINE/MOBILE" : "ONLINE";
                break;
            case dpp::ps_idle: label = "IDLE"; break;
            case dpp::ps_dnd: label = "DND"; break;
            default: break;
        }
        presence_snapshot result{string("CHANGE STATUS TO ") + code(label), ""};
        vector<string> details;
        if (presence.status() != dpp::ps_offline && presence.status() != dpp::ps_invisible)
        {
            for (const auto& activity : presence.activities)
            {
                if (activity.type == dpp::at_custom)
                {
                    if (!activity.state.empty()) details.push_back(activity.state);
                    continue;
                }
                const char* action = "Playing ";
                switch (activity.type)
                {
                    case dpp::at_streaming: action = "Streaming "; break;
                    case dpp::at_listening: action = "Listening to "; break;
                    case dpp::at_watching: action = "Watching "; break;
                    case dpp::at_competing: action = "Competing in "; break;
                    default: break;
                }
                string detail = string(action) + activity.name;
                if (!activity.details.empty()) detail += " - " + activity.details;
                if (!activity.state.empty()) detail += " - " + activity.state;
                details.push_back(detail);
                if (activity.type == dpp::at_streaming && result.stream_title.empty())
                    result.stream_title = activity.details.empty() ? activity.name : activity.details;
            }
        }
        // Activity ordering and ticking timestamps don't represent a meaningful status change.
        ranges::sort(details);
        details.erase(unique(details.begin(), details.end()), details.end());
        bool first = true;
        for (const auto& detail : details)
        {
            const string suffix = string(first ? ": " : " | ") + code(detail);
            if (result.text.size() + suffix.size() > 1800) break; // Leave space for the timestamp.
            result.text += suffix;
            first = false;
        }
        return result;
    }

    vc_joins::vc_joins(dpp::cluster& bot, webhook_queue& queue, dpp::webhook hook)
        : queue(queue), hook(std::move(hook))
    {
        bot.on_guild_create([this](const dpp::guild_create_t& ev)
        {
            lock_guard lk(mu);
            auto& members = channels[ev.created.id];
            members.clear();
            for (const auto& [user, state] : ev.created.voice_members)
                if (state.channel_id) members[user] = state;
            auto& presence = statuses[ev.created.id];
            presence.clear();
            for (const auto& [user, state] : ev.presences)
                presence[user] = describe(state);
        });
        bot.on_guild_delete([this](const dpp::guild_delete_t& ev)
        {
            lock_guard lk(mu);
            channels.erase(ev.guild_id);
            statuses.erase(ev.guild_id);
        });
        bot.on_voice_state_update([this](const dpp::voice_state_update_t& ev) { on_update(ev); });
        bot.on_presence_update([this](const dpp::presence_update_t& ev) { on_presence(ev); });
    }

    void vc_joins::on_update(const dpp::voice_state_update_t& ev)
    {
        const auto& state = ev.state;
        if (!state.guild_id || !state.user_id) return;

        lock_guard lk(mu);
        auto& members = channels[state.guild_id];
        const auto previous = members.find(state.user_id);
        const dpp::voicestate old_state = previous == members.end() ? dpp::voicestate{} : previous->second;
        const auto old_channel = old_state.channel_id;

        if (state.channel_id) members[state.user_id] = state;
        else members.erase(state.user_id);

        const auto timestamp = time(nullptr);
        if (old_channel && old_channel != state.channel_id)
            send(state.guild_id, state.user_id, "LEFT VOICE CHANNEL <#" + to_string(old_channel) + ">", timestamp);
        if (state.channel_id && old_channel != state.channel_id)
            send(state.guild_id, state.user_id, "JOINED VOICE CHANNEL <#" + to_string(state.channel_id) + ">", timestamp);

        if (!state.channel_id) return; // Disconnects are reported as LEFT, not synthetic toggle changes.
        const string location = " @ <#" + to_string(state.channel_id) + ">";
        const auto report = [&](const string& text) { send(state.guild_id, state.user_id, text + location, timestamp, true); };
        if (state.self_stream() && !old_state.self_stream())
        {
            const auto& presence = statuses[state.guild_id];
            const auto it = presence.find(state.user_id);
            const string title = it != presence.end() && !it->second.stream_title.empty()
                ? it->second.stream_title : "Go Live";
            report("LIVE ON " + code(title));
        }
        if (state.self_video() != old_state.self_video())
            report(state.self_video() ? "TURNED ON CAMERA" : "TURNED OFF CAMERA");
        const bool deaf = state.is_deaf() || state.is_self_deaf();
        const bool old_deaf = old_state.is_deaf() || old_state.is_self_deaf();
        if (deaf != old_deaf) report(deaf ? "DEAFENED" : "UNDEAFENED");
        const bool mute = state.is_mute() || state.is_self_mute();
        const bool old_mute = old_state.is_mute() || old_state.is_self_mute();
        if (mute != old_mute) report(mute ? "MUTED" : "UNMUTED");
    }

    void vc_joins::on_presence(const dpp::presence_update_t& ev)
    {
        const auto& presence = ev.rich_presence;
        if (!presence.guild_id || !presence.user_id) return;

        const auto status = describe(presence);

        lock_guard lk(mu);
        auto& members = statuses[presence.guild_id];
        const auto previous = members.find(presence.user_id);
        // Initial guild presences omit offline members. Treat an unlisted member as offline.
        const string old_text = previous == members.end() ? "CHANGE STATUS TO `OFFLINE`" : previous->second.text;
        members[presence.user_id] = status;
        if (old_text != status.text)
            send(presence.guild_id, presence.user_id, status.text, time(nullptr));
    }

    void vc_joins::send(dpp::snowflake guild, dpp::snowflake user_id, const string& text,
                        time_t timestamp, bool voice_detail) const
    {
        dpp::webhook w = hook;
        w.name = to_string(user_id);
        w.avatar_url.clear();
        if (const auto* user = dpp::find_user(user_id))
        {
            if (user->is_bot()) return; // Match the message forwarder's human-only behavior.
            w.name = user->global_name.empty() ? user->username : user->global_name;
            w.avatar_url = user->get_avatar_url();
        }
        try
        {
            const auto member = dpp::find_guild_member(guild, user_id);
            if (!member.get_nickname().empty()) w.name = member.get_nickname();
            const auto avatar = member.get_avatar_url();
            if (!avatar.empty()) w.avatar_url = avatar;
        }
        catch (const dpp::cache_exception&)
        {
            // Fall back to the user's global identity (or ID) if the member isn't cached.
        }

        // Discord's long date/time format, rendered in each viewer's local timezone.
        dpp::message msg(text + (voice_detail ? ", <t:" : " @ <t:") + to_string(timestamp) + ":F>");
        msg.set_allowed_mentions(false, false, false, false);
        queue.post(w, msg);
    }
}
