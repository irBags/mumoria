// Mumoria - the server: settings, the database, the world, the timer, and
// the listener. See docs/MUMORIA_DESIGN.md section 4.

#include "irc.h"
#include "mu_config.h"
#include "screens.h"
#include "session.h"
#include "social.h"

#include <csignal>
#include <fstream>
#include <map>
#include <unistd.h>

const char *mu_aol_secret = "";

static std::map<std::string, std::string> settings;

static std::string setting(const std::string &key, const std::string &dflt) {
    auto it = settings.find(key);
    return it != settings.end() ? it->second : dflt;
}

// key = value lines; # starts a comment
static void read_settings(const std::string &path) {
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        size_t hash = line.find('#');
        if (hash != std::string::npos) {
            line.erase(hash);
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        auto trim = [](std::string s) {
            size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
            return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
        };
        settings[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
}

[[noreturn]] static void shut_down(const char *why) {
    fprintf(stderr, "mumoria: shutting down (%s)\n", why);
    mu_system_all("*** The server is shutting down. Your character is saved. ***");
    pthread_mutex_lock(&mu_sessions_lock);
    for (MuSession *p : mu_sessions) {
        p->kicked = true;
        p->kick_reason = "the server is shutting down";
        mu_conn_kill(p->conn);
    }
    pthread_mutex_unlock(&mu_sessions_lock);
    // each session saves its character on the way out
    for (int i = 0; i < 100; i++) {
        pthread_mutex_lock(&mu_sessions_lock);
        bool empty = mu_sessions.empty();
        pthread_mutex_unlock(&mu_sessions_lock);
        if (empty) {
            break;
        }
        usleep(100000);
    }
    fprintf(stderr, "mumoria: sessions closed; saving the world\n");
    mu_world_shutdown();
    fprintf(stderr, "mumoria: saved\n");
    mu_db_close();
    _exit(0);
}

static void *timer_thread(void *) {
    time_t warned = 0;
    for (;;) {
        sleep(15);
        mu_world_lock(nullptr);
        mu_world_maintain(false);
        mu_world_unlock(nullptr, false);
        mu_auction_settle();

        time_t at = mu_shutdown_at, now = time(nullptr);
        if (at != 0 && now >= at) {
            shut_down("scheduled");
        }
        if (at != 0 && at - now <= 60 && warned != at) {
            warned = at;
            mu_system_all("*** The server shuts down in one minute. ***");
        }
    }
    return nullptr;
}

static void *signal_thread(void *arg) {
    auto *set = (sigset_t *) arg;
    int sig = 0;
    sigwait(set, &sig);
    shut_down(sig == SIGINT ? "SIGINT" : "SIGTERM");
}

static int create_admin(const char *name) {
    std::string pw1, pw2;
    printf("Password for %s: ", name);
    fflush(stdout);
    std::getline(std::cin, pw1);
    printf("Again: ");
    fflush(stdout);
    std::getline(std::cin, pw2);
    if (pw1 != pw2 || pw1.size() < 6) {
        fprintf(stderr, "The passwords differ or are shorter than 6 characters.\n");
        return 1;
    }
    MuAccount acct{};
    if (!mu_db_account_find(name, &acct)) {
        if (!mu_valid_name(name) || mu_db_account_create(name, pw1.c_str(), 2) <= 0) {
            fprintf(stderr, "Could not create %s.\n", name);
            return 1;
        }
    } else {
        mu_db_set_password(acct.id, pw1.c_str());
        mu_db_run("UPDATE account SET role = 2 WHERE id = ?", "i", acct.id);
    }
    mu_db_audit("console", "create-admin", name, "");
    printf("%s is an admin.\n", name);
    return 0;
}

int main(int argc, char *argv[]) {
    std::string config = "mumoria.conf", admin;
    std::string port_arg, db_arg;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--config" && i + 1 < argc) {
            config = argv[++i];
        } else if (a == "--port" && i + 1 < argc) {
            port_arg = argv[++i];
        } else if (a == "--db" && i + 1 < argc) {
            db_arg = argv[++i];
        } else if (a == "--create-admin" && i + 1 < argc) {
            admin = argv[++i];
        } else {
            printf("Usage: mumoria-server [--config FILE] [--port N] [--db FILE] [--create-admin NAME]\n");
            return a == "--help" || a == "-h" ? 0 : 1;
        }
    }
    read_settings(config);
    int port = atoi((port_arg.empty() ? setting("port", std::to_string(MU_DEFAULT_PORT)) : port_arg).c_str());
    std::string db = db_arg.empty() ? setting("db", "mumoria.db") : db_arg;
    static std::string secret = setting("aol_token_secret", "");
    mu_aol_secret = secret.c_str();
    mu_guild_cost = atoi(setting("guild_cost", "1000").c_str());
    mu_gold_level_bonus = (int) (atof(setting("party_gold_per_level", "0.5").c_str()) * 100 + 0.5);

    if (mu_db_open(db.c_str()) != 0) {
        return 1;
    }
    if (!admin.empty()) {
        return create_admin(admin.c_str());
    }

    // the signals go to one thread, which shuts the server down cleanly
    static sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &set, nullptr);

    mu_world_lock(nullptr);
    mu_world_init();
    mu_world_unlock(nullptr, false);

    int fd = mu_net_listen(port);
    if (fd < 0) {
        return 1;
    }

    pthread_t t;
    pthread_create(&t, nullptr, timer_thread, nullptr);
    pthread_create(&t, nullptr, signal_thread, &set);

    static std::string irc_host = setting("irc_host", ""), irc_channel = setting("irc_channel", "#mumoria"),
                       irc_nick = setting("irc_nick", "Mumoria");
    mu_irc_start(irc_host.c_str(), atoi(setting("irc_port", "6667").c_str()), irc_channel.c_str(), irc_nick.c_str(),
                 [](const char *nick, const char *text) { mu_chat_from_irc(nick, text); });

    fprintf(stderr, "mumoria: listening on port %d, database %s\n", port, db.c_str());
    mu_net_serve(fd, mu_session_run);
    return 0;
}
