#include <map>
#include <chrono>

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