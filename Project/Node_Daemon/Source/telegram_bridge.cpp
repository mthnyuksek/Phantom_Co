#include "telegram_bridge.hpp"

#include <windows.h>
#include <bcrypt.h>

#include <chrono>
#include <ctime>

#include "mcp_builtin.hpp"
#include "prompt.hpp"

namespace pc2 {

static const char* kReset = "New session: the agent forgot the earlier talk.";

static std::string T(bool tr, const std::string& en, const std::string& t) { return tr ? t : en; }

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

// "Ayşe Çalışkan" -> "ayse_caliskan": Telegram commands are a-z, 0-9 and _ only. A name with no letters keeps the old "ghost".
static std::string command_of(const std::string& name) {
    std::string c = command_slug(name);
    return c.empty() ? "ghost" : c;
}

static long long now_s() { return (long long)std::time(nullptr); }

TelegramBridge::TelegramBridge(Store& store, Auth& auth, Runner& runner, Mcp& mcp, const fs::path& data_dir)
    : store_(store), auth_(auth), runner_(runner), mcp_(mcp), file_(data_dir / L"telegram.json") {
    load();
}

void TelegramBridge::load() {
    json j = read_json(file_);
    state_ = j.is_object() ? j : json::object();
    for (const char* k : {"links", "current", "convs", "watch"})
        if (!state_.contains(k) || !state_[k].is_object()) state_[k] = json::object();
    if (!state_.contains("offset") || !state_["offset"].is_number_integer()) state_["offset"] = 0;
}

void TelegramBridge::save() { write_json(file_, state_); }

void TelegramBridge::start() {
    poll_ = std::thread([this] { poll_loop(); });
    sender_ = std::thread([this] { send_loop(); });
}

void TelegramBridge::stop() {
    stop_ = true;
    q_cv_.notify_all();
    if (poll_.joinable()) poll_.detach();
    if (sender_.joinable()) sender_.detach();
}

bool TelegramBridge::config(std::string& token) {
    for (auto& s : mcp_.list())
        if (s.value("builtin", "") == "telegram" && s.value("enabled", true) && s.value("options", json::object()).value("listen", false)) {
            token = mcp_.secret(s["name"], "bot_token");
            return !token.empty();
        }
    return false;
}

std::vector<std::pair<std::string, json>> TelegramBridge::commands() {
    std::vector<std::pair<std::string, json>> out;
    std::set<std::string> used;
    for (auto& a : store_.agents()) {
        if (!a.value("enabled", true)) continue;
        std::string c = command_of(a.value("name", "")), base = c;
        for (int i = 2; used.count(c) || is_reserved_command(c); ++i) c = base.substr(0, 29) + "_" + std::to_string(i);
        used.insert(c);
        out.push_back({c, a});
    }
    return out;
}

// The prompt shortcuts a person can use as /<command>: a ghost with the same command wins (shortcuts are checked when made,
// but a ghost hired later may take the word).
static std::vector<json> usable_shortcuts(Store& store, const std::vector<std::pair<std::string, json>>& ghost_cmds) {
    std::set<std::string> taken;
    for (auto& [c, a] : ghost_cmds) { taken.insert(c); taken.insert(a.value("id", "")); }
    std::vector<json> out;
    for (auto& s : store.shortcuts()) {
        std::string c = s.value("command", "");
        if (!c.empty() && !taken.count(c) && !is_reserved_command(c)) out.push_back(s);
    }
    return out;
}

// ------------------------------------------------------------------ polling
void TelegramBridge::poll_loop() {
    std::string last_token, last_cmds;
    auto nap = [&](int seconds) { for (int i = 0; i < seconds * 5 && !stop_; ++i) Sleep(200); };
    auto set_status = [&](const std::string& st, const std::string& text) {
        std::lock_guard<std::mutex> l(mu_);
        status_ = st;
        status_text_ = text;
    };
    while (!stop_) {
        std::string token, err;
        if (!config(token)) {
            set_status("off", "");
            { std::lock_guard<std::mutex> l(mu_); token_now_.clear(); }
            last_token.clear();
            nap(3);
            continue;
        }
        if (token != last_token) {
            json me = builtin::telegram_api(token, "getMe", json::object(), err, 20000);
            if (stop_) break;
            if (me.is_null()) { set_status("error", err); nap(15); continue; }
            std::lock_guard<std::mutex> l(mu_);
            bot_ = me.value("username", "");
            token_now_ = token;
            last_token = token;
            last_cmds.clear();
        }
        {   // the bot's command menu: the fixed commands and one per ghost
            json list = json::array({{{"command", "agents"}, {"description", "List the ghosts / Hayaletleri listele"}},
                                     {{"command", "new"}, {"description", "Forget the chat / Sohbeti unut"}},
                                     {{"command", "help"}, {"description", "Help / Yardım"}}});
            auto ghost_cmds = commands();
            for (auto& [c, a] : ghost_cmds)
                if (list.size() < 100) list.push_back({{"command", c}, {"description", truncate_utf8(a.value("name", "") + (a.value("role", "").empty() ? "" : " · " + a.value("role", "")), 250)}});
            for (auto& s : usable_shortcuts(store_, ghost_cmds))  // description must not be empty
                if (list.size() < 100) list.push_back({{"command", s.value("command", "")},
                                                       {"description", truncate_utf8("⚡ " + replace_all(s.value("title", "").empty() ? s.value("body", "") : s.value("title", ""), "\n", " "), 250)}});
            std::string sig = list.dump();
            if (sig != last_cmds && !builtin::telegram_api(token, "setMyCommands", {{"commands", list}}, err, 20000).is_null()) last_cmds = sig;
        }
        long long offset;
        { std::lock_guard<std::mutex> l(mu_); offset = state_["offset"].get<long long>(); }
        json ups = builtin::telegram_api(token, "getUpdates", {{"offset", offset}, {"timeout", 20}, {"allowed_updates", {"message"}}}, err, 35000);
        if (stop_) break;
        if (ups.is_null()) {
            set_status("error", err.find("onflict") != std::string::npos
                                    ? "Another program reads this bot's messages (another Phantom Co. window, or a webhook). Only one may listen."
                                    : err);
            nap(5);
            continue;
        }
        set_status("on", "");
        for (auto& u : ups) {
            {
                std::lock_guard<std::mutex> l(mu_);
                state_["offset"] = u.value("update_id", 0LL) + 1;  // confirmed at the next getUpdates
                save();
            }
            if (u.contains("message") && u["message"].is_object()) {
                try { handle(u["message"], token); } catch (const std::exception& e) { std::string ignored = e.what(); }
            }
        }
    }
}

void TelegramBridge::reply(const std::string& chat, const std::string& text) {
    std::string token, err;
    { std::lock_guard<std::mutex> l(mu_); token = token_now_; }
    if (!token.empty()) builtin::telegram_send(token, chat, text, err);
}

static std::string help_text(bool tr) {
    return T(tr, "Commands:\n/agents — list the ghosts\n/<ghost> message — talk to a ghost (it becomes the current one: plain messages go to it)\n/new — the current ghost forgets the chat\n/yes, /no — answer an approval request or a script's question\n/answer value — give the value a script asks for\n/<shortcut> text — a ready-made prompt of the company (the shortcuts are listed in /agents)",
             "Komutlar:\n/agents — hayaletleri listeler\n/<hayalet> mesaj — bir hayaletle konuş (o artık geçerli hayalet olur: düz mesajlar ona gider)\n/new — geçerli hayalet sohbeti unutur\n/yes, /no — bir onay isteğini ya da betik sorusunu yanıtla\n/answer değer — betiğin istediği değeri ver\n/<kısayol> metin — şirketin hazır promptu (kısayollar /agents içinde listelenir)");
}

void TelegramBridge::handle(const json& m, const std::string& token) {
    const json& from = m.value("from", json::object());
    const json& c = m.value("chat", json::object());
    const std::string tg = std::to_string(from.value("id", 0LL)), chat = std::to_string(c.value("id", 0LL));
    const bool tr = from.value("language_code", "").rfind("tr", 0) == 0, priv = c.value("type", "") == "private";
    const std::string text = trim(m.value("text", ""));
    {   // what came in (for the read_messages tool), newest last
        std::lock_guard<std::mutex> l(mu_);
        inbox_.push_back({{"date", m.value("date", 0LL)}, {"chat", c.value("id", 0LL)}, {"chat_name", c.value("title", c.value("first_name", ""))},
                          {"from", from.value("first_name", "") + (from.contains("username") ? " (@" + from.value("username", "") + ")" : "")},
                          {"text", m.value("text", m.value("caption", ""))}});
        while (inbox_.size() > 100) inbox_.pop_front();
    }
    if (text.empty() || tg == "0") return;

    std::string cmd, rest = text;
    if (text[0] == '/') {
        size_t sp = text.find_first_of(" \n\t");
        cmd = text.substr(1, sp == std::string::npos ? std::string::npos : sp - 1);
        rest = sp == std::string::npos ? "" : trim(text.substr(sp));
        size_t at = cmd.find('@');  // "/agents@my_bot" in groups
        if (at != std::string::npos) cmd.resize(at);
        for (auto& ch : cmd) ch = (char)tolower((unsigned char)ch);
    }

    if (cmd == "start" && rest.rfind("link_", 0) == 0) {  // the t.me/<bot>?start=link_<code> button of the Profile
        cmd = "link";
        rest = rest.substr(5);
    }
    if (cmd == "link") {
        std::string uid;
        {
            std::lock_guard<std::mutex> l(mu_);
            auto it = codes_.find(rest);
            if (it != codes_.end() && it->second.second >= now_s()) uid = it->second.first;
            if (it != codes_.end()) codes_.erase(it);
        }
        auto u = uid.empty() ? std::nullopt : store_.user(uid);
        if (!u) return reply(chat, T(tr, "That code is wrong or too old. Make a new one in Phantom Co. (Settings > Personal > Telegram).", "Bu kod yanlış ya da süresi dolmuş. Phantom Co.’da yeni bir kod oluştur (Ayarlar > Kişisel > Telegram)."));
        {
            std::lock_guard<std::mutex> l(mu_);
            state_["links"][tg] = {{"user", uid}, {"name", from.value("first_name", "")}, {"tr", tr}};
            save();
        }
        return reply(chat, T(tr, "Connected to Phantom Co. as " + u->value("name", "") + ".\n\n", "Phantom Co.’ya " + u->value("name", "") + " olarak bağlandın.\n\n") + help_text(tr));
    }

    std::string uid;
    {
        std::lock_guard<std::mutex> l(mu_);
        if (state_["links"].contains(tg)) uid = state_["links"][tg].value("user", "");
    }
    if (uid.empty()) {
        if (priv) reply(chat, T(tr, "This bot belongs to a Phantom Co. company. To talk to its ghosts, open Settings > Personal in Phantom Co., press \"Connect Telegram\" and send the /link code here.",
                                "Bu bot bir Phantom Co. şirketine ait. Hayaletleriyle konuşmak için Phantom Co.’da Ayarlar > Kişisel bölümünü aç,\"Telegram’ı bağla\"ya bas ve /link kodunu buraya gönder."));
        return;
    }
    auto user = store_.user(uid);
    const std::string role = auth_.role(uid);
    if (!user || user->value("disabled", false) || role.empty()) return reply(chat, T(tr, "Your Phantom Co. account is no longer active.", "Phantom Co. hesabın artık etkin değil."));
    const bool owner = role == "owner";

    if (cmd == "unlink") {
        { std::lock_guard<std::mutex> l(mu_); state_["links"].erase(tg); save(); }
        return reply(chat, T(tr, "Disconnected. This Telegram account no longer reaches the ghosts.", "Bağlantı kesildi. Bu Telegram hesabı artık hayaletlere ulaşmıyor."));
    }
    if (cmd == "start" || cmd == "help" || cmd == "yardim") return reply(chat, help_text(tr));

    auto cmds = commands();
    auto by_cmd = [&](const std::string& name) -> std::optional<json> {
        for (auto& [k, a] : cmds) if (k == name || a.value("id", "") == name) return a;
        return std::nullopt;
    };
    auto current = [&]() -> std::optional<json> {
        std::string id;
        { std::lock_guard<std::mutex> l(mu_); id = state_["current"].value(tg, ""); }
        auto a = id.empty() ? std::nullopt : store_.agent(id);
        return a && a->value("enabled", true) ? a : std::nullopt;
    };

    if (cmd == "agents" || cmd == "phantoms" || cmd == "ghosts" || cmd == "hayaletler") {
        json act = runner_.agent_activity();
        auto cur = current();
        std::string out = T(tr, "Ghosts of the company:\n", "Şirketin hayaletleri:\n");
        for (auto& [k, a] : cmds)
            out += "/" + k + " — " + a.value("name", "") + (a.value("role", "").empty() ? "" : " · " + a.value("role", "")) +
                   (act.contains(a.value("id", "")) ? T(tr, " (working)", " (çalışıyor)") : "") + (cur && (*cur)["id"] == a["id"] ? " ←" : "") + "\n";
        if (cmds.empty()) out += T(tr, "(no ghosts yet)\n", "(henüz hayalet yok)\n");
        auto shorts = usable_shortcuts(store_, cmds);
        if (!shorts.empty()) {
            out += T(tr, "\nShortcuts (ready-made prompts):\n", "\nKısayollar (hazır promptlar):\n");
            for (auto& s : shorts) out += "/" + s.value("command", "") + (s.value("title", "").empty() ? "" : " — " + s.value("title", "")) + "\n";
        }
        out += T(tr, "\nWrite /<ghost> and your message. /new: the current ghost forgets the chat.", "\n/<hayalet> ve mesajını yaz. /new: geçerli hayalet sohbeti unutur.");
        return reply(chat, out);
    }

    std::vector<std::string> my_convs;
    {
        std::lock_guard<std::mutex> l(mu_);
        for (auto& [k, v] : state_["convs"].items()) if (k.rfind(tg + ":", 0) == 0 && v.is_string()) my_convs.push_back(v.get<std::string>());
    }
    if (cmd == "yes" || cmd == "no" || cmd == "evet" || cmd == "hayir") {
        for (auto& conv : my_convs)
            for (auto& p : mcp_.pending(conv, uid, owner))
                if (p.value("can_decide", false)) {
                    std::string err;
                    if (!mcp_.decide(p["id"], uid, user->value("name", ""), owner, cmd == "yes" || cmd == "evet", err)) reply(chat, err);
                    return;  // the answer line in the chat goes to Telegram by itself
                }
        // no ghost approval is waiting: the newest question of a script
        json asks = asks_list_ ? asks_list_(uid, owner) : json::array();
        if (!asks.empty()) {
            const json ask = asks.back();
            const bool approved = cmd == "yes" || cmd == "evet";
            if (approved && ask.value("mode", "") == "value") return reply(chat, T(tr, "This question needs a value: /answer <value>", "Bu soru bir değer istiyor: /answer <değer>"));
            std::string err;
            if (!asks_answer_(ask, uid, owner, user->value("name", ""), approved, "", err)) return reply(chat, err);
            return reply(chat, approved ? T(tr, "✓ Approved. The script goes on.", "✓ Onaylandı. Betik devam ediyor.") : T(tr, "✗ Rejected.", "✗ Reddedildi."));
        }
        return reply(chat, T(tr, "Nothing is waiting for your answer.", "Yanıtını bekleyen bir şey yok."));
    }
    if (cmd == "answer" || cmd == "cevap") {
        json asks = asks_list_ ? asks_list_(uid, owner) : json::array();
        json ask;
        for (auto& a : asks) if (a.value("mode", "") == "value") ask = a;  // the newest one that wants a value
        if (ask.is_null()) return reply(chat, T(tr, "No script is waiting for a value.", "Değer bekleyen bir betik yok."));
        if (rest.empty()) return reply(chat, T(tr, "Write the value after the command: /answer <value>", "Değeri komuttan sonra yaz: /answer <değer>"));
        std::string err;
        if (!asks_answer_(ask, uid, owner, user->value("name", ""), true, rest, err)) return reply(chat, err);
        return reply(chat, T(tr, "✓ Got it. The script goes on.", "✓ Aldım. Betik devam ediyor."));
    }

    if (cmd == "new" || cmd == "yeni") {
        auto a = rest.empty() ? current() : by_cmd(command_of(rest));
        if (!a) return reply(chat, T(tr, "Pick a ghost first: /agents", "Önce bir hayalet seç: /agents"));
        std::string conv;
        { std::lock_guard<std::mutex> l(mu_); conv = state_["convs"].value(tg + ":" + (*a)["id"].get<std::string>(), ""); }
        if (conv.empty() || !store_.conv_load(conv)) return reply(chat, T(tr, (*a).value("name", "") + " has nothing to forget yet.", (*a).value("name", "") + " henüz unutacak bir şey yok."));
        if (runner_.conversation_busy(conv)) return reply(chat, T(tr, (*a).value("name", "") + " is still working. Try again when it has answered.", (*a).value("name", "") + " hâlâ çalışıyor. Yanıt verince tekrar dene."));
        store_.conv_append(conv, {{"t", "reset"}, {"text", kReset}});
        return reply(chat, T(tr, "🧹 " + (*a).value("name", "") + " forgot the chat. The next message starts fresh.", "🧹 " + (*a).value("name", "") + " sohbeti unuttu. Sonraki mesaj sıfırdan başlar."));
    }

    std::optional<json> target;
    if (!cmd.empty()) {
        target = by_cmd(cmd);
        if (!target) {  // not a ghost: a prompt shortcut? It goes to its own ghost, else to the current one (which stays as it was)
            std::optional<json> sc;
            for (auto& s : usable_shortcuts(store_, cmds)) if (s.value("command", "") == cmd) sc = s;
            if (!sc) return reply(chat, T(tr, "There is no ghost or shortcut called /" + cmd + ". /agents lists them.", "/" + cmd + " adında bir hayalet ya da kısayol yok. /agents hepsini listeler."));
            std::optional<json> a;
            if (!sc->value("agent_id", "").empty()) {
                a = store_.agent(sc->value("agent_id", ""));
                if (a && !a->value("enabled", true)) a = std::nullopt;
            }
            if (!a) a = current();
            if (!a) return reply(chat, T(tr, "Who should get this? Pick a ghost first, for example /" + (cmds.empty() ? std::string("agents") : cmds[0].first) + ", then send /" + cmd + " again.",
                                         "Bu kime gitsin? Önce bir hayalet seç, örneğin /" + (cmds.empty() ? std::string("agents") : cmds[0].first) + ", sonra /" + cmd + " komutunu tekrar gönder."));
            return to_ghost(tg, chat, *a, expand_shortcut(sc->value("body", ""), rest), *user, role, tr);
        }
        { std::lock_guard<std::mutex> l(mu_); state_["current"][tg] = (*target)["id"]; save(); }
        if (rest.empty()) return reply(chat, T(tr, "Now talking to " + target->value("name", "") + ". Write your message.", "Artık " + target->value("name", "") + " ile konuşuyorsun. Mesajını yaz."));
    } else {
        target = current();
        if (!target) return reply(chat, T(tr, "Who should get this? Start with /<ghost>, for example:\n", "Bu kime gitsin? /<hayalet> ile başla, örneğin:\n") +
                                            (cmds.empty() ? std::string("/agents") : "/" + cmds[0].first + " " + T(tr, "hello", "merhaba")));
    }
    to_ghost(tg, chat, *target, rest, *user, role, tr);
}

void TelegramBridge::to_ghost(const std::string& tg, const std::string& chat, const json& agent, const std::string& text, const json& user,
                              const std::string& role, bool tr) {
    const std::string uid = user.value("id", ""), aid = agent.value("id", ""), key = tg + ":" + aid;
    if (role_rank(role) < role_rank("editor")) return reply(chat, T(tr, "Your role can only read. Ask the owner to make you an editor.", "Rolün yalnızca okumaya izin veriyor. Sahibinden seni düzenleyici yapmasını iste."));
    std::string conv;
    { std::lock_guard<std::mutex> l(mu_); conv = state_["convs"].value(key, ""); }
    auto cv = conv.empty() ? std::nullopt : store_.conv_load(conv);
    if (!cv || (*cv)["meta"].value("user_id", "") != uid) {
        conv = store_.conv_create({{"kind", "user"}, {"title", "Telegram · " + agent.value("name", "")}, {"auto_title", false}, {"agent_id", aid},
                                   {"user_id", uid}, {"project_id", ""}, {"shared", false}, {"telegram", true}});
        cv = store_.conv_load(conv);
        std::lock_guard<std::mutex> l(mu_);
        state_["convs"][key] = conv;
        save();
    }
    if (runner_.conversation_busy(conv))
        return reply(chat, T(tr, agent.value("name", "") + " is still working on your last message. Wait for the answer (or /new after it).",
                             agent.value("name", "") + " hâlâ son mesajın üzerinde çalışıyor. Yanıtı bekle."));
    {
        std::lock_guard<std::mutex> l(mu_);
        state_["watch"][conv] = {{"chat", chat}, {"tr", tr}};
        save();
    }
    store_.conv_append(conv, {{"t", "msg"}, {"from", "user:" + uid}, {"text", text}, {"via", "telegram"}});
    Job j;
    j.kind = "chat";
    j.agent_id = aid;
    j.user_id = uid;
    j.user_role = role;
    j.stack = {{"user", uid, conv, false}};
    j.prompt = text;
    std::string err;
    if (runner_.enqueue(j, err).empty()) {
        store_.conv_append(conv, {{"t", "msg"}, {"from", "system"}, {"text", "⚠ " + err}});  // goes to Telegram too
        return;
    }
    std::string token, e2;
    { std::lock_guard<std::mutex> l(mu_); token = token_now_; }
    if (!token.empty()) builtin::telegram_api(token, "sendChatAction", {{"chat_id", chat}, {"action", "typing"}}, e2, 20000);
}

// ------------------------------------------------------- answers to Telegram
void TelegramBridge::on_append(const std::string& conv, const json& line) {
    if (line.value("t", "") != "msg") return;
    const std::string from = line.value("from", "");
    {
        std::lock_guard<std::mutex> l(mu_);
        if (!state_["watch"].contains(conv)) return;
        if (from.rfind("user:", 0) == 0) {  // the person went on in the app: answers stay there
            if (line.value("via", "") != "telegram") { state_["watch"].erase(conv); save(); }
            return;
        }
    }
    {
        std::lock_guard<std::mutex> l(q_mu_);
        queue_.push_back({conv, line});
    }
    q_cv_.notify_one();
}

void TelegramBridge::send_loop() {
    while (!stop_) {
        std::pair<std::string, json> item;
        {
            std::unique_lock<std::mutex> l(q_mu_);
            q_cv_.wait_for(l, std::chrono::seconds(2), [&] { return stop_ || !queue_.empty(); });
            if (stop_) return;
            if (queue_.empty()) continue;
            item = queue_.front();
            queue_.pop_front();
        }
        const auto& [conv, line] = item;
        std::string chat, token;
        bool tr = false;
        {
            std::lock_guard<std::mutex> l(mu_);
            if (!state_["watch"].contains(conv)) continue;
            chat = state_["watch"][conv].value("chat", "");
            tr = state_["watch"][conv].value("tr", false);
            token = token_now_;
        }
        if (chat.empty() || token.empty()) continue;
        const std::string from = line.value("from", "");
        std::string text;
        if (from.rfind("agent:", 0) == 0) {
            auto a = store_.agent(from.substr(6));
            text = "👻 " + (a ? a->value("name", "") : from.substr(6)) + ":\n" + line.value("text", "");
        } else if (line.value("kind", "") == "approval") {
            auto a = store_.agent(line.value("agent", ""));
            text = "🔐 " + T(tr, (a ? a->value("name", "") : "A ghost") + " asks to use ", (a ? a->value("name", "") : "Bir hayalet") + " şunu kullanmak istiyor: ") +
                   line.value("server", "") + " › " + line.value("tool", "") + "\n\n" + truncate_utf8(line.value("args", ""), 1500) + "\n\n" +
                   T(tr, "Answer with /yes or /no.", "/yes ya da /no ile yanıtla.");
        } else {
            text = line.value("text", "");  // ⚠ failures, ℹ notes, ✓/✗ answers to approvals
        }
        std::string err;
        if (!text.empty()) builtin::telegram_send(token, chat, text, err);
    }
}

// ------------------------------------------------------------------ the app
json TelegramBridge::status() {
    std::lock_guard<std::mutex> l(mu_);
    return {{"state", status_}, {"text", status_text_}, {"bot", bot_}};
}

json TelegramBridge::link_info(const std::string& user_id) {
    std::lock_guard<std::mutex> l(mu_);
    json linked = json::array();
    for (auto& [k, v] : state_["links"].items()) if (v.value("user", "") == user_id) linked.push_back({{"name", v.value("name", "")}});
    return {{"on", status_ == "on"}, {"bot", bot_}, {"linked", linked}};
}

std::vector<std::string> TelegramBridge::chat_ids(const std::string& user_id) {
    std::vector<std::string> out;
    std::lock_guard<std::mutex> l(mu_);
    for (auto& [k, v] : state_["links"].items()) if (v.value("user", "") == user_id) out.push_back(k);
    return out;
}

std::string TelegramBridge::new_link_code(const std::string& user_id) {
    unsigned int r = 0;
    BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&r), sizeof(r), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    std::string code = std::to_string(100000 + r % 900000);
    std::lock_guard<std::mutex> l(mu_);
    for (auto it = codes_.begin(); it != codes_.end();) it = it->second.first == user_id || it->second.second < now_s() ? codes_.erase(it) : std::next(it);
    codes_[code] = {user_id, now_s() + 600};
    return code;
}

void TelegramBridge::unlink(const std::string& user_id) {
    std::lock_guard<std::mutex> l(mu_);
    for (auto it = state_["links"].begin(); it != state_["links"].end();) it = it->value("user", "") == user_id ? state_["links"].erase(it) : std::next(it);
    save();
}

void TelegramBridge::send_to_user(const std::string& user_id, const std::string& en, const std::string& tr) {
    std::vector<std::pair<std::string, bool>> chats;
    std::string token;
    {
        std::lock_guard<std::mutex> l(mu_);
        token = token_now_;
        for (auto& [tg, v] : state_["links"].items())
            if (v.value("user", "") == user_id) chats.push_back({tg, v.value("tr", false)});  // a person's private chat has the id of the person
    }
    if (token.empty()) return;
    for (auto& [chat, is_tr] : chats) {
        std::string err;
        builtin::telegram_send(token, chat, is_tr ? tr : en, err);
    }
}

void TelegramBridge::set_asks(std::function<json(const std::string&, bool)> list,
                              std::function<bool(const json&, const std::string&, bool, const std::string&, bool, const std::string&, std::string&)> answer) {
    asks_list_ = std::move(list);
    asks_answer_ = std::move(answer);
}

json TelegramBridge::inbox() {
    std::lock_guard<std::mutex> l(mu_);
    json out = json::array();
    for (auto& m : inbox_) out.push_back(m);
    return out;
}

}  // namespace pc2
