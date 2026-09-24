#include "app.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <ctime>
#include <filesystem>
#include <sstream>

#include <qrcodegen.hpp>

#include "consensus/limits.h"
#include "util/log.h"
#include "wallet/mnemonic.h"

using namespace quant;

static ImFont* g_big = nullptr;
static ImFont* g_mono = nullptr;
void App::set_fonts(ImFont* big, ImFont* mono) { g_big = big; g_mono = mono; }

static const ImVec4 COL_DIM(0.60f, 0.62f, 0.70f, 1), COL_GOOD(0.35f, 0.85f, 0.55f, 1), COL_BAD(1.0f, 0.42f, 0.42f, 1),
    COL_WARN(1.0f, 0.78f, 0.30f, 1), COL_ACCENT(0.60f, 0.56f, 1.0f, 1);

static std::string fmt_time(int64_t t) {
    if (t <= 0) return "-";
    time_t tt = time_t(t);
    struct tm tm;
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    char b[32];
    strftime(b, sizeof b, "%Y-%m-%d %H:%M", &tm);
    return b;
}
static std::string fmt_bytes(double b) {
    const char* u[] = {"B", "KB", "MB", "GB", "TB"};
    int i = 0;
    while (b >= 1000 && i < 4) { b /= 1000; i++; }
    char s[32];
    snprintf(s, sizeof s, i ? "%.1f %s" : "%.0f %s", b, u[i]);
    return s;
}
static std::string fmt_hashrate(double h) {
    const char* u[] = {"H/s", "KH/s", "MH/s", "GH/s", "TH/s", "PH/s"};
    int i = 0;
    while (h >= 1000 && i < 5) { h /= 1000; i++; }
    char s[32];
    snprintf(s, sizeof s, "%.2f %s", h, u[i]);
    return s;
}
static std::string short_hash(const std::string& h) { return h.size() > 20 ? h.substr(0, 10) + ".." + h.substr(h.size() - 8) : h; }

static void draw_qr(const std::string& text, float size) {
    using qrcodegen::QrCode;
    std::string up = text;
    for (auto& c : up) c = char(toupper((unsigned char)c)); // bech32 uppercase => compact alphanumeric QR
    QrCode qr = QrCode::encodeText(up.c_str(), QrCode::Ecc::MEDIUM);
    int n = qr.getSize();
    float quiet = 4;
    float cell = size / (n + 2 * quiet);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size), IM_COL32(255, 255, 255, 255), 8);
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
            if (qr.getModule(x, y)) {
                ImVec2 a(p.x + (x + quiet) * cell, p.y + (y + quiet) * cell);
                dl->AddRectFilled(a, ImVec2(a.x + cell + 0.5f, a.y + cell + 0.5f), IM_COL32(10, 10, 20, 255));
            }
    ImGui::Dummy(ImVec2(size, size));
}

static void kv(const char* k, const char* fmt, ...) {
    ImGui::TextColored(ImVec4(0.60f, 0.62f, 0.70f, 1), "%s", k);
    ImGui::SameLine(ImGui::GetFontSize() * 9.5f);
    va_list ap;
    va_start(ap, fmt);
    ImGui::PushTextWrapPos(0);
    ImGui::TextWrappedV(fmt, ap);
    ImGui::PopTextWrapPos();
    va_end(ap);
}

static void help_marker(const char* t) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) { ImGui::BeginTooltip(); ImGui::PushTextWrapPos(420); ImGui::TextUnformatted(t); ImGui::PopTextWrapPos(); ImGui::EndTooltip(); }
}

static bool copy_button(const char* id, const std::string& text) {
    ImGui::PushID(id);
    bool c = ImGui::SmallButton("Copy");
    if (c) ImGui::SetClipboardText(text.c_str());
    ImGui::PopID();
    return c;
}

static void big_text(const std::string& s, ImVec4 col = ImVec4(1, 1, 1, 1)) {
    ImGui::PushFont(g_big);
    ImGui::TextColored(col, "%s", s.c_str());
    ImGui::PopFont();
}

// ============================================================================ lifecycle
App::App(int argc, char** argv) {
    std::string dd = default_datadir();
    snprintf(datadir_, sizeof datadir_, "%s", dd.c_str());
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "-regtest") net_choice_ = 1;
        else if (a == "-mainnet") net_choice_ = 2;
        else if (a.rfind("-datadir=", 0) == 0) snprintf(datadir_, sizeof datadir_, "%s", a.substr(9).c_str());
        else if (a.rfind("-addnode=", 0) == 0) snprintf(addnode_, sizeof addnode_, "%s", a.substr(9).c_str());
        else if (a.rfind("-shots=", 0) == 0) shots_dir_ = a.substr(7);
    }
    unsigned hc = std::thread::hardware_concurrency();
    mine_threads_ = std::max(1, int(hc ? hc / 2 : 1));
}

void App::shutdown() {
    if (starting_.valid()) starting_.wait();
    if (node_) node_->stop();
    node_.reset();
}

void App::start_node() {
    cfg_ = NodeConfig{};
    cfg_.net = net_choice_ == 1 ? Network::Regtest : net_choice_ == 2 ? Network::Main : Network::Test;
    cfg_.datadir = datadir_;
    cfg_.log_stdout = false;
    if (addnode_[0]) cfg_.p2p.addnodes.push_back(addnode_);
    if (cfg_.net == Network::Regtest) { cfg_.p2p.dht = false; cfg_.p2p.upnp = false; }
    cfg_.p2p.agent = "quant-qt:0.1.0";
    node_ = std::make_unique<Node>(cfg_);
    screen_ = Screen::Starting;
    starting_ = std::async(std::launch::async, [this] {
        std::string err;
        if (!node_->start(&err)) return err.empty() ? std::string("failed to start") : err;
        return std::string();
    });
}

void App::toast(const std::string& msg, bool error) { toasts_.push_back({msg, error, ImGui::GetTime() + 4.5}); }

void App::tick_background() {}

void App::refresh() {
    if (!node_) return;
    Snapshot s;
    Chainstate& cs = node_->chain();
    {
        std::lock_guard l(cs.mu);
        s.chain = cs.stats();
        s.syncing = cs.is_initial_sync();
        s.mempool = node_->mempool().size();
        for (int64_t h = cs.height(); h >= 0 && h > cs.height() - 25; h--) {
            BlockIndex* b = cs.at_height(h);
            Block blk;
            size_t ntx = 0;
            std::string reward = "-";
            if (cs.read_block(b, blk, false)) { ntx = blk.txs.size(); reward = format_amount(blk.txs[0].total_out()); }
            s.recent.push_back({h, b->hash.hex(), b->header.time, ntx, reward, !cs.have_witness(b)});
        }
        if (Wallet* w = node_->wallet()) {
            s.bal = w->balance(s.chain.height);
            s.history = w->history();
            s.contacts = w->contacts;
            for (auto* k : w->issued_keys()) {
                Amount bal = 0;
                for (auto& [op, c] : cs.coins_for_address(k->addr)) bal += c.out.value;
                s.addresses.push_back({w->address_string(k->addr), k->label, bal, k->index});
            }
            for (auto it = s.addresses.rbegin(); it != s.addresses.rend(); ++it)
                if (it->label != "(change)") { s.receive_address = it->address; break; }
        }
    }
    if (P2P* p = node_->p2p()) { s.net = p->stats(); s.peers = p->peers(); }
    s.mining = node_->miner().running();
    s.threads = node_->miner().threads();
    s.hashrate = node_->miner().hashrate();
    s.found = node_->miner().blocks_found();
    snap_ = std::move(s);
    hash_hist_.push_back(float(snap_.hashrate));
    if (hash_hist_.size() > 120) hash_hist_.erase(hash_hist_.begin());
}

// ============================================================================ frame
void App::drive_shots() {
    // Walks through every screen on a throwaway regtest chain and saves a PNG of each.
    static const std::pair<Page, const char*> pages[] = {
        {Page::Overview, "overview"}, {Page::Send, "send"}, {Page::Receive, "receive"}, {Page::History, "history"},
        {Page::Mining, "mining"}, {Page::Network, "network"}, {Page::Explorer, "explorer"}, {Page::Console, "console"},
        {Page::Settings, "settings"},
    };
    constexpr int NPAGES = int(sizeof pages / sizeof pages[0]);
    shot_frames_++;
    if (shot_step_ == 0) {
        if (shot_frames_ == 20) capture_ = shots_dir_ + "/00_start.png";
        if (shot_frames_ == 25) {
            net_choice_ = 1;
            std::string d = shots_dir_ + "/data";
            std::error_code ec;
            std::filesystem::remove_all(d, ec);
            snprintf(datadir_, sizeof datadir_, "%s", d.c_str());
            start_node();
            shot_step_ = 1;
            shot_frames_ = 0;
        }
        return;
    }
    if (shot_step_ == 1) {
        if (screen_ == Screen::WalletSetup && shot_frames_ > 10) {
            if (wallet_mode_ == 0) { new_words_ = mnemonic_generate(); wallet_mode_ = 1; }
            if (shot_frames_ == 20) capture_ = shots_dir_ + "/01_wallet.png";
            if (shot_frames_ == 25) {
                std::string err;
                node_->create_wallet(new_words_, "", "", false, &err);
                node_->rpc("generate", json::array({"112"}));
                std::string to = node_->rpc("getnewaddress", json::array({"savings"})).get<std::string>();
                node_->rpc("send", json::array({to, "3.1415926535"}));
                node_->rpc("addcontact", json::array({"Pi node", to}));
                node_->rpc("generate", json::array({"1"}));
                std::string other = node_->rpc("getnewaddress", json::array({"friend"})).get<std::string>();
                node_->rpc("send", json::array({other, "0.25"}));
                node_->start_mining(2, "", &err);
                console_run("getblockchaininfo");
                screen_ = Screen::Main;
                shot_step_ = 2;
                shot_frames_ = 0;
            }
        }
        return;
    }
    int idx = shot_step_ - 2;
    if (idx >= NPAGES) { if (shot_frames_ > 5) quit_ = true; return; }
    if (shot_frames_ == 1) {
        page_ = pages[idx].first;
        last_refresh_ = -10;
        if (page_ == Page::Explorer) explorer_search("113");
    }
    if (shot_frames_ == 90) {
        char name[64];
        snprintf(name, sizeof name, "/%02d_%s.png", idx + 2, pages[idx].second);
        capture_ = shots_dir_ + name;
    }
    if (shot_frames_ > 92) { shot_step_++; shot_frames_ = 0; }
}

void App::frame() {
    if (!shots_dir_.empty()) drive_shots();
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("##root", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus);
    switch (screen_) {
    case Screen::Start: draw_start(); break;
    case Screen::Starting: draw_starting(); break;
    case Screen::WalletSetup: draw_wallet_setup(); break;
    case Screen::Main: draw_main(); break;
    case Screen::Failed:
        big_text("Could not start the node", COL_BAD);
        ImGui::TextWrapped("%s", fail_msg_.c_str());
        if (ImGui::Button("Back")) { node_.reset(); screen_ = Screen::Start; }
        break;
    }
    ImGui::End();
    draw_toasts();
}

void App::draw_toasts() {
    double now = ImGui::GetTime();
    while (!toasts_.empty() && toasts_.front().until < now) toasts_.pop_front();
    ImGuiViewport* vp = ImGui::GetMainViewport();
    float y = vp->WorkPos.y + vp->WorkSize.y - 20;
    for (auto it = toasts_.rbegin(); it != toasts_.rend(); ++it) {
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - 20, y), 0, ImVec2(1, 1));
        ImGui::SetNextWindowBgAlpha(0.95f);
        char id[32];
        snprintf(id, sizeof id, "##toast%p", (void*)&*it);
        ImGui::Begin(id, nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
        ImGui::PushTextWrapPos(460);
        ImGui::TextColored(it->error ? COL_BAD : COL_GOOD, "%s", it->msg.c_str());
        ImGui::PopTextWrapPos();
        y -= ImGui::GetWindowHeight() + 8;
        ImGui::End();
    }
}

// ============================================================================ start / wallet setup
void App::draw_start() {
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::SetCursorPos(ImVec2(avail.x * 0.5f - 300, avail.y * 0.18f));
    ImGui::BeginChild("start", ImVec2(600, 520), true);
    big_text("QUANT", COL_ACCENT);
    ImGui::TextColored(COL_DIM, "Post-quantum peer-to-peer money  -  full node wallet");
    ImGui::Separator();
    ImGui::Text("Network");
    ImGui::RadioButton("Testnet (coins have no value - recommended for now)", &net_choice_, 0);
    ImGui::RadioButton("Regtest (private local chain, instant blocks)", &net_choice_, 1);
    bool main_ok = params_for(Network::Main).launched;
    ImGui::BeginDisabled(!main_ok);
    ImGui::RadioButton(main_ok ? "Mainnet" : "Mainnet (not launched yet)", &net_choice_, 2);
    ImGui::EndDisabled();
    ImGui::Spacing();
    ImGui::Text("Data folder");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##dd", datadir_, sizeof datadir_);
    ImGui::Text("Connect to a friend's node (optional)");
    help_marker("Normally not needed: Quant finds peers by itself through the BitTorrent DHT and your local network.");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##an", "ip:port", addnode_, sizeof addnode_);
    ImGui::Spacing();
    ImGui::TextColored(COL_DIM, "No servers: peers are discovered via the BitTorrent DHT,\nLAN broadcast and addresses remembered from last time.");
    ImGui::Spacing();
    if (ImGui::Button("Start node", ImVec2(-1, 44))) start_node();
    ImGui::EndChild();
}

void App::draw_starting() {
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::SetCursorPos(ImVec2(avail.x * 0.5f - 160, avail.y * 0.4f));
    ImGui::BeginGroup();
    big_text("Starting Quant...", COL_ACCENT);
    const char* spin = "|/-\\";
    kv("Loading the blockchain", "%c", spin[int(ImGui::GetTime() * 8) % 4]);
    ImGui::EndGroup();
    if (starting_.valid() && starting_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        std::string err = starting_.get();
        if (!err.empty()) { fail_msg_ = err; screen_ = Screen::Failed; return; }
        if (node_->wallet_loaded()) screen_ = Screen::Main;
        else { screen_ = Screen::WalletSetup; wallet_mode_ = node_->has_wallet_file() ? 3 : 0; }
        refresh();
    }
}

void App::draw_wallet_setup() {
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::SetCursorPos(ImVec2(avail.x * 0.5f - 340, avail.y * 0.12f));
    ImGui::BeginChild("ws", ImVec2(680, 600), true);
    big_text("Wallet", COL_ACCENT);
    ImGui::Separator();
    auto passwords = [&](bool confirm) {
        ImGui::Text("Wallet file password (optional, encrypts the file on this computer)");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##pw", password_, sizeof password_, ImGuiInputTextFlags_Password);
        if (confirm) {
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##pw2", "repeat password", password2_, sizeof password2_, ImGuiInputTextFlags_Password);
        }
    };
    if (wallet_mode_ == 0) {
        ImGui::TextWrapped("Your wallet is 24 seed words. The same words work in this app, in quantd and in the Termux phone wallet.");
        ImGui::Spacing();
        if (ImGui::Button("Create a new wallet", ImVec2(-1, 48))) { new_words_ = mnemonic_generate(); words_confirmed_ = false; wallet_mode_ = 1; }
        if (ImGui::Button("Restore from 24 seed words", ImVec2(-1, 48))) wallet_mode_ = 2;
        ImGui::Spacing();
        if (ImGui::Button("Skip (node only, no wallet)", ImVec2(-1, 36))) screen_ = Screen::Main;
    } else if (wallet_mode_ == 1) {
        ImGui::TextColored(COL_WARN, "Write these 24 words down on paper, in order. Anyone with them can spend your QNT.");
        ImGui::BeginChild("words", ImVec2(-1, 170), true);
        std::istringstream is(new_words_);
        std::string w;
        int i = 0;
        ImGui::PushFont(g_mono);
        if (ImGui::BeginTable("wt", 4)) {
            while (is >> w) { ImGui::TableNextColumn(); ImGui::Text("%2d. %s", ++i, w.c_str()); }
            ImGui::EndTable();
        }
        ImGui::PopFont();
        ImGui::EndChild();
        if (ImGui::SmallButton("Copy words")) ImGui::SetClipboardText(new_words_.c_str());
        ImGui::Text("Extra passphrase (optional \"25th word\" - if you use it you need it to restore)");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##pp", passphrase_, sizeof passphrase_, ImGuiInputTextFlags_Password);
        passwords(true);
        ImGui::Checkbox("I wrote my 24 words down", &words_confirmed_);
        ImGui::BeginDisabled(!words_confirmed_ || strcmp(password_, password2_) != 0);
        if (ImGui::Button("Create wallet", ImVec2(-1, 44))) {
            if (node_->create_wallet(new_words_, passphrase_, password_, false, &wallet_err_)) { screen_ = Screen::Main; new_words_.clear(); refresh(); }
        }
        ImGui::EndDisabled();
        if (strcmp(password_, password2_) != 0) ImGui::TextColored(COL_BAD, "Passwords don't match");
    } else if (wallet_mode_ == 2) {
        ImGui::Text("Seed words");
        ImGui::InputTextMultiline("##rw", restore_words_, sizeof restore_words_, ImVec2(-1, 110));
        bool valid = mnemonic_valid(restore_words_);
        ImGui::TextColored(valid ? COL_GOOD : COL_DIM, valid ? "Valid seed words" : "Enter all 24 words separated by spaces");
        ImGui::Text("Extra passphrase (only if you set one)");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##pp", passphrase_, sizeof passphrase_, ImGuiInputTextFlags_Password);
        passwords(true);
        ImGui::BeginDisabled(!valid || strcmp(password_, password2_) != 0);
        if (ImGui::Button("Restore wallet", ImVec2(-1, 44))) {
            if (node_->create_wallet(restore_words_, passphrase_, password_, true, &wallet_err_)) { screen_ = Screen::Main; memset(restore_words_, 0, sizeof restore_words_); refresh(); }
        }
        ImGui::EndDisabled();
    } else if (wallet_mode_ == 3) {
        ImGui::Text("Unlock your wallet");
        passwords(false);
        if (ImGui::Button("Open", ImVec2(-1, 44)) || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
            if (node_->open_wallet(password_, &wallet_err_)) { screen_ = Screen::Main; refresh(); }
        }
    }
    if (!wallet_err_.empty()) ImGui::TextColored(COL_BAD, "%s", wallet_err_.c_str());
    if (wallet_mode_ == 1 || wallet_mode_ == 2) if (ImGui::Button("Back")) { wallet_mode_ = 0; wallet_err_.clear(); }
    ImGui::EndChild();
}

// ============================================================================ main layout
void App::draw_main() {
    if (ImGui::GetTime() - last_refresh_ > 1.0) { last_refresh_ = ImGui::GetTime(); refresh(); }
    if (node_->stop_requested()) quit_ = true;
    draw_statusbar();
    ImGui::BeginChild("sidebar", ImVec2(190, 0), true);
    draw_sidebar();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("page", ImVec2(0, 0), false);
    switch (page_) {
    case Page::Overview: page_overview(); break;
    case Page::Send: page_send(); break;
    case Page::Receive: page_receive(); break;
    case Page::History: page_history(); break;
    case Page::Contacts: page_contacts(); break;
    case Page::Mining: page_mining(); break;
    case Page::Network: page_network(); break;
    case Page::Explorer: page_explorer(); break;
    case Page::Console: page_console(); break;
    case Page::Settings: page_settings(); break;
    }
    ImGui::EndChild();
}

void App::draw_statusbar() {
    ImGui::BeginChild("status", ImVec2(0, 44), true, ImGuiWindowFlags_NoScrollbar);
    ImGui::TextColored(COL_ACCENT, "QUANT");
    ImGui::SameLine();
    ImGui::TextColored(COL_DIM, "%s", node_->params().name.c_str());
    ImGui::SameLine(0, 30);
    auto& c = snap_.chain;
    if (snap_.syncing && c.header_height > c.height) {
        float f = c.header_height ? float(c.height) / float(c.header_height) : 0;
        ImGui::Text("Syncing");
        ImGui::SameLine();
        char ov[64];
        snprintf(ov, sizeof ov, "%lld / %lld", (long long)c.height, (long long)c.header_height);
        ImGui::ProgressBar(f, ImVec2(220, 0), ov);
    } else {
        ImGui::Text("Block %lld", (long long)c.height);
    }
    ImGui::SameLine(0, 30);
    size_t np = snap_.peers.size();
    ImGui::TextColored(np ? COL_GOOD : COL_WARN, "%zu peer%s", np, np == 1 ? "" : "s");
    ImGui::SameLine(0, 30);
    if (snap_.mining) ImGui::TextColored(COL_GOOD, "Mining %s", fmt_hashrate(snap_.hashrate).c_str());
    else ImGui::TextColored(COL_DIM, "Not mining");
    if (node_->wallet()) {
        std::string bal = format_amount(snap_.bal.confirmed) + " QNT";
        ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize(bal.c_str()).x - 20);
        ImGui::Text("%s", bal.c_str());
    }
    ImGui::EndChild();
}

void App::draw_sidebar() {
    struct Item { Page p; const char* name; bool wallet; };
    static const Item items[] = {
        {Page::Overview, "Overview", false}, {Page::Send, "Send", true}, {Page::Receive, "Receive", true},
        {Page::History, "Transactions", true}, {Page::Contacts, "Address book", true}, {Page::Mining, "Mining", false},
        {Page::Network, "Network", false}, {Page::Explorer, "Explorer", false}, {Page::Console, "Console", false},
        {Page::Settings, "Settings", false},
    };
    for (auto& it : items) {
        if (it.wallet && !node_->wallet()) continue;
        if (ImGui::Selectable(it.name, page_ == it.p, 0, ImVec2(0, 34))) page_ = it.p;
    }
    if (!node_->wallet()) {
        ImGui::Separator();
        if (ImGui::Button("Set up wallet", ImVec2(-1, 0))) { screen_ = Screen::WalletSetup; wallet_mode_ = node_->has_wallet_file() ? 3 : 0; }
    }
}

// ============================================================================ pages
void App::page_overview() {
    if (node_->wallet()) {
        ImGui::BeginChild("bal", ImVec2(0, 150), true);
        ImGui::TextColored(COL_DIM, "Available balance");
        big_text(format_amount(snap_.bal.confirmed) + " QNT");
        ImGui::TextColored(COL_DIM, "Pending: %s    Immature (mined, <100 blocks): %s    Sending: %s",
                           format_amount(snap_.bal.unconfirmed).c_str(), format_amount(snap_.bal.immature).c_str(),
                           format_amount(snap_.bal.locked).c_str());
        if (ImGui::Button("Send")) page_ = Page::Send;
        ImGui::SameLine();
        if (ImGui::Button("Receive")) page_ = Page::Receive;
        ImGui::SameLine();
        if (ImGui::Button(snap_.mining ? "Mining..." : "Start mining")) page_ = Page::Mining;
        ImGui::EndChild();
    }
    float half = (ImGui::GetContentRegionAvail().x - 10) * 0.5f;
    ImGui::BeginChild("chain", ImVec2(half, 230), true);
    ImGui::TextColored(COL_ACCENT, "Blockchain");
    auto& c = snap_.chain;
    kv("Height", "%lld", (long long)c.height);
    kv("Difficulty", "%.3f", c.difficulty);
    kv("Block reward", "%s QNT", format_amount(block_subsidy(uint64_t(c.height + 1))).c_str());
    kv("Supply", "%s QNT", format_amount(c.supply).c_str());
    kv("Chain on disk", "%s  (signatures %s)", fmt_bytes(double(c.disk_blocks + c.disk_headers)).c_str(), fmt_bytes(double(c.disk_witness)).c_str());
    kv("Mempool", "%zu tx", snap_.mempool);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("netov", ImVec2(half, 230), true);
    ImGui::TextColored(COL_ACCENT, "Network");
    kv("Peers", "%d out / %d in", snap_.net.outbound, snap_.net.inbound);
    kv("DHT", "%s", snap_.net.dht_status.c_str());
    kv("Router", "%s", snap_.net.upnp_status.c_str());
    kv("Public address", "%s", snap_.net.external_addr.c_str());
    ImGui::EndChild();
    if (node_->wallet()) {
        ImGui::TextColored(COL_ACCENT, "Recent activity");
        if (ImGui::BeginTable("recent", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
            int n = 0;
            for (auto& t : snap_.history) {
                if (++n > 8) break;
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextColored(COL_DIM, "%s", fmt_time(t.time).c_str());
                ImGui::TableNextColumn(); ImGui::Text("%s", t.coinbase ? "Mined" : t.delta < 0 ? "Sent" : "Received");
                ImGui::TableNextColumn();
                std::string a = (t.delta < 0 ? "-" : "+") + format_amount(Amount(t.delta < 0 ? -t.delta : t.delta));
                ImGui::TextColored(t.delta < 0 ? COL_BAD : COL_GOOD, "%s QNT", a.c_str());
                ImGui::TableNextColumn();
                if (t.height < 0) ImGui::TextColored(COL_WARN, "pending");
                else ImGui::TextColored(COL_DIM, "%lld conf", (long long)(snap_.chain.height - t.height + 1));
            }
            ImGui::EndTable();
        }
        if (snap_.history.empty()) ImGui::TextColored(COL_DIM, "No transactions yet. Mine some testnet QNT on the Mining page!");
    }
}

void App::page_send() {
    Wallet* w = node_->wallet();
    big_text("Send QNT");
    ImGui::Text("Available: %s QNT", format_amount(snap_.bal.confirmed).c_str());
    ImGui::Spacing();
    ImGui::Text("Pay to");
    ImGui::SetNextItemWidth(-120);
    ImGui::InputTextWithHint("##to", (node_->params().hrp + "1...").c_str(), send_to_, sizeof send_to_);
    ImGui::SameLine();
    if (ImGui::Button("Contacts")) ImGui::OpenPopup("pick");
    if (ImGui::BeginPopup("pick")) {
        for (auto& c : snap_.contacts)
            if (ImGui::Selectable((c.name + "   " + short_hash(c.address)).c_str())) snprintf(send_to_, sizeof send_to_, "%s", c.address.c_str());
        if (snap_.contacts.empty()) ImGui::TextColored(COL_DIM, "Address book is empty");
        ImGui::EndPopup();
    }
    uint8_t v; Hash256 h;
    bool addr_ok = decode_address(node_->params(), send_to_, v, h);
    if (send_to_[0] && !addr_ok) ImGui::TextColored(COL_BAD, "Not a valid %s address", node_->params().name.c_str());
    ImGui::Text("Amount (QNT)");
    ImGui::SetNextItemWidth(260);
    ImGui::InputTextWithHint("##amt", "0.0", send_amount_, sizeof send_amount_, ImGuiInputTextFlags_CharsDecimal);
    ImGui::SameLine();
    if (ImGui::SmallButton("Max")) { std::string m = format_amount(snap_.bal.confirmed); snprintf(send_amount_, sizeof send_amount_, "%s", m.c_str()); send_subtract_ = true; }
    auto amt = parse_amount(send_amount_);
    if (send_amount_[0] && !amt) ImGui::TextColored(COL_BAD, "Invalid amount (max 10 decimals)");
    else if (amt && *amt < DUST_LIMIT) ImGui::TextColored(COL_BAD, "Minimum is 0.000001 QNT");
    ImGui::Checkbox("Subtract fee from amount", &send_subtract_);
    ImGui::Text("Note (private, stored in your wallet)");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##note", send_note_, sizeof send_note_);
    ImGui::Text("Fee rate (QNT per KB, blank = minimum 0.00001)");
    ImGui::SetNextItemWidth(260);
    ImGui::InputTextWithHint("##fee", "0.00001", send_fee_, sizeof send_fee_, ImGuiInputTextFlags_CharsDecimal);
    ImGui::Spacing();
    ImGui::BeginDisabled(!addr_ok || !amt || *amt < DUST_LIMIT);
    if (ImGui::Button("Review payment", ImVec2(240, 42))) {
        send_err_.clear();
        Amount feekb = send_fee_[0] ? parse_amount(send_fee_).value_or(0) : 0;
        if (!w->create_tx({{send_to_, *amt}}, feekb, snap_.chain.height, pending_tx_, &pending_fee_, &send_err_, false, send_subtract_))
            toast(send_err_, true);
        else { send_confirm_open_ = true; ImGui::OpenPopup("Confirm payment"); }
    }
    ImGui::EndDisabled();
    if (ImGui::BeginPopupModal("Confirm payment", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        Amount out_to = 0;
        for (auto& o : pending_tx_.outs) if (o.addr == h) out_to += o.value;
        ImGui::Text("Send");
        big_text(format_amount(out_to) + " QNT");
        ImGui::Text("to  %s", send_to_);
        ImGui::Text("Network fee: %s QNT   (%zu bytes)", format_amount(pending_fee_).c_str(), pending_tx_.full_size());
        ImGui::TextColored(COL_DIM, "Signed with Falcon-512 (post-quantum)");
        ImGui::Spacing();
        if (ImGui::Button("Send now", ImVec2(160, 38))) {
            try {
                std::string id = node_->broadcast(pending_tx_, send_note_);
                toast("Sent! txid " + short_hash(id));
                send_to_[0] = send_amount_[0] = send_note_[0] = 0;
                send_subtract_ = false;
            } catch (const std::exception& e) {
                toast(e.what(), true);
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 38))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void App::page_receive() {
    big_text("Receive QNT");
    ImGui::TextColored(COL_DIM, "Share this address or QR code. A fresh address each time improves privacy.");
    const std::string& a = snap_.receive_address;
    if (!a.empty()) {
        ImGui::BeginChild("qr", ImVec2(0, 290), true);
        draw_qr(a, 260);
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::PushFont(g_mono);
        ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + 440);
        ImGui::TextWrapped("%s", a.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
        if (ImGui::Button("Copy address")) { ImGui::SetClipboardText(a.c_str()); toast("Address copied"); }
        ImGui::Spacing();
        ImGui::SetNextItemWidth(240);
        ImGui::InputTextWithHint("##lbl", "label (optional)", new_label_, sizeof new_label_);
        ImGui::SameLine();
        if (ImGui::Button("New address")) {
            node_->wallet()->new_address(new_label_);
            node_->wallet()->save();
            new_label_[0] = 0;
            refresh();
        }
        ImGui::EndGroup();
        ImGui::EndChild();
    }
    ImGui::TextColored(COL_ACCENT, "Your addresses");
    if (ImGui::BeginTable("addrs", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 140);
        ImGui::TableSetupColumn("Address");
        ImGui::TableSetupColumn("Balance", ImGuiTableColumnFlags_WidthFixed, 160);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableHeadersRow();
        for (auto it = snap_.addresses.rbegin(); it != snap_.addresses.rend(); ++it) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%s", it->label.c_str());
            ImGui::TableNextColumn(); ImGui::PushFont(g_mono); ImGui::Text("%s", it->address.c_str()); ImGui::PopFont();
            ImGui::TableNextColumn(); ImGui::Text("%s", format_amount(it->balance).c_str());
            ImGui::TableNextColumn(); copy_button(it->address.c_str(), it->address);
        }
        ImGui::EndTable();
    }
}

void App::page_history() {
    big_text("Transactions");
    if (ImGui::BeginTable("hist", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("Date", ImGuiTableColumnFlags_WidthFixed, 140);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Amount", ImGuiTableColumnFlags_WidthFixed, 190);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 110);
        ImGui::TableSetupColumn("To / note");
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        for (auto& t : snap_.history) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%s", fmt_time(t.time).c_str());
            ImGui::TableNextColumn(); ImGui::Text("%s", t.coinbase ? "Mined" : t.delta < 0 ? "Sent" : "Received");
            ImGui::TableNextColumn();
            std::string a = (t.delta < 0 ? "-" : "+") + format_amount(Amount(t.delta < 0 ? -t.delta : t.delta));
            ImGui::TextColored(t.delta < 0 ? COL_BAD : COL_GOOD, "%s", a.c_str());
            ImGui::TableNextColumn();
            int64_t conf = t.height < 0 ? 0 : snap_.chain.height - t.height + 1;
            if (conf == 0) ImGui::TextColored(COL_WARN, "pending");
            else if (t.coinbase && conf < COINBASE_MATURITY) ImGui::TextColored(COL_WARN, "matures in %lld", (long long)(COINBASE_MATURITY - conf));
            else ImGui::TextColored(conf >= 30 ? COL_GOOD : COL_DIM, "%lld conf", (long long)conf);
            ImGui::TableNextColumn();
            std::string d = t.note.empty() ? (t.delta < 0 ? short_hash(t.counterparty) : "") : t.note;
            if (t.fee && t.delta < 0) d += "  (fee " + format_amount(t.fee) + ")";
            ImGui::Text("%s", d.c_str());
            ImGui::TableNextColumn();
            ImGui::PushID(t.txid.hex().c_str());
            if (ImGui::SmallButton("View")) { page_ = Page::Explorer; explorer_search(t.txid.hex()); }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void App::page_contacts() {
    big_text("Address book");
    ImGui::SetNextItemWidth(180);
    ImGui::InputTextWithHint("##cn", "name", contact_name_, sizeof contact_name_);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-90);
    ImGui::InputTextWithHint("##ca", "address", contact_addr_, sizeof contact_addr_);
    ImGui::SameLine();
    if (ImGui::Button("Add")) {
        try { node_->rpc("addcontact", json::array({contact_name_, contact_addr_})); contact_name_[0] = contact_addr_[0] = 0; refresh(); }
        catch (const std::exception& e) { toast(e.what(), true); }
    }
    if (ImGui::BeginTable("ct", 3, ImGuiTableFlags_RowBg)) {
        for (auto& c : snap_.contacts) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%s", c.name.c_str());
            ImGui::TableNextColumn(); ImGui::PushFont(g_mono); ImGui::Text("%s", c.address.c_str()); ImGui::PopFont();
            ImGui::TableNextColumn();
            ImGui::PushID(c.name.c_str());
            if (ImGui::SmallButton("Pay")) { snprintf(send_to_, sizeof send_to_, "%s", c.address.c_str()); page_ = Page::Send; }
            ImGui::SameLine();
            if (ImGui::SmallButton("Remove")) { node_->rpc("removecontact", json::array({c.name})); refresh(); }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void App::page_mining() {
    big_text("Mining");
    ImGui::TextColored(COL_DIM, "BLAKE3 proof-of-work. Every block pays %s QNT + fees; mined coins can be spent after 100 blocks.",
                       format_amount(block_subsidy(uint64_t(snap_.chain.height + 1))).c_str());
    ImGui::BeginChild("mctl", ImVec2(0, 190), true);
    int maxt = int(std::max(1u, std::thread::hardware_concurrency()));
    ImGui::SetNextItemWidth(300);
    ImGui::SliderInt("CPU threads", &mine_threads_, 1, maxt);
    ImGui::SetNextItemWidth(-1);
    std::string hint = node_->wallet() ? "reward address (blank = your wallet)" : "reward address (required without a wallet)";
    ImGui::InputTextWithHint("##ma", hint.c_str(), mine_addr_, sizeof mine_addr_);
    if (!snap_.mining) {
        if (ImGui::Button("Start mining", ImVec2(200, 44))) {
            std::string err;
            if (!node_->start_mining(mine_threads_, mine_addr_, &err)) toast(err, true);
            else toast("Mining started with " + std::to_string(mine_threads_) + " threads");
        }
    } else {
        if (ImGui::Button("Stop mining", ImVec2(200, 44))) node_->stop_mining();
        ImGui::SameLine();
        if (ImGui::Button("Apply thread count", ImVec2(200, 44))) { std::string err; node_->start_mining(mine_threads_, mine_addr_, &err); }
    }
    if (snap_.syncing && snap_.chain.header_height > snap_.chain.height)
        ImGui::TextColored(COL_WARN, "Still syncing - blocks you mine now will likely be orphaned.");
    ImGui::EndChild();
    ImGui::BeginChild("mstat", ImVec2(0, 0), true);
    kv("Your hashrate", "%s", fmt_hashrate(snap_.hashrate).c_str());
    kv("Blocks found", "%llu", (unsigned long long)snap_.found);
    double diff = snap_.chain.difficulty;
    double net_hr = diff * std::pow(2.0, 256 - node_->params().pow_limit.bits()) / TARGET_SPACING;
    kv("Network (est.)", "%s", fmt_hashrate(net_hr).c_str());
    if (snap_.hashrate > 0 && net_hr > 0) {
        double share = std::min(1.0, snap_.hashrate / std::max(net_hr, snap_.hashrate));
        double secs = TARGET_SPACING / share;
        ImGui::Text("Expected time/block %s", secs < 3600 ? (std::to_string(int(secs / 60)) + " min").c_str() : (std::to_string(int(secs / 3600)) + " h").c_str());
    }
    if (!hash_hist_.empty()) {
        float mx = *std::max_element(hash_hist_.begin(), hash_hist_.end());
        ImGui::PlotLines("##hr", hash_hist_.data(), int(hash_hist_.size()), 0, "hashrate (last 2 min)", 0, mx * 1.2f + 1, ImVec2(-1, 120));
    }
    ImGui::EndChild();
}

void App::page_network() {
    big_text("Network");
    ImGui::BeginChild("nstat", ImVec2(0, 200), true);
    auto& n = snap_.net;
    kv("Connections", "%d outbound, %d inbound", n.outbound, n.inbound);
    kv("Discovery (DHT)", "%s", n.dht_status.c_str());
    kv("LAN discovery", "%s", n.lan_status.c_str());
    kv("Router (UPnP)", "%s", n.upnp_status.c_str());
    kv("Public address", "%s", n.external_addr.c_str());
    kv("Known addresses", "%zu", n.known_addrs);
    kv("Traffic", "%s in / %s out", fmt_bytes(double(n.bytes_in)).c_str(), fmt_bytes(double(n.bytes_out)).c_str());
    ImGui::EndChild();
    ImGui::SetNextItemWidth(300);
    ImGui::InputTextWithHint("##ap", "ip:port", add_peer_, sizeof add_peer_);
    ImGui::SameLine();
    if (ImGui::Button("Connect to node") && add_peer_[0]) { node_->p2p()->add_node(add_peer_); toast(std::string("Connecting to ") + add_peer_); add_peer_[0] = 0; }
    if (ImGui::BeginTable("peers", 8, ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY,
                          ImVec2(0, snap_.peers.empty() ? ImGui::GetFrameHeight() * 2 : 0))) {
        ImGui::TableSetupColumn("Address");
        ImGui::TableSetupColumn("Dir", ImGuiTableColumnFlags_WidthFixed, 40);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Height", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Ping", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Traffic", ImGuiTableColumnFlags_WidthFixed, 150);
        ImGui::TableSetupColumn("Client");
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 40);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        for (auto& p : snap_.peers) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%s", p.addr.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%s", p.inbound ? "in" : "out");
            ImGui::TableNextColumn(); ImGui::Text("%s", p.light ? "wallet" : "full node");
            ImGui::TableNextColumn(); ImGui::Text("%lld", (long long)p.height);
            ImGui::TableNextColumn(); if (p.ping_ms >= 0) ImGui::Text("%.0f ms", p.ping_ms); else ImGui::Text("-");
            ImGui::TableNextColumn(); ImGui::Text("%s / %s", fmt_bytes(double(p.bytes_in)).c_str(), fmt_bytes(double(p.bytes_out)).c_str());
            ImGui::TableNextColumn(); ImGui::Text("%s", p.agent.c_str());
            ImGui::TableNextColumn();
            ImGui::PushID(int(p.id));
            if (ImGui::SmallButton("x")) node_->p2p()->disconnect(p.id);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (snap_.peers.empty())
        ImGui::TextColored(COL_DIM, "Searching for peers... On a brand-new network there may be nobody else yet:\n"
                                    "you are the first node. Share your address (ip:%u) with friends or run a second node.",
                           cfg_.p2p.port ? cfg_.p2p.port : node_->params().p2p_port);
}

void App::explorer_search(const std::string& q_in) {
    std::string q = q_in;
    q.erase(0, q.find_first_not_of(" \t"));
    q.erase(q.find_last_not_of(" \t") + 1);
    explorer_err_.clear();
    if (!explorer_kind_.empty()) explorer_back_.push_back(search_);
    snprintf(search_, sizeof search_, "%s", q.c_str());
    try {
        bool digits = !q.empty() && std::all_of(q.begin(), q.end(), ::isdigit);
        if (digits) { explorer_result_ = node_->rpc("getblock", json::array({q, false})); explorer_kind_ = "block"; return; }
        if (q.size() == 64) {
            try { explorer_result_ = node_->rpc("getblock", json::array({q, false})); explorer_kind_ = "block"; return; } catch (...) {}
            explorer_result_ = node_->rpc("gettransaction", json::array({q}));
            explorer_kind_ = "tx";
            return;
        }
        explorer_result_ = node_->rpc("getaddressinfo", json::array({q}));
        if (!explorer_result_["valid"].get<bool>()) throw RpcError("not a block height, hash, txid or address");
        explorer_kind_ = "address";
    } catch (const std::exception& e) {
        explorer_err_ = e.what();
        explorer_kind_.clear();
    }
}

void App::page_explorer() {
    big_text("Explorer");
    ImGui::SetNextItemWidth(-200);
    bool go = ImGui::InputTextWithHint("##s", "block height, block hash, txid or address", search_, sizeof search_, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("Search") || go) explorer_search(search_);
    ImGui::SameLine();
    if (ImGui::Button("Latest")) { explorer_kind_.clear(); explorer_back_.clear(); search_[0] = 0; }
    if (!explorer_err_.empty()) ImGui::TextColored(COL_BAD, "%s", explorer_err_.c_str());
    auto link = [&](const std::string& label, const std::string& target) {
        ImGui::PushStyleColor(ImGuiCol_Text, COL_ACCENT);
        ImGui::PushID(target.c_str());
        ImGui::PushID(label.c_str());
        bool c = ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_AllowOverlap);
        ImGui::PopID();
        ImGui::PopID();
        ImGui::PopStyleColor();
        if (c) explorer_search(target);
    };
    ImGui::BeginChild("ex", ImVec2(0, 0), true);
    const json& r = explorer_result_;
    if (explorer_kind_.empty()) {
        ImGui::TextColored(COL_ACCENT, "Latest blocks");
        if (ImGui::BeginTable("lb", 5, ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Height", ImGuiTableColumnFlags_WidthFixed, 80);
            ImGui::TableSetupColumn("Hash");
            ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 140);
            ImGui::TableSetupColumn("Txs", ImGuiTableColumnFlags_WidthFixed, 50);
            ImGui::TableSetupColumn("Reward", ImGuiTableColumnFlags_WidthFixed, 120);
            ImGui::TableHeadersRow();
            for (auto& b : snap_.recent) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); link(std::to_string(b.height), std::to_string(b.height));
                ImGui::TableNextColumn(); ImGui::PushFont(g_mono); ImGui::Text("%s", b.hash.c_str()); ImGui::PopFont();
                ImGui::TableNextColumn(); ImGui::Text("%s", fmt_time(int64_t(b.time)).c_str());
                ImGui::TableNextColumn(); ImGui::Text("%zu", b.txs);
                ImGui::TableNextColumn(); ImGui::Text("%s", b.reward.c_str());
            }
            ImGui::EndTable();
        }
    } else if (explorer_kind_ == "block") {
        ImGui::TextColored(COL_ACCENT, "Block %lld", (long long)r["height"].get<int64_t>());
        ImGui::PushFont(g_mono);
        kv("Hash", "%s", r["hash"].get<std::string>().c_str());
        ImGui::PopFont();
        kv("Previous", ""); ImGui::SameLine(); link(r["prev"].get<std::string>(), r["prev"].get<std::string>());
        if (r.contains("next")) { kv("Next", ""); ImGui::SameLine(); link(r["next"].get<std::string>(), r["next"].get<std::string>()); }
        kv("Time", "%s", fmt_time(r["time"].get<int64_t>()).c_str());
        ImGui::Text("Confirmations %lld", (long long)r["confirmations"].get<int64_t>());
        kv("Difficulty", "%.4f", r["difficulty"].get<double>());
        kv("Nonce", "%llu", (unsigned long long)r["nonce"].get<uint64_t>());
        kv("Stored size", "%zu bytes%s", r["size_core"].get<size_t>(), r["signatures_pruned"].get<bool>() ? "  (signatures pruned)" : "  (+ signatures)");
        kv("Reward paid", "%s QNT", r["reward"].get<std::string>().c_str());
        ImGui::Separator();
        ImGui::Text("Transactions (%zu)", r["txs"].size());
        for (auto& t : r["txs"]) link(t.get<std::string>(), t.get<std::string>());
    } else if (explorer_kind_ == "tx") {
        ImGui::TextColored(COL_ACCENT, "Transaction");
        ImGui::PushFont(g_mono);
        ImGui::Text("%s", r["txid"].get<std::string>().c_str());
        ImGui::PopFont();
        int64_t conf = r.value("confirmations", 0);
        if (conf == 0) ImGui::TextColored(COL_WARN, "Unconfirmed (in mempool)");
        else { ImGui::Text("In block %lld,  %lld confirmations", (long long)r["height"].get<int64_t>(), (long long)conf); ImGui::SameLine(); link("open block", r["block"].get<std::string>()); }
        ImGui::Text("Size: %zu bytes stored forever + %zu bytes of signatures (pruned after ~1 week)", r["size_core"].get<size_t>(), r["size_witness"].get<size_t>());
        ImGui::Separator();
        float half = ImGui::GetContentRegionAvail().x * 0.5f - 6;
        ImGui::BeginChild("ins", ImVec2(half, 0), true);
        ImGui::Text("Inputs");
        if (r["coinbase"].get<bool>()) ImGui::TextColored(COL_GOOD, "New coins (block reward)  %s", r.value("miner_tag", "").c_str());
        for (auto& in : r["inputs"]) {
            std::string id = in["txid"].get<std::string>();
            link(short_hash(id) + ":" + std::to_string(in["vout"].get<int>()), id);
            if (in.contains("signature")) { ImGui::SameLine(); ImGui::TextColored(COL_DIM, "(%s)", in["signature"].get<std::string>().c_str()); }
        }
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("outs", ImVec2(0, 0), true);
        kv("Outputs", "(total %s)", r["total_out"].get<std::string>().c_str());
        for (auto& o : r["outputs"]) {
            link(o["address"].get<std::string>(), o["address"].get<std::string>());
            ImGui::SameLine();
            ImGui::Text("%s QNT", o["amount"].get<std::string>().c_str());
        }
        ImGui::EndChild();
    } else if (explorer_kind_ == "address") {
        ImGui::TextColored(COL_ACCENT, "Address%s", r["mine"].get<bool>() ? "  (yours)" : "");
        ImGui::PushFont(g_mono);
        ImGui::Text("%s", search_);
        ImGui::PopFont();
        big_text(r["balance"].get<std::string>() + " QNT");
        ImGui::Text("Unspent coins (%zu)", r["coins"].size());
        for (auto& c : r["coins"]) {
            link(short_hash(c["txid"].get<std::string>()) + ":" + std::to_string(c["vout"].get<int>()), c["txid"].get<std::string>());
            ImGui::SameLine();
            ImGui::Text("%s QNT  (block %lld)", c["amount"].get<std::string>().c_str(), (long long)c["height"].get<int64_t>());
        }
    }
    ImGui::EndChild();
}

void App::console_run(const std::string& line) {
    // split respecting quotes
    std::vector<std::string> parts;
    std::string cur;
    bool q = false;
    for (char c : line) {
        if (c == '"') { q = !q; continue; }
        if (c == ' ' && !q) { if (!cur.empty()) parts.push_back(cur), cur.clear(); continue; }
        cur += c;
    }
    if (!cur.empty()) parts.push_back(cur);
    if (parts.empty()) return;
    console_lines_.push_back({"> " + line, false});
    if (parts[0] == "clear") { console_lines_.clear(); return; }
    json params = json::array();
    for (size_t i = 1; i < parts.size(); i++) {
        try { json j = json::parse(parts[i]); if (j.is_boolean() || j.is_array() || j.is_object()) { params.push_back(j); continue; } } catch (...) {}
        params.push_back(parts[i]);
    }
    std::string method = parts[0];
    console_job_ = std::async(std::launch::async, [this, method, params]() -> std::pair<std::string, bool> {
        try {
            json r = node_->rpc(method, params);
            return {r.is_string() ? r.get<std::string>() : r.dump(2), false};
        } catch (const std::exception& e) {
            return {std::string("error: ") + e.what(), true};
        }
    });
}

void App::page_console() {
    big_text("Console");
    ImGui::TextColored(COL_DIM, "Same commands as quant-cli. Type 'help'. 'clear' empties the screen.");
    if (console_job_.valid() && console_job_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        auto [text, err] = console_job_.get();
        console_lines_.push_back({text, err});
        console_scroll_ = true;
    }
    float h = ImGui::GetContentRegionAvail().y - 50;
    ImGui::BeginChild("out", ImVec2(0, h), true, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::PushFont(g_mono);
    for (auto& [t, e] : console_lines_) {
        if (e) ImGui::TextColored(COL_BAD, "%s", t.c_str());
        else if (t.rfind("> ", 0) == 0) ImGui::TextColored(COL_ACCENT, "%s", t.c_str());
        else ImGui::TextUnformatted(t.c_str());
    }
    if (console_job_.valid()) ImGui::TextColored(COL_DIM, "running...");
    ImGui::PopFont();
    if (console_scroll_) { ImGui::SetScrollHereY(1.0f); console_scroll_ = false; }
    ImGui::EndChild();
    ImGui::SetNextItemWidth(-1);
    auto cb = [](ImGuiInputTextCallbackData* d) -> int {
        App* a = (App*)d->UserData;
        if (d->EventFlag == ImGuiInputTextFlags_CallbackHistory && !a->console_hist_.empty()) {
            if (d->EventKey == ImGuiKey_UpArrow) a->console_hist_pos_ = a->console_hist_pos_ < 0 ? int(a->console_hist_.size()) - 1 : std::max(0, a->console_hist_pos_ - 1);
            else if (d->EventKey == ImGuiKey_DownArrow && a->console_hist_pos_ >= 0) a->console_hist_pos_ = std::min(int(a->console_hist_.size()) - 1, a->console_hist_pos_ + 1);
            if (a->console_hist_pos_ >= 0) { d->DeleteChars(0, d->BufTextLen); d->InsertChars(0, a->console_hist_[a->console_hist_pos_].c_str()); }
        }
        if (d->EventFlag == ImGuiInputTextFlags_CallbackCompletion) {
            std::string pre(d->Buf, d->BufTextLen);
            for (auto& c : a->node_->rpc_commands()) if (c.rfind(pre, 0) == 0) { d->DeleteChars(0, d->BufTextLen); d->InsertChars(0, (c + " ").c_str()); break; }
        }
        return 0;
    };
    if (ImGui::InputTextWithHint("##in", "command (Tab completes, Up/Down history)", console_in_, sizeof console_in_,
                                 ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory | ImGuiInputTextFlags_CallbackCompletion, cb, this)) {
        std::string l = console_in_;
        if (!l.empty() && !console_job_.valid()) {
            console_hist_.push_back(l);
            console_hist_pos_ = -1;
            console_run(l);
            console_in_[0] = 0;
            console_scroll_ = true;
        }
        ImGui::SetKeyboardFocusHere(-1);
    }
}

void App::page_settings() {
    big_text("Settings");
    kv("Network", "%s", node_->params().name.c_str());
    kv("Data folder", "%s", node_->dir().c_str());
    kv("Pruning", "signatures deleted after %d blocks (~1 week)", PRUNE_DEPTH);
    ImGui::Separator();
    if (Wallet* w = node_->wallet()) {
        ImGui::TextColored(COL_ACCENT, "Seed words");
        ImGui::TextColored(COL_DIM, "Use these to restore on another PC or in the Termux wallet on your phone.");
        if (!show_seed_) { if (ImGui::Button("Show seed words")) ImGui::OpenPopup("seedwarn"); }
        else {
            ImGui::PushFont(g_mono);
            ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
            ImGui::TextWrapped("%s", w->mnemonic().c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopFont();
            if (ImGui::Button("Hide")) show_seed_ = false;
        }
        if (ImGui::BeginPopupModal("seedwarn", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextColored(COL_WARN, "Make sure nobody can see your screen.\nAnyone with these words owns your QNT.");
            if (ImGui::Button("Show", ImVec2(120, 0))) { show_seed_ = true; ImGui::CloseCurrentPopup(); }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::Separator();
        ImGui::TextColored(COL_ACCENT, "Wallet file password");
        ImGui::SetNextItemWidth(260);
        ImGui::InputTextWithHint("##op", "current password", old_pw_, sizeof old_pw_, ImGuiInputTextFlags_Password);
        ImGui::SetNextItemWidth(260);
        ImGui::InputTextWithHint("##np", "new password", new_pw_, sizeof new_pw_, ImGuiInputTextFlags_Password);
        if (ImGui::Button("Change password")) {
            if (w->change_password(old_pw_, new_pw_)) toast("Password changed");
            else toast("Current password is wrong", true);
            old_pw_[0] = new_pw_[0] = 0;
        }
        ImGui::Separator();
        ImGui::TextColored(COL_ACCENT, "Quantum emergency");
        ImGui::TextWrapped("If Falcon-512 is ever broken, move all your coins with your hash-based SPHINCS+ backup key "
                           "(bigger transaction, ~8 KB per address). Use the console: sweep <your new address> true");
    }
    ImGui::Separator();
    ImGui::TextColored(COL_ACCENT, "Log");
    ImGui::BeginChild("log", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::PushFont(g_mono);
    for (auto& l : log_recent(300)) ImGui::TextUnformatted(l.c_str());
    ImGui::PopFont();
    ImGui::EndChild();
}
