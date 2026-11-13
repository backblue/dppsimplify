#include <dpp/dpp.h>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include "forwarder.h"
#include "job_watcher.h"
#include "vc_joins.h"

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
    const dpp::webhook vc_hook{require("WEBHOOK_STATUS_VC")};

    const auto offline_setting = config.find("OFFLINE_STATUS");
    const string offline_status = offline_setting == config.end() ? "0" : offline_setting->second;
    if (offline_status != "0" && offline_status != "1")
        throw runtime_error(config_path + ": OFFLINE_STATUS must be 0 or 1");
    const bool appear_offline = offline_status == "1";
    dpp::snowflake online_channel;
    if (appear_offline)
    {
        const string& channel = require("IS_ONLINE_CHANNEL");
        if (channel.find_first_not_of("0123456789") != string::npos)
            throw runtime_error(config_path + ": IS_ONLINE_CHANNEL must be a channel ID");
        online_channel = stoull(channel);
        if (!online_channel)
            throw runtime_error(config_path + ": IS_ONLINE_CHANNEL must be a nonzero channel ID");
    }

    constexpr dpp::snowflake GENERAL_ID{1032677853101293603};
    constexpr dpp::snowflake MUDAE_ID{1477436372502446183};

    dpp::cluster bot{
        require("TOKEN"), dpp::i_default_intents | dpp::i_message_content | dpp::i_guild_members | dpp::i_guild_presences
    };
    // Like dpp::utility::cout_logger(), minus DEBUG: those lines include full webhook URLs (with tokens).
    bot.on_log([](const dpp::log_t& e)
    {
        if (e.severity < dpp::ll_info) return;
        cout << "[" << dpp::utility::current_date_time() << "] " << dpp::utility::loglevel(e.severity)
             << ": " << e.message << endl;
    });

    simplify::webhook_queue queue{bot};
    simplify::forwarder forwarder{bot, queue, {{GENERAL_ID, gen_hook}, {MUDAE_ID, mudae_hook}}};
    simplify::job_watcher jobs{bot, queue, jobs_hook};
    simplify::vc_joins voice{bot, queue, vc_hook};

    bot.on_ready([&bot, &jobs, appear_offline, online_channel](const dpp::ready_t&)
    {
        // Reapply invisible presence on reconnect, but start only one heartbeat timer.
        if (appear_offline)
            bot.set_presence(dpp::presence(dpp::ps_invisible, dpp::at_game, ""));

        if (dpp::run_once<struct start_job_watcher>())
        {
            jobs.start();
            if (appear_offline)
            {
                const auto heartbeat = [&bot, online_channel]
                {
                    bot.message_create(dpp::message(online_channel,
                        "Bot is online and continuously checking for jobs"));
                };
                heartbeat();
                bot.start_timer([heartbeat](dpp::timer) { heartbeat(); }, 2 * 60 * 60);
            }
        }
    });

    bot.start(dpp::st_wait);
}
