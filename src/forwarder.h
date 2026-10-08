#ifndef DPPSIMPLIFY_FORWARDER_H
#define DPPSIMPLIFY_FORWARDER_H

#include <dpp/dpp.h>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

#include "webhook_queue.h"

namespace simplify
{
    // Mirrors messages from source channels to webhooks, with mentions flattened to plain text
    // so nothing pings. Edits are reposted as "Name - Edited"; deletes leave the mirror untouched.
    class forwarder
    {
    public:
        // routes: source channel id -> webhook that channel is mirrored to
        forwarder(dpp::cluster& bot, webhook_queue& queue, std::unordered_map<dpp::snowflake, dpp::webhook> routes);

    private:
        struct tracked
        {
            std::string last_content;   // raw content as of the latest create/update, for edit detection
        };

        static constexpr uint8_t MAX_TRACKED = (1 << 8) - 1;

        dpp::cluster& bot;
        webhook_queue& queue;
        const std::unordered_map<dpp::snowflake, dpp::webhook> routes;

        std::mutex mu;
        std::unordered_map<dpp::snowflake, tracked> messages;
        std::deque<dpp::snowflake> order;   // insertion order, to evict the oldest past MAX_TRACKED

        void on_create(const dpp::message_create_t& ev);
        void on_update(const dpp::message_update_t& ev);
        void on_delete(const dpp::message_delete_t& ev);

        // Downloads m's attachments, then posts text + files through hook as m's author.
        void send(const dpp::message& m, const dpp::webhook& hook, const std::string& text,
                  const std::string& name_suffix) const;

        static std::string display_name(const dpp::message& m);
        static std::string avatar_url(const dpp::message& m);
        static std::string flatten_mentions(const dpp::message& m);
    };
}

#endif //DPPSIMPLIFY_FORWARDER_H
