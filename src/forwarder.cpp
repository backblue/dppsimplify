#include "forwarder.h"

#include <memory>
#include <optional>
#include <regex>
#include <utility>
#include <vector>

using namespace std;

namespace simplify
{
    forwarder::forwarder(dpp::cluster& bot, unordered_map<dpp::snowflake, dpp::webhook> routes)
        : bot(bot), routes(std::move(routes))
    {
        bot.on_message_create([this](const dpp::message_create_t& ev) { on_create(ev); });
        bot.on_message_update([this](const dpp::message_update_t& ev) { on_update(ev); });
        bot.on_message_delete([this](const dpp::message_delete_t& ev) { on_delete(ev); });
    }

    void forwarder::on_create(const dpp::message_create_t& ev)
    {
        const dpp::message& m = ev.msg;
        if (m.author.is_bot() || m.webhook_id) return;   // includes our own webhook posts

        const auto route = routes.find(m.channel_id);
        if (route == routes.end()) return;

        const string text = flatten_mentions(m);
        if (text.empty() && m.attachments.empty()) return;   // e.g. sticker-only; webhook rejects empty

        {
            lock_guard lk(mu);
            messages[m.id] = {m.content, text, 0};
            order.push_back(m.id);
            while (order.size() > MAX_TRACKED)
            {
                messages.erase(order.front());
                order.pop_front();
            }
        }

        send(m, route->second, text, "", [this, id = m.id](dpp::snowflake mirror_id)
        {
            lock_guard lk(mu);
            if (const auto it = messages.find(id); it != messages.end()) it->second.mirror_id = mirror_id;
        });
    }

    void forwarder::on_update(const dpp::message_update_t& ev)
    {
        const dpp::message& m = ev.msg;
        const auto route = routes.find(m.channel_id);
        if (route == routes.end()) return;

        {
            lock_guard lk(mu);
            const auto it = messages.find(m.id);
            if (it == messages.end()) return;
            // Discord also sends updates when link embeds load; only real text edits count.
            if (it->second.last_content == m.content) return;
            it->second.last_content = m.content;
        }

        send(m, route->second, flatten_mentions(m), " - Edited");
    }

    void forwarder::on_delete(const dpp::message_delete_t& ev)
    {
        const auto route = routes.find(ev.channel_id);
        if (route == routes.end()) return;

        tracked t;
        {
            lock_guard lk(mu);
            const auto it = messages.find(ev.id);
            if (it == messages.end()) return;
            t = it->second;
            messages.erase(it);   // its id stays in `order`; erasing a missing key later is harmless
        }
        if (t.mirror_text.empty() || !t.mirror_id) return;

        dpp::message edit("~~" + t.mirror_text + "~~");
        edit.id = t.mirror_id;
        edit.set_allowed_mentions(false, false, false, false);
        bot.edit_webhook_message(route->second, edit);
    }

    void forwarder::send(const dpp::message& m, const dpp::webhook& hook, const string& text,
                         const string& name_suffix, function<void(dpp::snowflake)> on_sent) const
    {
        dpp::webhook w = hook;
        w.name = display_name(m) + name_suffix;
        w.avatar_url = avatar_url(m);

        // Shared by all download callbacks; whichever finishes last posts the message.
        struct pending
        {
            mutex mu;
            vector<optional<pair<string, string>>> files;   // (filename, body), kept in original order
            size_t remaining;
        };
        auto p = make_shared<pending>();
        p->files.resize(m.attachments.size());
        p->remaining = m.attachments.size();

        auto post = [this, w, text, p, on_sent]
        {
            dpp::message out(text);
            out.set_allowed_mentions(false, false, false, false);   // never ping, even @everyone
            for (auto& f : p->files)
                if (f) out.add_file(f->first, f->second);

            bot.execute_webhook(w, out, /*wait=*/true, 0, "",
                [on_sent](const dpp::confirmation_callback_t& cb)
                {
                    if (cb.is_error())
                    {
                        cout << "Webhook post failed: " << cb.get_error().human_readable << '\n';
                        return;
                    }
                    if (on_sent) on_sent(cb.get<dpp::message>().id);
                });
        };

        if (m.attachments.empty())
        {
            post();
            return;
        }

        for (size_t i = 0; i < m.attachments.size(); ++i)
        {
            const dpp::attachment& a = m.attachments[i];
            bot.request(a.url, dpp::m_get, [p, i, name = a.filename, post](const dpp::http_request_completion_t& r)
            {
                bool last;
                {
                    lock_guard lk(p->mu);
                    if (r.status == 200) p->files[i] = make_pair(name, r.body);   // failed downloads are skipped
                    last = --p->remaining == 0;
                }
                if (last) post();
            });
        }
    }

    string forwarder::display_name(const dpp::message& m)
    {
        if (!m.member.get_nickname().empty()) return m.member.get_nickname();
        if (!m.author.global_name.empty()) return m.author.global_name;
        return m.author.username;
    }

    string forwarder::avatar_url(const dpp::message& m)
    {
        const string guild_avatar = m.member.get_avatar_url();
        return guild_avatar.empty() ? m.author.get_avatar_url() : guild_avatar;
    }

    // <@id> / <@!id> -> @name, <@&id> -> @role, <#id> -> #channel. Unknown ids are left as-is.
    string forwarder::flatten_mentions(const dpp::message& m)
    {
        unordered_map<dpp::snowflake, string> users;
        for (const auto& [user, member] : m.mentions)
        {
            if (!member.get_nickname().empty()) users[user.id] = member.get_nickname();
            else if (!user.global_name.empty()) users[user.id] = user.global_name;
            else users[user.id] = user.username;
        }

        static const regex mention(R"(<(@!?|@&|#)(\d+)>)");
        string out;
        auto last = m.content.cbegin();
        for (sregex_iterator it(m.content.begin(), m.content.end(), mention), end; it != end; ++it)
        {
            const smatch& match = *it;
            out.append(last, match[0].first);
            last = match[0].second;

            const string kind = match[1];
            const dpp::snowflake id{match[2].str()};
            optional<string> replacement;

            if (kind == "#")
            {
                // message::mention_channels is only filled for crossposts, so use the cache.
                if (const dpp::channel* c = dpp::find_channel(id)) replacement = "#" + c->name;
            }
            else if (kind == "@&")
            {
                if (const dpp::role* r = dpp::find_role(id)) replacement = "@" + r->name;
            }
            else if (const auto u = users.find(id); u != users.end())
            {
                replacement = "@" + u->second;
            }

            out += replacement ? *replacement : match[0].str();
        }
        out.append(last, m.content.cend());
        return out;
    }
}
