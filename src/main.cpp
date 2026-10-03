#include <dpp/dpp.h>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include "forwarder.h"
#include "job_watcher.h"

using namespace std;
namespace simplify
{
    static string trim(const string& s)
    {
        const auto start = s.find_first_not_of(" \t\r\n");
        if (start == string::npos) return "";
        const auto end = s.find_last_not_of(" \t\r\n");
        return s.substr(start, end - start + 1);
    }

    static unordered_map<string, string> config(const string& path = CONFIG_PATH)
    {
        ifstream file(path);
        if (!file) throw runtime_error("Could not open " + path);

        unordered_map<string, string> config;
        string line;
        while (getline(file, line))
        {
            line = trim(line);
            if (line.empty() || line[0] == '#' || line[0] == '!') continue;

            const auto eq = line.find('=');
            if (eq == string::npos) continue;

            config[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
        }
        return config;
    }
}


int main(int argc, char** argv)
{
    // Config path: first argument, else $DPPSIMPLIFY_CONFIG, else the source-tree copy baked in by CMake.
    const char* env_path = getenv("DPPSIMPLIFY_CONFIG");
    const string config_path = argc > 1 ? argv[1] : env_path && *env_path ? env_path : CONFIG_PATH;

    const unordered_map<string, string> config = simplify::config(config_path);
    const auto require = [&](const string& key) -> const string&
    {
        const auto it = config.find(key);
        if (it == config.end() || it->second.empty())
            throw runtime_error(config_path + ": " + key + " is missing or empty");
        return it->second;
    };

    const dpp::webhook gen_hook{require("WEBHOOK_GEN")};
    const dpp::webhook mudae_hook{require("WEBHOOK_MUDAE")};
    const dpp::webhook jobs_hook{require("WEBHOOK_JOBS")};

    constexpr dpp::snowflake GENERAL_ID{1032677853101293603};
    constexpr dpp::snowflake MUDAE_ID{1477436372502446183};

    dpp::cluster bot{
        require("TOKEN"), dpp::i_default_intents | dpp::i_message_content | dpp::i_guild_members
    };
    bot.on_log(dpp::utility::cout_logger());

    simplify::forwarder forwarder{bot, {{GENERAL_ID, gen_hook}, {MUDAE_ID, mudae_hook}}};
    simplify::job_watcher jobs{bot, jobs_hook};

    bot.on_ready([&jobs](const dpp::ready_t&)
    {
        if (dpp::run_once<struct start_job_watcher>()) jobs.start();
    });

    bot.start(dpp::st_wait);
}