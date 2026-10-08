#ifndef DPPSIMPLIFY_JOB_WATCHER_H
#define DPPSIMPLIFY_JOB_WATCHER_H

#include <dpp/dpp.h>
#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "webhook_queue.h"

namespace simplify
{
    // Polls SimplifyJobs' listings.json and posts an embed through a webhook for every new posting
    // that isn't Canada/UK-only. The first successful fetch only seeds the known set.
    class job_watcher
    {
    public:
        job_watcher(dpp::cluster& bot, webhook_queue& queue, dpp::webhook hook);

        // Polls now, then every POLL_SECONDS. Call once, e.g. from on_ready behind dpp::run_once.
        void start();

    private:
        struct job
        {
            std::string category;
            std::string company;
            std::string title;
            std::string url;
            bool active;
            std::vector<std::string> terms;
            std::vector<std::string> locations;
            time_t date_posted;
        };

        static constexpr uint64_t POLL_SECONDS = 20;
        static constexpr size_t MAX_EMBEDS_PER_MESSAGE = 10;     // Discord limit
        static constexpr size_t MAX_EMBED_CHARS_PER_MESSAGE = 6000;   // Discord limit, summed over embeds
        // More new postings than this in one poll looks like a feed reset, not real news:
        // mark them seen and post one notice instead of hundreds of embeds.
        static constexpr size_t MAX_NEW_PER_POLL = 100;
        static constexpr auto LISTINGS_URL =
            "https://raw.githubusercontent.com/SimplifyJobs/Summer2026-Internships/refs/heads/dev/.github/scripts/listings.json";
        static constexpr auto REPO_URL = "https://github.com/SimplifyJobs/Summer2026-Internships";

        dpp::cluster& bot;
        webhook_queue& queue;
        const dpp::webhook hook;

        std::atomic<bool> in_flight{false};   // skip a tick if the previous fetch hasn't finished

        std::mutex mu;
        // Every url ever seen, never shrunk. Comparing against only the previous fetch re-posted
        // jobs whenever a CDN edge briefly served an older copy of the file.
        std::unordered_set<std::string> seen;
        std::string etag;                            // sent as If-None-Match; unchanged file -> 304, no body
        bool seeded = false;

        void poll();
        void handle(const dpp::http_request_completion_t& r);
        void notify(const std::vector<job>& jobs) const;

        static dpp::embed make_embed(const job& j);
        static size_t embed_chars(const dpp::embed& e);

        static std::unordered_map<std::string, job> parse(const std::string& body);
        static bool is_canada_or_uk_only(const std::vector<std::string>& locations);
        static std::string join(const std::vector<std::string>& items, size_t max_len);
    };
}

#endif //DPPSIMPLIFY_JOB_WATCHER_H
