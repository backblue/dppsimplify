#ifndef DPPSIMPLIFY_VC_JOINS_H
#define DPPSIMPLIFY_VC_JOINS_H

#include <dpp/dpp.h>
#include <chrono>
#include <ctime>
#include <deque>
#include <mutex>
#include <unordered_map>

#include "webhook_queue.h"

namespace simplify
{
    // Reports voice channel and presence transitions using the member's display name and avatar.
    class vc_joins
    {
    public:
        vc_joins(dpp::cluster& bot, webhook_queue& queue, dpp::webhook hook);

    private:
        webhook_queue& queue;
        const dpp::webhook hook;
        std::mutex mu;
        // Guild -> user -> current voice channel. Seeded silently on guild availability.
        struct presence_snapshot
        {
            std::string text;
            std::string stream_title;
        };
        std::unordered_map<dpp::snowflake, std::unordered_map<dpp::snowflake, dpp::voicestate>> channels;
        std::unordered_map<dpp::snowflake, std::unordered_map<dpp::snowflake, presence_snapshot>> statuses;
        struct recent_status
        {
            dpp::snowflake guild;
            dpp::snowflake user;
            std::string text;
            std::chrono::steady_clock::time_point posted_at;
        };
        std::deque<recent_status> recent_statuses; // Only retain notices within the three-second window.

        static presence_snapshot describe(const dpp::presence& presence);
        void on_update(const dpp::voice_state_update_t& ev);
        void on_presence(const dpp::presence_update_t& ev);
        void send(dpp::snowflake guild, dpp::snowflake user, const std::string& text,
                  std::time_t timestamp, bool voice_detail = false) const;
    };
}

#endif //DPPSIMPLIFY_VC_JOINS_H
