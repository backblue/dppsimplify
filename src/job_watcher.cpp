#include "job_watcher.h"

#include <algorithm>
#include <map>
#include <unordered_set>
#include <utility>

using namespace std;
using json = nlohmann::json;

namespace simplify
{
    job_watcher::job_watcher(dpp::cluster& bot, dpp::webhook hook)
        : bot(bot), hook(std::move(hook))
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

            if (seeded)
            {
                for (const auto& [url, j] : now)
                    if (!last.contains(url) && !is_canada_or_uk_only(j.locations))
                        fresh.push_back(j);
            }
            else
            {
                bot.log(dpp::ll_info, "Loaded " + to_string(now.size()) + " jobs");
                seeded = true;
            }
            last = std::move(now);
        }

        for (const job& j : fresh) notify(j);
    }

    void job_watcher::notify(const job& j) const
    {
        bot.log(dpp::ll_info, "New posting: " + j.title + " @ " + j.company + " " + j.url);

        // Discord limits: title 256, field value 1024.
        string title = j.title + " @ " + j.company;
        if (title.size() > 256) title = title.substr(0, 253) + "...";

        const dpp::embed e = dpp::embed()
            .set_color(dpp::colors::yellow)
            .set_author("New " + j.category + " posting", REPO_URL, "")
            .set_title(title)
            .set_url(j.url)
            .add_field("Term", join(j.terms, 1024), false)
            .add_field("Location", join(j.locations, 1024), false)
            .set_footer("Created on", "")
            .set_timestamp(j.date_posted);

        dpp::webhook w = hook;
        w.name = "New Job";
        bot.execute_webhook(w, dpp::message().add_embed(e));
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
