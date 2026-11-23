#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/steady_timer.hpp>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <iostream>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <random>
#include <cstdlib>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = boost::asio::ip::tcp;

// --- ECONOMIC & POOL CONSTANTS ---
constexpr double MIN_BET = 10.0;
constexpr double MAX_BET_CAP_RATIO = 0.05;      // Dynamic 5% Max Bet Cap of Total Pool
constexpr double COMMISSION_RATE = 0.20;        // 20% Company Cut
constexpr double POOL_PAYOUT_RATE = 0.80;       // 80% Distributable Pool
constexpr int WAITING_DURATION_MS = 7000;       // 7s Betting Window
constexpr int CRASH_DISPLAY_MS = 4500;          // 4.5s Leaderboard View
constexpr int TICK_INTERVAL_MS = 50;            // 50ms Server Tick (20 FPS)

enum class GameState { WAITING, FLYING, CRASHED };

struct PlayerEntry {
    std::string playerId;
    double betAmount = 0.0;
    bool cashedOut = false;
    double cashoutMultiplier = 0.0;
    int64_t cashoutTimeMicros = 0;
    double score = 0.0;
    double prizeWon = 0.0;
    int rank = 0;
};

// --- CRYPTOGRAPHIC PROVABLY FAIR ENGINE ---
std::string generateRandomHex(size_t bytes = 32) {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dis;
    std::ostringstream oss;
    for (size_t i = 0; i < bytes / 8; ++i) {
        oss << std::hex << std::setw(16) << std::setfill('0') << dis(gen);
    }
    return oss.str();
}

std::string sha256Hex(const std::string& input) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(input.c_str()), input.size(), hash);
    std::ostringstream oss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        oss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
    }
    return oss.str();
}

double computeCryptoCrashPoint(const std::string& serverSeed, uint64_t roundId) {
    std::string msg = "pulse8-round-" + std::to_string(roundId);
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digestLen = 0;

    HMAC(EVP_sha256(),
         serverSeed.c_str(), static_cast<int>(serverSeed.size()),
         reinterpret_cast<const unsigned char*>(msg.c_str()), msg.size(),
         digest, &digestLen);

    uint64_t h = 0;
    for (int i = 0; i < 8; ++i) {
        h = (h << 8) | digest[i];
    }

    if (h % 25 == 0) return 1.00; // 4% instant crash edge
    double e = static_cast<double>(h >> 11) / static_cast<double>(1ULL << 53);
    double raw = 0.99 / (1.0 - e);
    return std::min(100.0, std::max(1.01, std::floor(raw * 100.0) / 100.0));
}

class Session;

// --- CENTRAL GAME ENGINE ---
class GameEngine {
public:
    GameEngine(net::io_context& ioc) : timer_(ioc) {
        prepareNextRound();
    }

    void start() {
        scheduleTick();
    }

    void joinSession(std::shared_ptr<Session> s) {
        std::lock_guard<std::mutex> lock(mtx_);
        sessions_.push_back(s);
    }

    void leaveSession(std::shared_ptr<Session> s) {
        std::lock_guard<std::mutex> lock(mtx_);
        sessions_.erase(std::remove(sessions_.begin(), sessions_.end(), s), sessions_.end());
    }

    // Dynamic 5% Max Bet Limit (min ₹10 so early players can seed the pool)
    double computeDynamicMaxBet() const {
        double cap = std::floor(totalPool_ * MAX_BET_CAP_RATIO);
        return std::max(MIN_BET, cap);
    }

    std::string handleCommand(const std::string& rawMsg) {
        std::lock_guard<std::mutex> lock(mtx_);

        // Protocol: JOIN|playerId|amount
        if (rawMsg.rfind("JOIN|", 0) == 0) {
            std::string rest = rawMsg.substr(5);
            size_t sep = rest.find('|');
            if (sep == std::string::npos) {
                return R"({"type":"ERROR","msg":"Invalid JOIN format"})";
            }

            std::string playerId = rest.substr(0, sep);
            double betAmount = 0.0;
            try {
                betAmount = std::stod(rest.substr(sep + 1));
            } catch (...) {
                return R"({"type":"ERROR","msg":"Invalid bet amount"})";
            }

            if (state_ != GameState::WAITING) {
                return R"({"type":"ERROR","msg":"Round already in flight! Wait for next pool."})";
            }
            if (players_.count(playerId)) {
                return R"({"type":"ERROR","msg":"Already joined this pool."})";
            }

            double allowedMax = computeDynamicMaxBet();
            if (betAmount < MIN_BET) {
                return R"({"type":"ERROR","msg":"Minimum bet is ₹10."})";
            }
            if (betAmount > allowedMax) {
                std::ostringstream err;
                err << R"({"type":"ERROR","msg":"Exceeds 5% Dynamic Pool Cap! Current Max: ₹)"
                    << std::fixed << std::setprecision(0) << allowedMax << R"("})";
                return err.str();
            }

            PlayerEntry entry;
            entry.playerId = playerId;
            entry.betAmount = betAmount;
            players_[playerId] = entry;
            totalPool_ += betAmount;

            std::ostringstream oss;
            oss << std::fixed << std::setprecision(2)
                << R"({"type":"JOIN_OK","playerId":")" << playerId
                << R"(","betAmount":)" << betAmount
                << R"(,"totalPool":)" << totalPool_
                << R"(,"dynamicMaxBet":)" << computeDynamicMaxBet() << "}";
            return oss.str();
        }

        // Protocol: CASHOUT|playerId
        if (rawMsg.rfind("CASHOUT|", 0) == 0) {
            std::string playerId = rawMsg.substr(8);
            if (state_ != GameState::FLYING) {
                return R"({"type":"ERROR","msg":"Plane is not flying."})";
            }
            auto it = players_.find(playerId);
            if (it == players_.end()) {
                return R"({"type":"ERROR","msg":"You did not join this round."})";
            }
            if (it->second.cashedOut) {
                return R"({"type":"ERROR","msg":"Already cashed out."})";
            }

            auto nowMicros = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();

            it->second.cashedOut = true;
            it->second.cashoutMultiplier = currentMultiplier_;
            it->second.cashoutTimeMicros = nowMicros;
            it->second.score = it->second.betAmount * currentMultiplier_;

            std::ostringstream oss;
            oss << std::fixed << std::setprecision(2)
                << R"({"type":"CASHOUT_OK","playerId":")" << playerId
                << R"(","multiplier":)" << currentMultiplier_
                << R"(,"score":)" << it->second.score << "}";
            return oss.str();
        }

        return R"({"type":"ERROR","msg":"Unknown command"})";
    }

private:
    void prepareNextRound() {
        roundId_++;
        state_ = GameState::WAITING;
        players_.clear();
        totalPool_ = 0.0;
        currentMultiplier_ = 1.00;
        phaseStart_ = std::chrono::steady_clock::now();

        serverSeed_ = generateRandomHex(32);
        hashCommitment_ = sha256Hex(serverSeed_);
        cryptoCrashPoint_ = computeCryptoCrashPoint(serverSeed_, roundId_);
    }

    void scheduleTick() {
        timer_.expires_after(std::chrono::milliseconds(TICK_INTERVAL_MS));
        timer_.async_wait([this](beast::error_code ec) {
            if (!ec) {
                onTick();
                scheduleTick();
            }
        });
    }

    void onTick() {
        std::string broadcastMsg;
        {
            std::lock_guard<std::mutex> lock(mtx_);
            auto now = std::chrono::steady_clock::now();
            auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - phaseStart_).count();

            if (state_ == GameState::WAITING) {
                long remaining = std::max(0L, static_cast<long>(WAITING_DURATION_MS - elapsedMs));
                if (remaining == 0) {
                    state_ = GameState::FLYING;
                    phaseStart_ = now;
                    currentMultiplier_ = 1.00;

                    double commission = totalPool_ * COMMISSION_RATE;
                    double distributablePool = totalPool_ * POOL_PAYOUT_RATE;

                    std::ostringstream oss;
                    oss << std::fixed << std::setprecision(2)
                        << R"({"type":"START","roundId":)" << roundId_
                        << R"(,"players":)" << players_.size()
                        << R"(,"totalPool":)" << totalPool_
                        << R"(,"companyCommission":)" << commission
                        << R"(,"prizePool":)" << distributablePool
                        << R"(,"hashCommitment":")" << hashCommitment_ << R"("})";
                    broadcastMsg = oss.str();
                } else {
                    std::ostringstream oss;
                    oss << std::fixed << std::setprecision(2)
                        << R"({"type":"WAITING","roundId":)" << roundId_
                        << R"(,"remainingMs":)" << remaining
                        << R"(,"players":)" << players_.size()
                        << R"(,"totalPool":)" << totalPool_
                        << R"(,"prizePool":)" << (totalPool_ * POOL_PAYOUT_RATE)
                        << R"(,"dynamicMaxBet":)" << computeDynamicMaxBet() << "}";
                    broadcastMsg = oss.str();
                }
            }
            else if (state_ == GameState::FLYING) {
                double elapsedSec = elapsedMs / 1000.0;
                currentMultiplier_ = std::floor(std::exp(0.068 * elapsedSec * 1.45) * 100.0) / 100.0;

                if (currentMultiplier_ >= cryptoCrashPoint_) {
                    broadcastMsg = settleAndCrash(cryptoCrashPoint_);
                } else {
                    std::ostringstream oss;
                    oss << std::fixed << std::setprecision(2)
                        << R"({"type":"TICK","roundId":)" << roundId_
                        << R"(,"multiplier":)" << currentMultiplier_ << "}";
                    broadcastMsg = oss.str();
                }
            }
            else if (state_ == GameState::CRASHED) {
                if (elapsedMs >= CRASH_DISPLAY_MS) {
                    prepareNextRound();
                }
            }
        }

        if (!broadcastMsg.empty()) {
            broadcast(broadcastMsg);
        }
    }

    // Weighted Score Pool-Share Settlement + Microsecond Tie-Breaking
    std::string settleAndCrash(double finalCrashMult) {
        state_ = GameState::CRASHED;
        phaseStart_ = std::chrono::steady_clock::now();
        currentMultiplier_ = finalCrashMult;

        double distributablePool = totalPool_ * POOL_PAYOUT_RATE;
        std::vector<PlayerEntry*> winners;
        double totalWinnerScore = 0.0;

        for (auto& kv : players_) {
            if (kv.second.cashedOut) {
                kv.second.score = kv.second.betAmount * kv.second.cashoutMultiplier;
                totalWinnerScore += kv.second.score;
                winners.push_back(&kv.second);
            }
        }

        // Rank by highest Weighted Score, tie-break by earliest microsecond cashout
        std::sort(winners.begin(), winners.end(), [](const PlayerEntry* a, const PlayerEntry* b) {
            if (std::abs(a->score - b->score) > 1e-6) {
                return a->score > b->score;
            }
            return a->cashoutTimeMicros < b->cashoutTimeMicros;
        });

        for (size_t i = 0; i < winners.size(); ++i) {
            winners[i]->rank = static_cast<int>(i + 1);
            if (totalWinnerScore > 0.0 && distributablePool > 0.0) {
                double share = (winners[i]->score / totalWinnerScore) * distributablePool;
                winners[i]->prizeWon = std::floor(share * 100.0) / 100.0;
            }
        }

        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2)
            << R"({"type":"CRASH","roundId":)" << roundId_
            << R"(,"crashPoint":)" << finalCrashMult
            << R"(,"serverSeed":")" << serverSeed_ << R"(")"
            << R"(,"totalPool":)" << totalPool_
            << R"(,"prizePool":)" << distributablePool
            << R"(,"totalWinnerScore":)" << totalWinnerScore
            << R"(,"totalWinners":)" << winners.size()
            << R"(,"leaderboard":[)";

        size_t limit = std::min<size_t>(15, winners.size());
        for (size_t i = 0; i < limit; ++i) {
            if (i > 0) oss << ",";
            oss << R"({"rank":)" << winners[i]->rank
                << R"(,"playerId":")" << winners[i]->playerId
                << R"(","bet":)" << winners[i]->betAmount
                << R"(,"mult":)" << winners[i]->cashoutMultiplier
                << R"(,"score":)" << winners[i]->score
                << R"(,"prize":)" << winners[i]->prizeWon << "}";
        }
        oss << "]}";
        return oss.str();
    }

    void broadcast(const std::string& msg);

    net::steady_timer timer_;
    std::mutex mtx_;
    std::vector<std::shared_ptr<Session>> sessions_;

    uint64_t roundId_ = 0;
    GameState state_ = GameState::WAITING;
    std::chrono::steady_clock::time_point phaseStart_;

    std::string serverSeed_;
    std::string hashCommitment_;
    double cryptoCrashPoint_ = 1.00;
    double currentMultiplier_ = 1.00;
    double totalPool_ = 0.0;

    std::unordered_map<std::string, PlayerEntry> players_;
};

// --- WEBSOCKET SESSION ---
class Session : public std::enable_shared_from_this<Session> {
public:
    Session(tcp::socket&& socket, GameEngine& engine)
        : ws_(std::move(socket)), engine_(engine) {}

    void run() {
        ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));
        ws_.async_accept([self = shared_from_this()](beast::error_code ec) {
            if (!ec) {
                self->engine_.joinSession(self);
                self->doRead();
            }
        });
    }

    void send(const std::string& msg) {
        net::post(ws_.get_executor(), [self = shared_from_this(), msg]() {
            bool writing = !self->outbox_.empty();
            self->outbox_.push_back(msg);
            if (!writing) self->doWrite();
        });
    }

private:
    void doRead() {
        ws_.async_read(buffer_, [self = shared_from_this()](beast::error_code ec, std::size_t) {
            if (ec) {
                self->engine_.leaveSession(self);
                return;
            }
            std::string msg = beast::buffers_to_string(self->buffer_.data());
            self->buffer_.consume(self->buffer_.size());

            std::string reply = self->engine_.handleCommand(msg);
            self->send(reply);
            self->doRead();
        });
    }

    void doWrite() {
        ws_.async_write(net::buffer(outbox_.front()), [self = shared_from_this()](beast::error_code ec, std::size_t) {
            if (ec) {
                self->engine_.leaveSession(self);
                return;
            }
            self->outbox_.erase(self->outbox_.begin());
            if (!self->outbox_.empty()) self->doWrite();
        });
    }

    websocket::stream<beast::tcp_stream> ws_;
    beast::flat_buffer buffer_;
    std::vector<std::string> outbox_;
    GameEngine& engine_;
};

void GameEngine::broadcast(const std::string& msg) {
    std::vector<std::shared_ptr<Session>> snapshot;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        snapshot = sessions_;
    }
    for (auto& s : snapshot) {
        s->send(msg);
    }
}

class Listener : public std::enable_shared_from_this<Listener> {
public:
    Listener(net::io_context& ioc, tcp::endpoint endpoint, GameEngine& engine)
        : ioc_(ioc), acceptor_(ioc), engine_(engine) {
        acceptor_.open(endpoint.protocol());
        acceptor_.set_option(net::socket_base::reuse_address(true));
        acceptor_.bind(endpoint);
        acceptor_.listen(net::socket_base::max_listen_connections);
    }

    void run() { doAccept(); }

private:
    void doAccept() {
        acceptor_.async_accept(net::make_strand(ioc_), [self = shared_from_this()](beast::error_code ec, tcp::socket socket) {
            if (!ec) {
                std::make_shared<Session>(std::move(socket), self->engine_)->run();
            }
            self->doAccept();
        });
    }

    net::io_context& ioc_;
    tcp::acceptor acceptor_;
    GameEngine& engine_;
};

int main() {
    const char* portEnv = std::getenv("PORT");
    unsigned short port = portEnv ? static_cast<unsigned short>(std::stoi(portEnv)) : 8080;

    net::io_context ioc{1};
    GameEngine engine(ioc);
    engine.start();

    std::make_shared<Listener>(ioc, tcp::endpoint{tcp::v4(), port}, engine)->run();
    std::cout << "[Pulse8 C++ Engine] Weighted Pool-Share + 5% Dynamic Cap running on port " << port << std::endl;
    ioc.run();
    return 0;
}
