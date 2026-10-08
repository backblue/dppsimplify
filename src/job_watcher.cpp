#include "job_watcher.h"

#include <algorithm>
#include <map>
#include <unordered_set>
#include <utility>

using namespace std;
using json = nlohmann::json;

namespace simplify
{
    job_watcher::job_watcher(dpp::cluster& bot, webhook_queue& queue, dpp::webhook hook)
        : bot(bot), queue(queue), hook(std::move(hook))
    {
    }

    void job_watcher::start()
    {
        poll();   // start_timer's first tick is POLL_SECONDS away, so fetch once right now
        bot.start_timer([this](dpp::timer) { poll(); }, POLL_SECONDS);
    }

    void job_watcher::poll()
    {
        if (in_flight.exchange(true)) return;

        multimap<string, string> headers;
        {
            lock_guard lk(mu);
            if (!etag.empty()) headers.emplace("If-None-Match", etag);
        }

        bot.request(LISTINGS_URL, dpp::m_get, [this](const dpp::http_request_completion_t& r)
        {
            handle(r);
            in_flight = false;
        }, "", "text/plain", headers);
    }

    void job_watcher::handle(const dpp::http_request_completion_t& r)
    {
        if (r.status == 304) return;   // unchanged since the last fetch
        if (r.status != 200)
        {
            bot.log(dpp::ll_warning, "Job fetch failed: HTTP " + to_string(r.status));
            return;
        }

        unordered_map<string, job> now;
        try
        {
            now = parse(r.body);
        }
        catch (const exception& e)
        {
            bot.log(dpp::ll_warning, string("Job listings didn't parse: ") + e.what());
            return;
        }

        vector<job> fresh;
        {
            lock_guard lk(mu);
            for (const auto& [name, value] : r.headers)
            {
                string lower = name;
                ranges::transform(lower, lower.begin(), [](unsigned char c) { return tolower(c); });
                if (lower == "etag") etag = value;
            }

            for (auto& [url, j] : now)
                if (seen.insert(url).second && seeded && !is_canada_or_uk_only(j.locations))
                    fresh.push_back(std::move(j));

            if (!seeded)
            {
                bot.log(dpp::ll_info, "Loaded " + to_string(now.size()) + " jobs");
                seeded = true;
            }
        }

        if (fresh.empty()) return;

        dpp::webhook w = hook;
        w.name = "New Job";
        if (fresh.size() > MAX_NEW_PER_POLL)
        {
            bot.log(dpp::ll_warning, to_string(fresh.size()) + " new postings in one poll; posting a notice instead");
            queue.post(w, dpp::message(to_string(fresh.size()) + " new postings at once (likely a feed reset). See " +
                                       REPO_URL + " for the full list."));
            return;
        }

        ranges::sort(fresh, {}, &job::date_posted);   // oldest first, so the newest ends up at the bottom
        notify(fresh);
    }

    // Packs the embeds into as few messages as Discord allows; the queue spaces them out.
    void job_watcher::notify(const vector<job>& jobs) const
    {
        dpp::webhook w = hook;
        w.name = "New Job";

        dpp::message msg;
        size_t chars = 0;
        for (const job& j : jobs)
        {
            bot.log(dpp::ll_info, "New posting: " + j.title + " @ " + j.company + " " + j.url);

            dpp::embed e = make_embed(j);
            const size_t n = embed_chars(e);
            if (!msg.embeds.empty() &&
                (msg.embeds.size() == MAX_EMBEDS_PER_MESSAGE || chars + n > MAX_EMBED_CHARS_PER_MESSAGE))
            {
                queue.post(w, msg);
                msg = dpp::message();
                chars = 0;
            }
            msg.add_embed(e);
            chars += n;
        }
        if (!msg.embeds.empty()) queue.post(w, msg);
    }

    dpp::embed job_watcher::make_embed(const job& j)
    {
        // Discord limits: title 256, field value 1024.
        string title = j.title + " @ " + j.company;
        if (title.size() > 256) title = title.substr(0, 253) + "...";

        return dpp::embed()
            .set_color(dpp::colors::yellow)
            .set_author("New " + j.category + " posting", REPO_URL, "")
            .set_title(title)
            .set_url(j.url)
            .add_field("Term", join(j.terms, 1024), false)
            .add_field("Location", join(j.locations, 1024), false)
            .set_footer("Created on", "")
            .set_timestamp(j.date_posted);
    }

    // The characters Discord counts toward the 6000-per-message embed limit.
    size_t job_watcher::embed_chars(const dpp::embed& e)
    {
        size_t n = e.title.size() + e.description.size();
        if (e.author) n += e.author->name.size();
        if (e.footer) n += e.footer->text.size();
        for (const dpp::embed_field& f : e.fields) n += f.name.size() + f.value.size();
        return n;
    }

    unordered_map<string, job_watcher::job> job_watcher::parse(const string& body)
    {
        // The file is ~13 MB; drop fields we never read while parsing to keep peak memory down.
        static const unordered_set<string> wanted{
            "url", "company_name", "title", "category", "active", "terms", "locations", "date_posted"
        };
        const auto keep = [](int depth, const json::parse_event_t event, const json& parsed)
        {
            return !(event == json::parse_event_t::key && depth == 2 && !wanted.contains(parsed.get<string>()));
        };

        const json arr = json::parse(body, keep);
        unordered_map<string, job> jobs;
        jobs.reserve(arr.size());

        for (const json& j : arr)
        {
            if (!j.contains("url") || !j["url"].is_string()) continue;

            job x;
            x.url = j["url"];
            x.category = j.value("category", "");
            x.company = j.value("company_name", "Unknown");
            x.title = j.value("title", "Unknown");
            x.active = j.value("active", false);
            x.terms = j.value("terms", vector<string>{});
            x.locations = j.value("locations", vector<string>{});
            x.date_posted = j.value("date_posted", time_t{0});
            jobs.emplace(x.url, std::move(x));
        }
        return jobs;
    }

    // True if every location is in Canada or the UK. Matches on the ending ("Toronto, ON, Canada",
    // "Remote in UK") so places like "Milwaukee, WI" don't count as UK.
    bool job_watcher::is_canada_or_uk_only(const vector<string>& locations)
    {
        if (locations.empty()) return false;

        return ranges::all_of(locations, [](const string& loc)
        {
            string l = loc;
            ranges::transform(l, l.begin(), [](unsigned char c) { return tolower(c); });
            while (!l.empty() && isspace(static_cast<unsigned char>(l.back()))) l.pop_back();

            for (const string suffix : {"canada", "uk", "united kingdom"})
            {
                if (l == suffix) return true;
                if (l.ends_with(", " + suffix) || l.ends_with(" in " + suffix)) return true;
            }
            return false;
        });
    }

    string job_watcher::join(const vector<string>& items, size_t max_len)
    {
        if (items.empty()) return "N/A";

        string out;
        for (const string& s : items)
        {
            if (!out.empty()) out += ", ";
            out += s;
        }
        if (out.size() > max_len) out = out.substr(0, max_len - 3) + "...";
        return out;
    }
}
