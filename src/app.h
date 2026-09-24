// Quant desktop client: runs a full node in-process and draws the wallet / miner / explorer UI.
#pragma once
#include <atomic>
#include <deque>
#include <future>
#include <memory>
#include <string>
#include <vector>

#include <imgui.h>

#include "node/node.h"

class App {
public:
    App(int argc, char** argv);
    void frame();
    void tick_background();
    void shutdown();
    bool quit_requested() const { return quit_; }
    static void set_fonts(ImFont* big, ImFont* mono);
    // Scripted screenshot mode (-shots=<dir>): returns a file path when this frame should be saved.
    std::string capture_request() { std::string s; s.swap(capture_); return s; }

private:
    void drive_shots();
    std::string shots_dir_, capture_;
    int shot_step_ = 0, shot_frames_ = 0;
    enum class Screen { Start, Starting, WalletSetup, Main, Failed };
    enum class Page { Overview, Send, Receive, History, Contacts, Mining, Network, Explorer, Console, Settings };

    struct Snapshot {
        quant::ChainStats chain;
        bool syncing = true;
        quant::NetStats net;
        std::vector<quant::PeerInfo> peers;
        quant::Balance bal;
        std::vector<quant::WalletTx> history;
        struct Addr { std::string address, label; quant::Amount balance; uint32_t index; };
        std::vector<Addr> addresses;
        std::vector<quant::Contact> contacts;
        std::string receive_address;
        bool mining = false;
        int threads = 0;
        double hashrate = 0;
        uint64_t found = 0;
        struct Blk { int64_t height; std::string hash; uint64_t time; size_t txs; std::string reward; bool pruned; };
        std::vector<Blk> recent;
        size_t mempool = 0;
    };

    void start_node();
    void refresh();
    void draw_start();
    void draw_starting();
    void draw_wallet_setup();
    void draw_main();
    void draw_sidebar();
    void draw_statusbar();
    void page_overview();
    void page_send();
    void page_receive();
    void page_history();
    void page_contacts();
    void page_mining();
    void page_network();
    void page_explorer();
    void page_console();
    void page_settings();
    void explorer_search(const std::string& q);
    void console_run(const std::string& line);
    void toast(const std::string& msg, bool error = false);
    void draw_toasts();

    quant::NodeConfig cfg_;
    std::unique_ptr<quant::Node> node_;
    std::future<std::string> starting_;
    Screen screen_ = Screen::Start;
    Page page_ = Page::Overview;
    std::string fail_msg_;
    bool quit_ = false;
    Snapshot snap_;
    double last_refresh_ = -10;
    std::vector<float> hash_hist_;

    // start screen
    int net_choice_ = 0; // 0 testnet, 1 regtest, 2 mainnet
    char datadir_[512] = {0};
    int start_threads_ = 0;
    char addnode_[128] = {0};

    // wallet setup
    int wallet_mode_ = 0; // 0 choose, 1 new, 2 restore, 3 open
    std::string new_words_;
    bool words_confirmed_ = false;
    char restore_words_[1024] = {0};
    char passphrase_[128] = {0};
    char password_[128] = {0}, password2_[128] = {0};
    std::string wallet_err_;

    // send
    char send_to_[128] = {0}, send_amount_[40] = {0}, send_note_[128] = {0}, send_fee_[40] = {0};
    bool send_subtract_ = false;
    bool send_confirm_open_ = false;
    quant::Transaction pending_tx_;
    quant::Amount pending_fee_ = 0;
    std::string send_err_;

    // receive / contacts
    char new_label_[64] = {0};
    char contact_name_[64] = {0}, contact_addr_[128] = {0};

    // mining
    int mine_threads_ = 1;
    char mine_addr_[128] = {0};

    // network
    char add_peer_[128] = {0};

    // explorer
    char search_[160] = {0};
    quant::json explorer_result_;
    std::string explorer_kind_, explorer_err_;
    std::vector<std::string> explorer_back_;

    // console
    char console_in_[512] = {0};
    std::vector<std::pair<std::string, bool>> console_lines_; // text, is_error
    std::vector<std::string> console_hist_;
    int console_hist_pos_ = -1;
    std::future<std::pair<std::string, bool>> console_job_;
    bool console_scroll_ = false;

    // settings
    bool show_seed_ = false;
    char old_pw_[128] = {0}, new_pw_[128] = {0};

    struct Toast { std::string msg; bool error; double until; };
    std::deque<Toast> toasts_;
};
