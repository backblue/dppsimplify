#ifndef DPPSIMPLIFY_JOB_WATCHER_H
#define DPPSIMPLIFY_JOB_WATCHER_H

#include <dpp/dpp.h>
#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace simplify
{
    // Polls SimplifyJobs' listings.json and posts an embed through a webhook for every new posting
    // that isn't Canada/UK-only. The first successful fetch only seeds the known set.
    class job_watcher
    {
    public:
        job_watcher(dpp::cluster& bot, dpp::webhook hook);

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
        static constexpr auto LISTINGS_URL =
            "https://raw.githubusercontent.com/SimplifyJobs/Summer2026-Internships/refs/heads/dev/.github/scripts/listings.json";
        static constexpr auto REPO_URL = "https://github.com/SimplifyJobs/Summer2026-Internships";

        dpp::cluster& bot;
        const dpp::webhook hook;

        std::atomic<bool> in_flight{false};   // skip a tick if the previous fetch hasn't finished

        std::mutex mu;
        std::unordered_map<std::string, job> last;   // url -> job, from the latest successful fetch
        std::string etag;                            // sent as If-None-Match; unchanged file -> 304, no body
        bool seeded = false;

        void poll();
        void handle(const dpp::http_request_completion_t& r);
        void notify(const job& j) const;

        static std::unordered_map<std::string, job> parse(const std::string& body);
        static bool is_canada_or_uk_only(const std::vector<std::string>& locations);
        static std::string join(const std::vector<std::string>& items, size_t max_len);
    };
}

#endif //DPPSIMPLIFY_JOB_WATCHER_H
