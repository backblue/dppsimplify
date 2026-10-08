#include "webhook_queue.h"

#include <algorithm>
#include <utility>

using namespace std;

namespace simplify
{
    webhook_queue::webhook_queue(dpp::cluster& bot)
        : bot(bot)
    {
    }

    void webhook_queue::post(const dpp::webhook& hook, const dpp::message& msg, dpp::command_completion_event_t done)
    {
        {
            lock_guard lk(mu);
            queues[hook.id].push_back({hook, msg, std::move(done)});
            if (!busy.insert(hook.id).second) return;   // already draining this webhook
        }
        send_next(hook.id);
    }

    void webhook_queue::send_next(dpp::snowflake id)
    {
        item next;
        {
            lock_guard lk(mu);
            auto& q = queues[id];
            if (q.empty())
            {
                busy.erase(id);
                return;
            }
            next = q.front();   // stays queued until Discord answers, so a 429 can resend it
        }

        bot.execute_webhook(next.hook, next.msg, /*wait=*/true, 0, "",
            [this, id](const dpp::confirmation_callback_t& cb) { on_result(id, cb); });
    }

    void webhook_queue::on_result(dpp::snowflake id, const dpp::confirmation_callback_t& cb)
    {
        const dpp::http_request_completion_t& h = cb.http_info;

        if (cb.is_error() && h.status == 429)
        {
            bool retry;
            {
                lock_guard lk(mu);
                retry = ++queues[id].front().retries <= MAX_RETRIES;
            }
            if (retry)
            {
                const uint64_t wait = max<uint64_t>(1, h.ratelimit_retry_after);
                bot.log(dpp::ll_warning, "Webhook rate limited; retrying in " + to_string(wait) + "s");
                after(wait, [this, id] { send_next(id); });
                return;
            }
        }

        item finished;
        {
            lock_guard lk(mu);
            auto& q = queues[id];
            finished = std::move(q.front());
            q.pop_front();
        }

        if (finished.done) finished.done(cb);
        else if (cb.is_error())
            bot.log(dpp::ll_error, "Webhook post failed (HTTP " + to_string(h.status) + "): " +
                cb.get_error().human_readable + " | " + h.body.substr(0, 500));

        // Bucket used up: wait for it to refill before the next post instead of earning a 429.
        if (h.ratelimit_limit > 0 && h.ratelimit_remaining == 0)
            after(max<uint64_t>(1, h.ratelimit_reset_after), [this, id] { send_next(id); });
        else
            send_next(id);
    }

    void webhook_queue::after(uint64_t seconds, function<void()> fn)
    {
        // One-shot: stop the timer on its first tick.
        bot.start_timer([this, fn = std::move(fn)](dpp::timer t)
        {
            bot.stop_timer(t);
            fn();
        }, seconds);
    }
}
