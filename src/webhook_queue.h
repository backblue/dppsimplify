#ifndef DPPSIMPLIFY_WEBHOOK_QUEUE_H
#define DPPSIMPLIFY_WEBHOOK_QUEUE_H

#include <dpp/dpp.h>
#include <deque>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace simplify
{
    // Posts webhook messages one at a time per webhook, in the order they were queued.
    // Honors Discord's rate-limit headers (waits when the bucket is empty) and retries 429s
    // after retry_after, instead of firing everything at once and dropping what gets rejected.
    class webhook_queue
    {
    public:
        explicit webhook_queue(dpp::cluster& bot);

        // done (optional) gets the final result: success, a non-429 error, or the last 429 after
        // MAX_RETRIES. Without it, failures are logged.
        void post(const dpp::webhook& hook, const dpp::message& msg, dpp::command_completion_event_t done = {});

    private:
        struct item
        {
            dpp::webhook hook;
            dpp::message msg;
            dpp::command_completion_event_t done;
            int retries = 0;
        };

        static constexpr int MAX_RETRIES = 5;

        dpp::cluster& bot;

        std::mutex mu;
        std::unordered_map<dpp::snowflake, std::deque<item>> queues;   // webhook id -> posts not yet sent
        std::unordered_set<dpp::snowflake> busy;   // webhooks with a post in flight or waiting out a limit

        void send_next(dpp::snowflake id);
        void on_result(dpp::snowflake id, const dpp::confirmation_callback_t& cb);
        void after(uint64_t seconds, std::function<void()> fn);
    };
}

#endif //DPPSIMPLIFY_WEBHOOK_QUEUE_H
