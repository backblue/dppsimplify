#include "forwarder.h"

#include <memory>
#include <optional>
#include <regex>
#include <utility>
#include <vector>

using namespace std;

namespace simplify
{
    forwarder::forwarder(dpp::cluster& bot, webhook_queue& queue, unordered_map<dpp::snowflake, dpp::webhook> routes)
        : bot(bot), queue(queue), routes(std::move(routes))
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
            messages[m.id] = {m.content};
            order.push_back(m.id);
            while (order.size() > MAX_TRACKED)
            {
                messages.erase(order.front());
                order.pop_front();
            }
        }

        send(m, route->second, text, "");
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

    // The mirrored copy is left as-is; deleting the original only stops tracking it for edits.
    void forwarder::on_delete(const dpp::message_delete_t& ev)
    {
        if (!routes.contains(ev.channel_id)) return;

        lock_guard lk(mu);
        messages.erase(ev.id);   // its id stays in `order`; erasing a missing key later is harmless
    }

    void forwarder::send(const dpp::message& m, const dpp::webhook& hook, const string& text,
                         const string& name_suffix) const
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

        vector<string> urls;
        for (const dpp::attachment& a : m.attachments) urls.push_back(a.url);

        auto post = [this, w, text, p, urls]
        {
            dpp::message out(text);
            out.set_allowed_mentions(false, false, false, false);   // never ping, even @everyone
            size_t attached = 0;
            for (auto& f : p->files)
                if (f)
                {
                    out.add_file(f->first, f->second);
                    ++attached;
                }

            // Every download failed and there's no text: link the originals instead of posting an
            // empty message, which Discord rejects. (Signed CDN links expire after about a day.)
            if (attached == 0 && text.empty())
            {
                string links;
                for (const string& u : urls) links += (links.empty() ? "" : "\n") + u;
                out.set_content(links);
            }

            queue.post(w, out, [this, attached](const dpp::confirmation_callback_t& cb)
            {
                if (cb.is_error())
                    bot.log(dpp::ll_error, "Webhook post failed (HTTP " + to_string(cb.http_info.status) +
                        ", " + to_string(attached) + " file(s)): " + cb.get_error().human_readable +
                        " | " + cb.http_info.body.substr(0, 500));
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
            bot.request(a.url, dpp::m_get, [this, p, i, name = a.filename, post](const dpp::http_request_completion_t& r)
            {
                if (r.status != 200)
                    bot.log(dpp::ll_warning, "Attachment download failed: HTTP " + to_string(r.status) + " for " + name);

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
