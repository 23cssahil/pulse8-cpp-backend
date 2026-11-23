#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = boost::asio::ip::tcp;

// ============================================================================
// CONFIGURATION CONSTANTS
// ============================================================================
constexpr double ENTRY_FEE_INR = 10.0;
constexpr double COMPANY_COMMISSION_RATE = 0.20; // 20% fixed cut
constexpr double DISTRIBUTABLE_POOL_RATE = 0.80; // 80% prize pool
constexpr int TICK_INTERVAL_MS = 50;             // 20 ticks per second
constexpr int WAITING_DURATION_MS = 6000;        // 6 sec lobby join window
constexpr bool BYPASS_8PM_WINDOW_FOR_TESTING = true; // Set false for strict 8-9 PM IST

// ============================================================================
// DATA STRUCTURES
// ============================================================================
enum class RoundState {
    WINDOW_CLOSED,
    WAITING,
    FLYING,
    CRASHED
};

struct PlayerEntry {
    std::string player_id;
    double entry_fee = ENTRY_FEE_INR;
    bool cashed_out = false;
    double cashout_multiplier = 0.0;
    int64_t cashout_timestamp_us = 0; // Microsecond precision for tie-breaking
    int final_rank = 0;
    double prize_won = 0.0;
};

struct CashoutRecord {
    std::string player_id;
    double multiplier;
    int64_t timestamp_us;

    // Descending by multiplier, then Ascending by microsecond timestamp
    bool operator<(const CashoutRecord& other) const {
        if (std::abs(multiplier - other.multiplier) > 1e-6) {
            return multiplier > other.multiplier;
        }
        return timestamp_us < other.timestamp_us;
    }
};

// ============================================================================
// CRYPTOGRAPHIC PROVABLY FAIR ENGINE (HMAC-SHA256)
// ============================================================================
class ProvablyFair {
public:
    static std::string generate_hex_seed(size_t bytes = 32) {
        std::vector<unsigned char> buf(bytes);
        RAND_bytes(buf.data(), static_cast<int>(bytes));
        std::ostringstream oss;
        for (unsigned char b : buf) {
            oss << std::hex << std::setw(2) << std::setfill('0') << (int)b;
        }
        return oss.str();
    }

    static std::string hmac_sha256(const std::string& key, const std::string& data) {
        unsigned char hash[EVP_MAX_MD_SIZE];
        unsigned int len = 0;
        HMAC(EVP_sha256(), key.c_str(), static_cast<int>(key.size()),
             reinterpret_cast<const unsigned char*>(data.c_str()), data.size(), hash, &len);

        std::ostringstream oss;
        for (unsigned int i = 0; i < len; ++i) {
            oss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
        }
        return oss.str();
    }

    // Derives theoretical max crash multiplier from first 52 bits of HMAC-SHA256
    static double compute_crash_point(const std::string& server_seed,
                                      const std::string& client_seed,
                                      uint64_t nonce) {
        std::string msg = client_seed + ":" + std::to_string(nonce);
        std::string hex_hash = hmac_sha256(server_seed, msg);

        // Take top 13 hex chars (52 bits)
        uint64_t h = std::stoull(hex_hash.substr(0, 13), nullptr, 16);
        constexpr uint64_t E = (1ULL << 52);

        // 4% house instant-bust edge check
        if (h % 25 == 0) return 1.00;

        double raw = (100.0 * E - h) / (E - h) / 100.0;
        return std::min(250.0, std::max(1.01, std::floor(raw * 100.0) / 100.0));
    }
};

// ============================================================================
// PARI-MUTUEL POOL & PREFIX-SUM RANKING ENGINE
// ============================================================================
class Session; // Forward declaration

class GameEngine {
public:
    GameEngine() {
        client_seed_ = "pulse8_public_client_seed_v1";
        prepare_next_round();
    }

    void join_session(std::shared_ptr<Session> s) {
        std::lock_guard<std::mutex> lock(mtx_);
        sessions_.insert(s);
    }

    void leave_session(std::shared_ptr<Session> s) {
        std::lock_guard<std::mutex> lock(mtx_);
        sessions_.erase(s);
    }

    // Check if current IST time is between 20:00 (8 PM) and 21:00 (9 PM)
    bool is_live_window_ist() const {
        if (BYPASS_8PM_WINDOW_FOR_TESTING) return true;
        auto now = std::chrono::system_clock::now();
        std::time_t utc_now = std::chrono::system_clock::to_time_t(now);
        std::time_t ist_now = utc_now + (5 * 3600 + 30 * 60); // UTC + 5:30
        std::tm* ist_tm = std::gmtime(&ist_now);
        return (ist_tm->tm_hour == 20); // 20:00:00 to 20:59:59 IST
    }

    // Handle incoming JSON-like commands from WebSocket clients
    std::string handle_client_message(const std::string& msg) {
        std::lock_guard<std::mutex> lock(mtx_);

        // Simple lightweight parser: ACTION|PLAYER_ID
        size_t sep = msg.find('|');
        if (sep == std::string::npos) return R"({"type":"ERROR","msg":"Invalid format"})";

        std::string action = msg.substr(0, sep);
        std::string player_id = msg.substr(sep + 1);

        if (action == "JOIN") {
            if (state_ != RoundState::WAITING) {
                return R"({"type":"ERROR","msg":"Round already in flight. Wait for next round."})";
            }
            if (players_.count(player_id)) {
                return R"({"type":"ERROR","msg":"Already joined this round."})";
            }
            players_[player_id] = PlayerEntry{player_id, ENTRY_FEE_INR, false, 0.0, 0, 0, 0.0};
            recalculate_pools();

            std::ostringstream res;
            res << R"({"type":"JOIN_OK","playerId":")" << player_id
                << R"(","players":)" << players_.size()
                << R"(,"distributablePool":)" << distributable_pool_ << "}";
            return res.str();
        }

        if (action == "CASHOUT") {
            if (state_ != RoundState::FLYING) {
                return R"({"type":"ERROR","msg":"Plane is not flying."})";
            }
            auto it = players_.find(player_id);
            if (it == players_.end() || it->second.cashed_out) {
                return R"({"type":"ERROR","msg":"Not eligible for cashout."})";
            }

            // Capture exact microsecond timestamp
            int64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count();

            double claimed_mult = current_multiplier_;
            double theoretical_liability = ENTRY_FEE_INR * claimed_mult;

            // DYNAMIC POOL-LIMIT SAFETY CAP CHECK
            if (running_claimed_liability_ + theoretical_liability >= distributable_pool_ &&
                distributable_pool_ > 0.0) {
                // Force crash immediately before liability exceeds 80% pool!
                force_crash_locked("POOL_CAP_REACHED");
                return R"({"type":"CRASHED_BEFORE_CASHOUT","msg":"Pool safety cap triggered!"})";
            }

            running_claimed_liability_ += theoretical_liability;
            it->second.cashed_out = true;
            it->second.cashout_multiplier = claimed_mult;
            it->second.cashout_timestamp_us = now_us;

            cashouts_.push_back(CashoutRecord{player_id, claimed_mult, now_us});

            std::ostringstream res;
            res << R"({"type":"CASHOUT_OK","playerId":")" << player_id
                << R"(","multiplier":)" << std::fixed << std::setprecision(2) << claimed_mult
                << R"(,"liveCashouts":)" << cashouts_.size() << "}";
            return res.str();
        }

        return R"({"type":"ERROR","msg":"Unknown command"})";
    }

    // Called every 50ms by Boost.Asio steady_timer
    std::string tick() {
        std::lock_guard<std::mutex> lock(mtx_);
        auto now = std::chrono::steady_clock::now();

        if (!is_live_window_ist()) {
            state_ = RoundState::WINDOW_CLOSED;
            return R"({"type":"WINDOW_CLOSED","msg":"Pulse8 is live daily 8:00 PM - 9:00 PM IST"})";
        }

        if (state_ == RoundState::WAITING) {
            auto elapsed_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - phase_start_time_).count();

            if (elapsed_ms >= WAITING_DURATION_MS) {
                // Transition WAITING -> FLYING
                state_ = RoundState::FLYING;
                phase_start_time_ = now;
                current_multiplier_ = 1.00;
                running_claimed_liability_ = 0.0;
                cashouts_.clear();

                std::ostringstream oss;
                oss << R"({"type":"START","roundId":)" << nonce_
                    << R"(,"players":)" << players_.size()
                    << R"(,"totalPool":)" << total_pool_
                    << R"(,"companyCommission":)" << company_commission_
                    << R"(,"prizePool":)" << distributable_pool_
                    << R"(,"hashCommitment":")" << server_seed_hash_ << R"("})";
                return oss.str();
            } else {
                long remaining = WAITING_DURATION_MS - elapsed_ms;
                std::ostringstream oss;
                oss << R"({"type":"WAITING","remainingMs":)" << remaining
                    << R"(,"players":)" << players_.size()
                    << R"(,"prizePool":)" << distributable_pool_ << "}";
                return oss.str();
            }
        }

        if (state_ == RoundState::FLYING) {
            double elapsed_sec =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - phase_start_time_).count() /
                1000.0;

            // Exponential growth curve: M(t) = e^(0.098 * t)
            current_multiplier_ = std::floor(std::exp(0.098 * elapsed_sec) * 100.0) / 100.0;

            // Trigger A: Cryptographic Crash Point Reached
            if (current_multiplier_ >= crypto_crash_point_) {
                current_multiplier_ = crypto_crash_point_;
                return force_crash_locked("CRYPTO_CRASH_POINT");
            }

            std::ostringstream oss;
            oss << R"({"type":"TICK","multiplier":)" << std::fixed << std::setprecision(2)
                << current_multiplier_ << R"(,"cashedOutCount":)" << cashouts_.size() << "}";
            return oss.str();
        }

        if (state_ == RoundState::CRASHED) {
            auto elapsed_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - phase_start_time_).count();
            if (elapsed_ms >= 3500) {
                prepare_next_round();
            }
        }

        return "";
    }

    std::vector<std::shared_ptr<Session>> get_sessions_snapshot() {
        std::lock_guard<std::mutex> lock(mtx_);
        return std::vector<std::shared_ptr<Session>>(sessions_.begin(), sessions_.end());
    }

private:
    std::mutex mtx_;
    std::unordered_set<std::shared_ptr<Session>> sessions_;

    RoundState state_ = RoundState::WAITING;
    std::chrono::steady_clock::time_point phase_start_time_;

    uint64_t nonce_ = 0;
    std::string server_seed_;
    std::string server_seed_hash_;
    std::string client_seed_;
    double crypto_crash_point_ = 1.00;
    double current_multiplier_ = 1.00;

    std::unordered_map<std::string, PlayerEntry> players_;
    std::vector<CashoutRecord> cashouts_;

    double total_pool_ = 0.0;
    double company_commission_ = 0.0;
    double distributable_pool_ = 0.0;
    double running_claimed_liability_ = 0.0;

    void recalculate_pools() {
        total_pool_ = players_.size() * ENTRY_FEE_INR;
        company_commission_ = total_pool_ * COMPANY_COMMISSION_RATE;
        distributable_pool_ = total_pool_ * DISTRIBUTABLE_POOL_RATE;
    }

    void prepare_next_round() {
        nonce_++;
        players_.clear();
        cashouts_.clear();
        recalculate_pools();

        server_seed_ = ProvablyFair::generate_hex_seed();
        server_seed_hash_ = ProvablyFair::hmac_sha256(server_seed_, "COMMITMENT");
        crypto_crash_point_ = ProvablyFair::compute_crash_point(server_seed_, client_seed_, nonce_);

        state_ = RoundState::WAITING;
        phase_start_time_ = std::chrono::steady_clock::now();
        current_multiplier_ = 1.00;
    }

    // Build Asymmetric Tiered Prefix Sum Array & Settle Winners
    std::string force_crash_locked(const std::string& reason) {
        state_ = RoundState::CRASHED;
        phase_start_time_ = std::chrono::steady_clock::now();

        // 1. Sort winners by (Multiplier DESC, Microsecond Timestamp ASC)
        std::sort(cashouts_.begin(), cashouts_.end());

        size_t W = cashouts_.size();
        if (W > 0 && distributable_pool_ > 0.0) {
            // 2. Build Asymmetric Weights (Top ranks get exponentially larger share)
            std::vector<double> weights(W, 0.0);
            double weight_sum = 0.0;
            for (size_t i = 0; i < W; ++i) {
                // Zipf/Asymmetric curve: Rank 1 gets highest share, tapering down smoothly
                weights[i] = 1.0 / std::pow(static_cast<double>(i + 1), 0.75);
                weight_sum += weights[i];
            }

            // 3. Build Prefix Sum Array P[0..W] of exact INR prize allocations
            std::vector<double> prefix_sum(W + 1, 0.0);
            for (size_t i = 0; i < W; ++i) {
                double slot_prize = (weights[i] / weight_sum) * distributable_pool_;
                prefix_sum[i + 1] = prefix_sum[i] + slot_prize;
            }

            // 4. Tie-Breaking via Prefix Sum Range Averaging [L..R]
            size_t i = 0;
            while (i < W) {
                size_t j = i;
                while (j + 1 < W &&
                       std::abs(cashouts_[j + 1].multiplier - cashouts_[i].multiplier) < 1e-6 &&
                       cashouts_[j + 1].timestamp_us == cashouts_[i].timestamp_us) {
                    j++;
                }

                // Exact split across tied range [i, j] using Prefix Sum
                double range_total = prefix_sum[j + 1] - prefix_sum[i];
                double equal_prize = std::floor((range_total / (j - i + 1)) * 100.0) / 100.0;

                for (size_t k = i; k <= j; ++k) {
                    auto& p = players_[cashouts_[k].player_id];
                    p.final_rank = static_cast<int>(i + 1);
                    p.prize_won = equal_prize;
                }
                i = j + 1;
            }
        }

        // Build Crash + Top 10 Leaderboard Broadcast JSON
        std::ostringstream oss;
        oss << R"({"type":"CRASH","crashPoint":)" << std::fixed << std::setprecision(2)
            << current_multiplier_ << R"(,"reason":")" << reason
            << R"(","serverSeed":")" << server_seed_
            << R"(","totalWinners":)" << W << R"(,"leaderboard":[)";

        size_t top_n = std::min<size_t>(10, W);
        for (size_t idx = 0; idx < top_n; ++idx) {
            const auto& p = players_[cashouts_[idx].player_id];
            if (idx > 0) oss << ",";
            oss << R"({"rank":)" << p.final_rank
                << R"(,"playerId":")" << p.player_id
                << R"(","mult":)" << p.cashout_multiplier
                << R"(,"prize":)" << p.prize_won << "}";
        }
        oss << "]}";
        return oss.str();
    }
};

// ============================================================================
// WEBSOCKET SESSION HANDLER
// ============================================================================
class Session : public std::enable_shared_from_this<Session> {
public:
    Session(tcp::socket&& socket, GameEngine& engine)
        : ws_(std::move(socket)), engine_(engine) {}

    void run() {
        ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));
        ws_.async_accept(beast::bind_front_handler(&Session::on_accept, shared_from_this()));
    }

    void send(const std::string& message) {
        net::post(ws_.get_executor(), [self = shared_from_this(), message]() {
            bool write_in_progress = !self->write_queue_.empty();
            self->write_queue_.push_back(message);
            if (!write_in_progress) {
                self->do_write();
            }
        });
    }

private:
    websocket::stream<beast::tcp_stream> ws_;
    GameEngine& engine_;
    beast::flat_buffer buffer_;
    std::vector<std::string> write_queue_;

    void on_accept(beast::error_code ec) {
        if (ec) return;
        engine_.join_session(shared_from_this());
        do_read();
    }

    void do_read() {
        ws_.async_read(buffer_, beast::bind_front_handler(&Session::on_read, shared_from_this()));
    }

    void on_read(beast::error_code ec, std::size_t) {
        if (ec) {
            engine_.leave_session(shared_from_this());
            return;
        }
        std::string msg = beast::buffers_to_string(buffer_.data());
        buffer_.consume(buffer_.size());

        std::string reply = engine_.handle_client_message(msg);
        if (!reply.empty()) {
            send(reply);
        }
        do_read();
    }

    void do_write() {
        ws_.async_write(net::buffer(write_queue_.front()),
                        beast::bind_front_handler(&Session::on_write, shared_from_this()));
    }

    void on_write(beast::error_code ec, std::size_t) {
        if (ec) {
            engine_.leave_session(shared_from_this());
            return;
        }
        write_queue_.erase(write_queue_.begin());
        if (!write_queue_.empty()) {
            do_write();
        }
    }
};

// ============================================================================
// TCP LISTENER & 50MS BROADCAST LOOP
// ============================================================================
class Listener : public std::enable_shared_from_this<Listener> {
public:
    Listener(net::io_context& ioc, tcp::endpoint endpoint, GameEngine& engine)
        : ioc_(ioc), acceptor_(ioc), engine_(engine) {
        acceptor_.open(endpoint.protocol());
        acceptor_.set_option(net::socket_base::reuse_address(true));
        acceptor_.bind(endpoint);
        acceptor_.listen(net::socket_base::max_listen_connections);
    }

    void run() { do_accept(); }

private:
    net::io_context& ioc_;
    tcp::acceptor acceptor_;
    GameEngine& engine_;

    void do_accept() {
        acceptor_.async_accept(net::make_strand(ioc_),
                               [self = shared_from_this()](beast::error_code ec, tcp::socket socket) {
                                   if (!ec) {
                                       std::make_shared<Session>(std::move(socket), self->engine_)->run();
                                   }
                                   self->do_accept();
                               });
    }
};

void schedule_tick(net::steady_timer& timer, GameEngine& engine) {
    timer.expires_after(std::chrono::milliseconds(TICK_INTERVAL_MS));
    timer.async_wait([&timer, &engine](const beast::error_code& ec) {
        if (ec) return;
        std::string broadcast_msg = engine.tick();
        if (!broadcast_msg.empty()) {
            for (auto& session : engine.get_sessions_snapshot()) {
                session->send(broadcast_msg);
            }
        }
        schedule_tick(timer, engine);
    });
}

int main() {
    const char* port_env = std::getenv("PORT");
    unsigned short port = port_env ? static_cast<unsigned short>(std::stoi(port_env)) : 8080;

    net::io_context ioc{1};
    GameEngine engine;

    auto listener = std::make_shared<Listener>(ioc, tcp::endpoint{tcp::v4(), port}, engine);
    listener->run();

    net::steady_timer tick_timer(ioc);
    schedule_tick(tick_timer, engine);

    std::cout << "[Pulse8 C++ Engine] Listening on port " << port << " (50ms tick loop active)\n";
    ioc.run();
    return 0;
}