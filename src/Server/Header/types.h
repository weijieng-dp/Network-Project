#pragma once
#include <map>
#include <chrono>
#include <string>
#include <queue>
#include <functional>
#include <set>
#include <thread>

/*--------------------------------------------------------------------------
 * Exchange data structures
 *--------------------------------------------------------------------------*/

struct Order {
    uint64_t    orderId = 0;
    std::string username, symbol;
    char        side = 'B';
    uint32_t    qty = 0, origQty = 0;
    double      price = 0.0;
    std::chrono::steady_clock::time_point ts;
};

struct Trade {
    uint64_t    tradeId = 0;
    std::string symbol, buyUser, sellUser, datetime;
    uint32_t    qty = 0;
    double      price = 0.0;
};

struct TradePoint { 
    double price; 
    uint32_t qty; 
    std::string datetime; 
};

/** Price/time-priority order book for one symbol. */
struct OrderBook {
    std::map<double, std::map<uint64_t, Order>, std::greater<double>> bids; // highest first
    std::map<double, std::map<uint64_t, Order>>                       asks; // lowest first
    double   lastPrice = 0.0;
    uint32_t volume = 0;
    double   mmVolatility = 1.0;        // rolling volatility for MM spread calculation
    std::vector<TradePoint> tradeLog;   // historical trade prices for charting
};

struct Account {
    std::string                     username;
    std::string                     passwordHash;   // stored hash of password
    double                          cash = 100000.0;
    std::map<std::string, uint32_t> holdings;
    std::map<std::string, double>   avgCost;        // average cost basis per symbol
    std::vector<Trade>              trades;
    std::map<uint64_t, Order>       openOrders;
};

/*--------------------------------------------------------------------------
 * Conditional orders (stop-loss / take-profit)
 *--------------------------------------------------------------------------*/
struct ConditionalOrder {
    uint64_t    id;
    std::string username, symbol;
    char        type;       // 'S' = stop-loss, 'T' = take-profit
    uint32_t    qty;
    double      triggerPrice;
};

/*-------------------------------------------------------------------------
* threadWrapper
* 
* A utility class that wraps a std::thread and automatically
* 
* 1. Executes a given functiion in a seperate thread
* 2. Tracks when the thread has finished execution
* 3. Ensures the thread is joined when the object is destroyed (RAII)
* 
* Used to:
* - Manage it's own's thread completion
*--------------------------------------------------------------------------*/
class threadWrapper {
private:

	// Function object. It takes in a reference to the
	// finish signal of the threadWrapper class.
	// It then calls the function, and sets the signal
	// to true after the function finishes.
    struct Wrap {
        bool& signal;
        std::function<void()> fun;

        template <typename Func, typename... T>
        Wrap(bool& sig, Func&& f, T&&... t) : signal{ sig } {
            fun = std::bind(f, t...);
        }
        void operator()() {
            fun();
            signal = true;
        }
    };

public:
    bool finishSignal{ false };
    Wrap w;
    std::thread th;

    template <typename Func, typename... T>
    threadWrapper(Func&& f, T&&... t) : w(finishSignal, std::forward<Func>(f), std::forward<T>(t)...), th(w) {}

    // Automatically join upon object leaving scope.
    ~threadWrapper() { th.join(); }
};

inline bool operator<(const threadWrapper& t1, const threadWrapper& t2) { return t1.th.get_id() < t2.th.get_id(); }


/*-------------------------------------------------------------------------
* threadPool
*
* A utility class that manages a group of threadWrappers
*
* 1. Stores a max amount of concurrent threads
* 2. Tracks when a thread has finished execution and removes it from storage
* 3. Queues up excess threads to be pushed into storage once space is freed up
*
* Used to:
* - Manage multiple threads
* - Track which threads have completed
* - Safely clean up finished threads
* - Queue up threads exceeding the limit
*--------------------------------------------------------------------------*/
class threadPool {
    std::set<threadWrapper> threadpool;
    std::queue<std::function<void()>> threadQueue;
    uint32_t maxThreads;
    std::thread updateThread;
    bool stopped{ false };
public:

    threadPool(uint32_t count);

    ~threadPool();

    template <typename Func, typename... T>
    void addThread(Func&& f, T&&... t) {
        if (threadpool.size() < maxThreads) threadpool.emplace(f, t...);
        else threadQueue.push(std::bind(f, t...));
    }

    void stopAll();

    void update();

};
