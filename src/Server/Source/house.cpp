
#include "house.h"
#include <iostream>

/*--------------------------------------------------------------------------
 * House / Market-Maker account seeding
 *--------------------------------------------------------------------------*/


/**
 * @brief Reference prices for initial market-maker quotes.
 *        The EXCHANGE account posts sell orders at refPrice and buy orders
 *        at (refPrice - spread) so that clients can immediately trade.
 */
static const std::map<std::string, double> REFERENCE_PRICES = {
    {"AAPL", 180.00}, {"GOOGL", 170.00}, {"MSFT", 420.00},
    {"TSLA", 250.00}, {"AMZN", 185.00}
};

static const uint32_t HOUSE_SHARES_PER_SYM = 1000000;       // 1M shares each
static const double   HOUSE_CASH = 1e10;                    // $10 billion
// Realistic market maker constants (multi-level depth, inventory-aware)
static const int      MM_DEPTH_LEVELS = 5;                  // 5 price levels each side
static const uint32_t MM_BASE_QTY = 100;                    // base order size at best bid/ask
static const double   MM_BASE_SPREAD = 0.20;                // $0.20 minimum spread
static const double   MM_LEVEL_STEP = 0.10;                 // $0.10 between each depth level
static const double   MM_QTY_DECAY = 0.7;                   // each level is 70% of previous size
static const double   MM_INVENTORY_TARGET = 1.0;            // target: hold 100% of initial shares (no initial skew)
static const double   MM_INVENTORY_SKEW = 0.30;             // max spread skew from inventory imbalance
static const double   MM_VOLATILITY_MULT = 0.5;             // spread widens by vol * this factor


/**
 * @brief Create the EXCHANGE account (if not already present) and seed the
 *        order books with initial bid/ask quotes so clients can trade
 *        immediately.
 *
 * Called once at startup, AFTER loadPersistentData().  Must be called with NO other
 * threads running (single-threaded init) so no mutex needed.
 */
 // Forward declaration


void seedMarketMaker() {
    // If the EXCHANGE account was already loaded from persistence, skip seeding
    // to avoid duplicating orders and inflating the account on every restart.
    if (Global::accounts.count(Global::HOUSE_USER)) {
        std::cout << "[HOUSE] EXCHANGE account found in persisted data skipping seed.\n";
        std::cout << "[HOUSE] Cash: $" << std::fixed << std::setprecision(2)
            << Global::accounts[Global::HOUSE_USER].cash << ", holdings: ";
        for (auto& [sym, qty] : Global::accounts[Global::HOUSE_USER].holdings) std::cout << sym << "=" << qty << " ";
        std::cout << "\n\n";
        return;
    }

    // Create the house account fresh (first run only)
    Account& house = Global::accounts[Global::HOUSE_USER];
    house.username = Global::HOUSE_USER;
    house.cash = HOUSE_CASH;
    for (auto& sym : Global::SYMBOLS)
        house.holdings[sym] = HOUSE_SHARES_PER_SYM;

    std::cout << "[HOUSE] EXCHANGE account: $" << std::fixed << std::setprecision(0)
        << house.cash << ", " << HOUSE_SHARES_PER_SYM << " shares/symbol\n";

    // Seed multi-level depth for each symbol using requoteMarketMaker logic
    for (auto& sym : Global::SYMBOLS) {
        double refPx = REFERENCE_PRICES.count(sym) ? REFERENCE_PRICES.at(sym) : 100.0;
        OrderBook& book = Global::books[sym];
        book.lastPrice = refPx;
        book.tradeLog.push_back({ refPx, 0, nowString() });

        // Post 5 levels each side at reference price
        double spread = MM_BASE_SPREAD + book.mmVolatility * MM_VOLATILITY_MULT;
        for (int i = 0; i < MM_DEPTH_LEVELS; ++i) {
            double levelOffset = i * MM_LEVEL_STEP;
            uint32_t qty = (uint32_t)(MM_BASE_QTY * std::pow(MM_QTY_DECAY, i));
            if (qty < 10) qty = 10;

            double askPx = refPx + spread / 2.0 + levelOffset;
            double bidPx = refPx - spread / 2.0 - levelOffset;
            if (bidPx < 0.01) bidPx = 0.01;

            postHouseOrder(sym, 'S', askPx, qty);
            postHouseOrder(sym, 'B', bidPx, qty);
        }
    }

    std::cout << "[HOUSE] Market maker seeded " << Global::SYMBOLS.size() * MM_DEPTH_LEVELS * 2
        << " orders placed (" << MM_DEPTH_LEVELS << " levels/side).\n\n";
}

/*--------------------------------------------------------------------------
 * Continuous Market-Maker  (models real-world designated market makers)
 *--------------------------------------------------------------------------*
 * Real exchanges (NYSE, NASDAQ) have Designated Market Makers (DMMs) that
 * are obligated to continuously quote both a bid and an ask for their
 * assigned symbols.  The exchange itself never owns shares — it is purely
 * a matching venue.  The DMM profits from the bid-ask spread.
 *
 * Our EXCHANGE account models a DMM:
 *   - Always maintains one resting BID and one resting ASK per symbol.
 *   - When its ask is hit (sold shares to a client), it posts a new ask
 *     from its remaining inventory.
 *   - When its bid is hit (bought shares from a client), it posts a new
 *     ask to recycle those shares back to the market.
 *   - Quotes track the last trade price (price discovery) with a spread.
 *   - As long as the EXCHANGE has inventory/cash, liquidity never dries up.
 *--------------------------------------------------------------------------*/

 /**
  * @brief Post a single resting order for the EXCHANGE market maker.
  *
  * Directly inserts into the order book (no matching — the MM is passive).
  * Reserves the appropriate funds/shares from the EXCHANGE account.
  *
  * Called with Global::exMtx held.
  * @return true if order was posted, false if insufficient inventory/cash.
  */
bool postHouseOrder(const std::string& sym, char side, double price, uint32_t qty) {
    Account& house = Global::accounts[Global::HOUSE_USER];

    if (side == 'S') {
        uint32_t avail = house.holdings.count(sym) ? house.holdings[sym] : 0;
        if (avail == 0) return false;
        qty = std::min(qty, avail);
        house.holdings[sym] -= qty;
        if (house.holdings[sym] == 0) house.holdings.erase(sym);
    }
    else {
        double cost = price * qty;
        if (house.cash < cost) return false;
        house.cash -= cost;
    }

    uint64_t oid = Global::nextOrderId.fetch_add(1);
    Order ord;

    ord.orderId = oid; 
    ord.username = Global::HOUSE_USER;
    ord.side = side;
    ord.symbol = sym; 
    ord.qty = qty; 
    ord.origQty = qty; 
    ord.price = price;
    ord.ts = std::chrono::steady_clock::now();

    OrderBook& book = Global::books[sym];
    if (side == 'B') book.bids[price][oid] = ord;
    else             book.asks[price][oid] = ord;
    Global::liveOrders[oid] = ord;
    house.openOrders[oid] = ord;

    {
        std::lock_guard<std::mutex> plk(Global::printMtx);
        std::cout << "[MM " << (side == 'B' ? "BID" : "ASK") << "] " << sym
            << " " << qty << " @ " << std::fixed << std::setprecision(2) << price
            << "  (order #" << oid << ")\n";
    }
    return true;
}

/**
 * @brief Ensure the EXCHANGE market maker has both a bid and an ask for a symbol.
 *
 * Called after every trade involving the EXCHANGE.  Mirrors how a real DMM
 * re-quotes after a fill:
 *   1. Check if an ask exists for this symbol from EXCHANGE.  If not, post one.
 *   2. Check if a bid exists for this symbol from EXCHANGE.  If not, post one.
 *   3. Price tracks the last trade price ± half the spread (price discovery).
 *
 * Called with Global::exMtx held.
 */
void requoteMarketMaker(const std::string& sym) {
    if (!Global::accounts.count(Global::HOUSE_USER)) return;
    Account& house = Global::accounts[Global::HOUSE_USER];
    OrderBook& book = Global::books[sym];

    // Midpoint tracks last trade price (price discovery)
    double mid = book.lastPrice;
    if (mid <= 0) mid = REFERENCE_PRICES.count(sym) ? REFERENCE_PRICES.at(sym) : 100.0;

    // Cancel ALL existing EXCHANGE orders for this symbol
    std::vector<uint64_t> toCancel;
    for (auto& [price, lvl] : book.asks)
        for (auto& [oid, ord] : lvl)
            if (ord.username == Global::HOUSE_USER) toCancel.push_back(oid);
    for (auto& [price, lvl] : book.bids)
        for (auto& [oid, ord] : lvl)
            if (ord.username == Global::HOUSE_USER) toCancel.push_back(oid);
    for (uint64_t oid : toCancel) {
        auto it = Global::liveOrders.find(oid);
        if (it == Global::liveOrders.end()) continue;
        Order& ord = it->second;
        if (ord.side == 'B') {
            house.cash += ord.price * ord.qty;
            book.bids[ord.price].erase(oid);
            if (book.bids[ord.price].empty()) book.bids.erase(ord.price);
        }
        else {
            house.holdings[sym] += ord.qty;
            book.asks[ord.price].erase(oid);
            if (book.asks[ord.price].empty()) book.asks.erase(ord.price);
        }
        Global::liveOrders.erase(oid);
        house.openOrders.erase(oid);
    }

    // Inventory-aware spread: skew quotes to reduce imbalance
    uint32_t currentHoldings = house.holdings.count(sym) ? house.holdings[sym] : 0;
    double inventoryRatio = (double)currentHoldings / (double)HOUSE_SHARES_PER_SYM;
    double imbalance = inventoryRatio - MM_INVENTORY_TARGET;  // >0 = overloaded, <0 = underweight

    // Dynamic spread: widens with volatility
    double spread = MM_BASE_SPREAD + book.mmVolatility * MM_VOLATILITY_MULT;

    // Post multi-level depth (5 ask + 5 bid levels)
    for (int i = 0; i < MM_DEPTH_LEVELS; ++i) {
        double levelOffset = i * MM_LEVEL_STEP;
        uint32_t qty = (uint32_t)(MM_BASE_QTY * std::pow(MM_QTY_DECAY, i));
        if (qty < 10) qty = 10;

        // Inventory skew shifts all prices to reduce imbalance
        double skew = imbalance * MM_INVENTORY_SKEW;

        double askPx = mid + spread / 2.0 + levelOffset - skew;
        double bidPx = mid - spread / 2.0 - levelOffset - skew;
        if (bidPx < 0.01) bidPx = 0.01;

        postHouseOrder(sym, 'S', askPx, qty);
        postHouseOrder(sym, 'B', bidPx, qty);
    }
}
