#ifndef DPPSIMPLIFY_FORWARDER_H
#define DPPSIMPLIFY_FORWARDER_H

#include <dpp/dpp.h>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

namespace simplify
{
    // Mirrors messages from source channels to webhooks, with mentions flattened to plain text
    // so nothing pings. Edits are reposted as "Name - Edited"; deletes strike through the mirror.
    class forwarder
    {
    public:
        // routes: source channel id -> webhook that channel is mirrored to
        forwarder(dpp::cluster& bot, std::unordered_map<dpp::snowflake, dpp::webhook> routes);

    private:
        struct tracked
        {
            std::string last_content;   // raw content as of the latest create/update, for edit detection
            std::string mirror_text;    // text that was posted in the mirror, for the strikethrough
            dpp::snowflake mirror_id;   // id of the webhook's copy (0 until Discord replies)
        };

        static constexpr uint8_t MAX_TRACKED = (1 << 8) - 1;

        dpp::cluster& bot;
        const std::unordered_map<dpp::snowflake, dpp::webhook> routes;

        std::mutex mu;
        std::unordered_map<dpp::snowflake, tracked> messages;
        std::deque<dpp::snowflake> order;   // insertion order, to evict the oldest past MAX_TRACKED

        void on_create(const dpp::message_create_t& ev);
        void on_update(const dpp::message_update_t& ev);
        void on_delete(const dpp::message_delete_t& ev);

        // Downloads m's attachments, then posts text + files through hook as m's author.
        void send(const dpp::message& m, const dpp::webhook& hook, const std::string& text,
                  const std::string& name_suffix,
                  std::function<void(dpp::snowflake)> on_sent = {}) const;

        static std::string display_name(const dpp::message& m);
        static std::string avatar_url(const dpp::message& m);
        static std::string flatten_mentions(const dpp::message& m);
    };
}

#endif //DPPSIMPLIFY_FORWARDER_H
