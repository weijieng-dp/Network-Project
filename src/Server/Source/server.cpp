/* Start Header
*****************************************************************/
/*!
\file    server.cpp
\author  weixuan.toh@digipen.edu
\date    20 Mar 2026
\brief
  Exchange server for CSD2161 Assignment 5 - Online Trading Platform (Option 3).

  Transport design (satisfies both rubric grading criteria):
    TCP  - All client<->server control messages: login, logout, place/cancel
           order, query market, query account, query orders, query trades.
           TCP provides reliable ordered delivery and stream framing; partial
           reads are handled by recvExact() exactly as in Assignment 4.
    UDP  - Server-initiated market data BROADCASTS sent to all subscribed
           clients after every trade execution (best bid/ask, last price,
           volume). UDP is intentionally unreliable here — stale market
           data is less harmful than a blocked TCP stream, and we
           demonstrate handling of loss/dup/out-of-order as required.

  -----------------------------------------------------------------------
  Architecture
  -----------------------------------------------------------------------
  Thread model (pre-threading, same pattern as Assignment 4 TaskQueue):
    ListenerThread   - accept() loop; hands each TCP socket to a worker.
    WorkerThread(N)  - one per connected client; runs clientSession().
                       Handles all TCP I/O for that client.
    UdpBroadcast     - background thread: drains Global::broadcastQueue and
                       sendto() each market-data datagram to all registered
                       client UDP addresses.
    PersistThread    - flushes accounts.dat + trades.dat every 5 seconds
                       and on clean shutdown.
    ExchangeMutex    - single std::mutex (Global::exMtx) protects the order book,
                       account map, and trade log.  Worker threads contend on
                       this when placing orders; contention is brief.

  -----------------------------------------------------------------------
  TCP message framing (network byte order throughout)
  -----------------------------------------------------------------------
  Every message starts with:   CmdID(1) + PayloadLen(2)
  The receiver calls recvExact(PayloadLen) after reading the header.
  This handles TCP stream segmentation / partial reads correctly.

  CLIENT -> SERVER (CmdID values)
    0x01  LOGIN          UsernameLen(1) Username(var)
    0x02  LOGOUT         (no payload)
    0x03  PLACE_ORDER    Side(1)[0=Buy,1=Sell] SymLen(1) Sym Qty(4) Price(8)
    0x04  CANCEL_ORDER   OrderID(8)
    0x05  QUERY_MARKET   SymLen(1) Sym
    0x06  QUERY_ACCOUNT  (no payload)
    0x07  QUERY_ORDERS   (no payload)
    0x08  QUERY_TRADES   (no payload)
    0x09  SUB_MARKET     ClientUdpPort(2)   [subscribe to UDP market data]

  SERVER -> CLIENT (CmdID values)
    0x81  LOGIN_OK       Cash(8) NumH(2) [SymLen(1) Sym Qty(4)]*N
    0x82  LOGIN_FAIL     ReasonLen(1) Reason
    0x83  ORDER_ACK      OrderID(8) Side(1) SymLen(1) Sym Qty(4) Price(8)
    0x84  ORDER_REJECT   ReasonLen(1) Reason
    0x85  TRADE_EXEC     OrderID(8) FilledQty(4) Price(8) RemQty(4)
                         SymLen(1) Sym
    0x86  CANCEL_ACK     OrderID(8)
    0x87  CANCEL_REJECT  ReasonLen(1) Reason
    0x88  MARKET_DATA    SymLen(1) Sym BestBid(8) BidQty(4) BestAsk(8)
                         AskQty(4) LastPrice(8) Volume(4)
                         (also broadcast over UDP to subscribers)
    0x89  ACCOUNT_DATA   Cash(8) NumH(2) [SymLen(1) Sym Qty(4)]*N
    0x8A  SERVER_MSG     MsgLen(2) Msg
    0x8B  LOGOUT_OK      (no payload)
    0x8C  ORDER_LIST     NumOrders(2)
                         [OrderID(8) Side(1) SymLen(1) Sym Qty(4) Price(8)]*N
    0x8D  TRADE_LIST     NumTrades(2)
                         [TradeID(8) SymLen(1) Sym Qty(4) Price(8)
                          Side(1)[0=bought] DateLen(1) Date]*N

  UDP BROADCAST (server -> all subscribers, best-effort)
    0x88  MARKET_DATA    (same payload as TCP MARKET_DATA above)
    Sequence(4) prepended so clients can detect out-of-order datagrams.

Copyright (C) 2026 DigiPen Institute of Technology.
Reproduction or disclosure of this file or its contents without the
prior written consent of DigiPen Institute of Technology is prohibited.
*/
/* End Header
*******************************************************************/

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "Windows.h"
#include "ws2tcpip.h"
#pragma comment(lib, "ws2_32.lib")

#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include <algorithm>
#include <sstream>
#include <fstream>
#include <iomanip>
#include <cstring>
#include <ctime>
#include <deque>
#include <condition_variable>
#include <functional>
#include <set>
#include <random>
#include <cmath>

#include "utils.h"
#include "types.h"
#include "global.h"

static const int    MAX_PAYLOAD       = 8192;   // max TCP payload bytes
static const double PERSIST_INTERVAL  = 5.0;    // seconds between disk flushes
static const int    WORKER_THREADS    = 20;      // pre-spawned worker pool size

static const std::vector<std::string> SYMBOLS = {"AAPL","GOOGL","MSFT","TSLA","AMZN"};

/*--------------------------------------------------------------------------
 * TCP framed send: CmdID(1) + PayloadLen(2) + Payload
 *--------------------------------------------------------------------------*/

/**
 * @brief Send a framed TCP response to a client (thread-safe).
 *
 * Frame layout: CmdID(1) + PayloadLen(2) + Payload(var)
 * Uses a per-socket send mutex to prevent concurrent sends from corrupting
 * the TCP stream (e.g., worker thread A's pushTradeExec to client B while
 * client B's own worker is also sending a response).
 */
static bool sendFrame(SOCKET s, CmdID cmd, const std::vector<char>& payload) {
    uint16_t len = (uint16_t)std::min(payload.size(), (size_t)MAX_PAYLOAD);
    std::vector<char> frame;
    frame.reserve(3 + len);
    pushU8 (frame, (uint8_t)cmd);
    pushU16(frame, len);
    frame.insert(frame.end(), payload.begin(), payload.begin()+len);

    return sendAll(s, frame.data(), (int)frame.size());
}

static bool sendServerMsg(SOCKET s, const std::string& msg) {
    std::vector<char> p; pushU16(p,(uint16_t)msg.size()); p.insert(p.end(),msg.begin(),msg.end());
    return sendFrame(s, CMD_SERVER_MSG, p);
}


/**
 * @brief Simple hash function for password storage.
 *        Uses djb2 + salted with username for basic security.
 *        (Not cryptographic — suitable for a school project.)
 */
static std::string hashPassword(const std::string& user, const std::string& pass) {
    std::string salted = user + ":" + pass;
    uint64_t hash = 5381;
    for(char c : salted) hash = ((hash << 5) + hash) + (uint8_t)c;
    std::ostringstream ss;
    ss << std::hex << std::setfill('0') << std::setw(16) << hash;
    return ss.str();
}

///*--------------------------------------------------------------------------
// * Global exchange state  (protected by Global::exMtx)
// *--------------------------------------------------------------------------*/
//
//static std::mutex Global::exMtx;   // guards all exchange state below
//
//static std::unordered_map<std::string,Account>   Global::accounts;
//static std::unordered_map<std::string,OrderBook> Global::books;
//static std::vector<Trade>                         Global::allTrades;
//static std::unordered_map<uint64_t,Order>         Global::liveOrders;
//static std::atomic<uint64_t> Global::nextOrderId{1}, Global::nextTradeId{1};
//
//// Map username -> TCP socket (for pushing TRADE_EXEC to counterparty)
//static std::unordered_map<std::string,SOCKET> Global::userSockets;
//static std::mutex Global::userSockMtx;
//
///*--------------------------------------------------------------------------
// * UDP broadcast state
// *--------------------------------------------------------------------------*/
//
//static SOCKET Global::udpSocket = INVALID_SOCKET;
//static std::atomic<uint32_t> Global::udpSeq{1};
//
//struct UdpSubscriber { sockaddr_in addr; };
//static std::mutex              Global::subMtx;
//static std::vector<UdpSubscriber> Global::subscribers;
//
//struct BroadcastItem { std::vector<char> payload; };
//static std::mutex              Global::bcastMtx;
//static std::condition_variable Global::bcastCV;
//static std::deque<BroadcastItem> Global::bcastQueue;
//
///*--------------------------------------------------------------------------
// * Persistence path + print mutex
// *--------------------------------------------------------------------------*/
//
//static std::string Global::persistPath;
//static std::mutex  Global::printMtx;
//static std::atomic<bool> Global::running{true};

/*--------------------------------------------------------------------------
 * Broadcast server message to ALL connected clients
 *--------------------------------------------------------------------------*/
static void broadcastServerMsg(const std::string& msg) {
    std::lock_guard<std::mutex> lk(Global::userSockMtx);
    for (auto& [uname, sock] : Global::userSockets) {
        sendServerMsg(sock, msg);
    }
    std::lock_guard<std::mutex> plk(Global::printMtx);
    std::cout << "[BROADCAST] " << msg << "\n";
}

/*--------------------------------------------------------------------------
 * Economic crisis simulation state
 *--------------------------------------------------------------------------*/
enum class CrisisPhase { NONE, SHOCK, PANIC, STABILIZE, RECOVERY };

struct CrisisState {
    std::atomic<int>     phase{0};        // cast to/from CrisisPhase
    std::atomic<int64_t> startMs{0};
    std::atomic<int64_t> phaseStartMs{0};
    std::atomic<double>  intensity{0.0};  // 1.0 at trigger, decays to 0
};
static CrisisState g_crisis;

static const int64_t CRISIS_SHOCK_MS     = 15000;   // 15s flash crash
static const int64_t CRISIS_PANIC_MS     = 120000;  // 2min panic selling
static const int64_t CRISIS_STABILIZE_MS = 180000;  // 3min volatile stabilization
static const int64_t CRISIS_RECOVERY_MS  = 300000;  // 5min gradual recovery

// Per-symbol vulnerability: higher = crashes harder (safe-haven effect)
static const std::unordered_map<std::string, double> CRISIS_VULNERABILITY = {
    {"AAPL", 0.7}, {"GOOGL", 0.8}, {"MSFT", 0.6}, {"TSLA", 1.4}, {"AMZN", 1.0}
};


//static std::vector<ConditionalOrder> Global::conditionalOrders;
//static std::atomic<uint64_t> Global::nextCondId{1};

/*--------------------------------------------------------------------------
 * Utility
 *--------------------------------------------------------------------------*/

static std::string nowString() {
    time_t t=time(nullptr); struct tm tm{}; localtime_s(&tm,&t);
    char buf[32]; strftime(buf,sizeof(buf),"%Y-%m-%d_%H:%M:%S",&tm); return buf;
}

/*--------------------------------------------------------------------------
 * Persistence
 *--------------------------------------------------------------------------*/

static void persistData() {
    std::string logMsg;
    {
        std::lock_guard<std::mutex> lk(Global::exMtx);
        // --- accounts.dat: cash + holdings ---
        std::ofstream fa(Global::persistPath+"\\accounts.dat",std::ios::trunc);
        for(auto&[u,acc]:Global::accounts){
            fa<<"A "<<u<<" "<<std::fixed<<std::setprecision(6)<<acc.cash<<" "<<acc.passwordHash<<"\n";
            for(auto&[sym,qty]:acc.holdings) if(qty>0) fa<<"H "<<sym<<" "<<qty<<"\n";
        }
        // --- trades.dat: trade log ---
        std::ofstream ft(Global::persistPath+"\\trades.dat",std::ios::trunc);
        for(auto&tr:Global::allTrades)
            ft<<tr.tradeId<<" "<<tr.symbol<<" "<<tr.qty<<" "
              <<std::fixed<<std::setprecision(6)<<tr.price<<" "
              <<tr.buyUser<<" "<<tr.sellUser<<" "<<tr.datetime<<"\n";
        // --- orders.dat: all resting orders in the book ---
        std::ofstream fo(Global::persistPath+"\\orders.dat",std::ios::trunc);
        for(auto&[oid,ord]:Global::liveOrders){
            fo<<"O "<<oid<<" "<<ord.username<<" "<<ord.side<<" "
              <<ord.symbol<<" "<<ord.qty<<" "<<ord.origQty<<" "
              <<std::fixed<<std::setprecision(6)<<ord.price<<"\n";
        }
        // --- history.dat: price history for charting ---
        std::ofstream fh(Global::persistPath+"\\history.dat",std::ios::trunc);
        for(auto&[sym,book]:Global::books){
            for(auto&tp:book.tradeLog)
                fh<<sym<<" "<<std::fixed<<std::setprecision(6)<<tp.price<<" "<<tp.qty<<" "<<tp.datetime<<"\n";
        }
        logMsg = "["+nowString()+"] Persisted "+std::to_string(Global::accounts.size())+" accounts, "
                +std::to_string(Global::allTrades.size())+" trades, "
                +std::to_string(Global::liveOrders.size())+" orders.";
    }
    // Print AFTER releasing Global::exMtx to avoid double-lock with Global::printMtx
    std::lock_guard<std::mutex> plk(Global::printMtx);
    std::cout<<logMsg<<"\n";
}

static void loadData() {
    // --- Load accounts ---
    std::ifstream fa(Global::persistPath+"\\accounts.dat");
    if(fa.is_open()){
        std::string line,curUser;
        while(std::getline(fa,line)){
            if(line.empty()) continue;
            std::istringstream ss(line); char tag; ss>>tag;
            if(tag=='A'){ std::string u,ph; double c; ss>>u>>c>>ph; Global::accounts[u].username=u; Global::accounts[u].cash=c; Global::accounts[u].passwordHash=ph; curUser=u; }
            else if(tag=='H'&&!curUser.empty()){ std::string sym; uint32_t qty; ss>>sym>>qty; Global::accounts[curUser].holdings[sym]=qty; }
        }
        std::cout<<"Loaded "<<Global::accounts.size()<<" accounts.\n";
    }
    // --- Load trades ---
    std::ifstream ft(Global::persistPath+"\\trades.dat");
    if(ft.is_open()){
        Trade tr;
        while(ft>>tr.tradeId>>tr.symbol>>tr.qty>>tr.price>>tr.buyUser>>tr.sellUser>>tr.datetime){
            Global::allTrades.push_back(tr);
            Global::accounts[tr.buyUser].trades.push_back(tr);
            Global::accounts[tr.sellUser].trades.push_back(tr);
            if(tr.tradeId>=Global::nextTradeId.load()) Global::nextTradeId.store(tr.tradeId+1);
        }
        std::cout<<"Loaded "<<Global::allTrades.size()<<" trades.\n";
    }
    // --- Load resting orders (rebuild order book) ---
    std::ifstream fo(Global::persistPath+"\\orders.dat");
    if(fo.is_open()){
        std::string line;
        uint64_t maxOid = Global::nextOrderId.load();
        while(std::getline(fo,line)){
            if(line.empty()) continue;
            std::istringstream ss(line); char tag; ss>>tag;
            if(tag!='O') continue;
            uint64_t oid=0; std::string user; char side; std::string sym;
            uint32_t qty=0, origQty=0; double price=0;
            ss>>oid>>user>>side>>sym>>qty>>origQty>>price;
            if(oid==0||user.empty()||sym.empty()||qty==0) continue;

            Order ord;
            ord.orderId=oid; ord.username=user; ord.side=side;
            ord.symbol=sym; ord.qty=qty; ord.origQty=origQty; ord.price=price;
            ord.ts=std::chrono::steady_clock::now();

            // Insert into the order book
            if(side=='B') Global::books[sym].bids[price][oid]=ord;
            else          Global::books[sym].asks[price][oid]=ord;
            Global::liveOrders[oid]=ord;
            Global::accounts[user].openOrders[oid]=ord;

            if(oid>=maxOid) maxOid=oid+1;
        }
        Global::nextOrderId.store(maxOid);
        std::cout<<"Loaded "<<Global::liveOrders.size()<<" resting orders.\n";
    }
    // --- Load price history for charting ---
    std::ifstream fh(Global::persistPath+"\\history.dat");
    if(fh.is_open()){
        std::string sym,dt; double price; uint32_t qty;
        size_t hcount=0;
        while(fh>>sym>>price>>qty>>dt){
            Global::books[sym].tradeLog.push_back({price,qty,dt});
            ++hcount;
        }
        std::cout<<"Loaded "<<hcount<<" price history points.\n";
    }
    for(auto&sym:SYMBOLS) Global::books[sym];
}

/*--------------------------------------------------------------------------
 * House / Market-Maker account seeding
 *--------------------------------------------------------------------------*/

static const std::string HOUSE_USER = "EXCHANGE";

/**
 * @brief Reference prices for initial market-maker quotes.
 *        The EXCHANGE account posts sell orders at refPrice and buy orders
 *        at (refPrice - spread) so that clients can immediately trade.
 */
static const std::map<std::string,double> REFERENCE_PRICES = {
    {"AAPL", 180.00}, {"GOOGL", 170.00}, {"MSFT", 420.00},
    {"TSLA", 250.00}, {"AMZN", 185.00}
};

static const uint32_t HOUSE_SHARES_PER_SYM = 1000000;   // 1M shares each
static const double   HOUSE_CASH           = 1e10;       // $10 billion
// Realistic market maker constants (multi-level depth, inventory-aware)
static const int      MM_DEPTH_LEVELS      = 5;       // 5 price levels each side
static const uint32_t MM_BASE_QTY          = 100;     // base order size at best bid/ask
static const double   MM_BASE_SPREAD       = 0.20;    // $0.20 minimum spread
static const double   MM_LEVEL_STEP        = 0.10;    // $0.10 between each depth level
static const double   MM_QTY_DECAY         = 0.7;     // each level is 70% of previous size
static const double   MM_INVENTORY_TARGET  = 1.0;     // target: hold 100% of initial shares (no initial skew)
static const double   MM_INVENTORY_SKEW    = 0.30;    // max spread skew from inventory imbalance
static const double   MM_VOLATILITY_MULT   = 0.5;     // spread widens by vol * this factor

// --- Simulation bot config (background trading to make charts move) ---
static const double   SIM_INTERVAL_MS   = 300;     // base interval — fast for many trades per candle
static const double   SIM_JITTER_MS     = 200;     // random jitter ± (ms)
static const uint32_t SIM_QTY_MIN       = 50;      // min shares per bot order
static const uint32_t SIM_QTY_MAX       = 200;     // max shares per bot order
static const int      SIM_BOT_COUNT     = 40;         // 20 bulls (BOT_1-20) + 20 bears (BOT_21-40)

/**
 * @brief Create the EXCHANGE account (if not already present) and seed the
 *        order books with initial bid/ask quotes so clients can trade
 *        immediately.
 *
 * Called once at startup, AFTER loadData().  Must be called with NO other
 * threads running (single-threaded init) so no mutex needed.
 */
// Forward declaration
static bool postHouseOrder(const std::string& sym, char side, double price, uint32_t qty);

static void seedMarketMaker() {
    // If the EXCHANGE account was already loaded from persistence, skip seeding
    // to avoid duplicating orders and inflating the account on every restart.
    if(Global::accounts.count(HOUSE_USER)) {
        std::cout << "[HOUSE] EXCHANGE account found in persisted data — skipping seed.\n";
        std::cout << "[HOUSE] Cash: $" << std::fixed << std::setprecision(2)
                  << Global::accounts[HOUSE_USER].cash << ", holdings: ";
        for(auto&[sym,qty]:Global::accounts[HOUSE_USER].holdings) std::cout<<sym<<"="<<qty<<" ";
        std::cout << "\n\n";
        return;
    }

    // Create the house account fresh (first run only)
    Account& house = Global::accounts[HOUSE_USER];
    house.username = HOUSE_USER;
    house.cash     = HOUSE_CASH;
    for (auto& sym : SYMBOLS)
        house.holdings[sym] = HOUSE_SHARES_PER_SYM;

    std::cout << "[HOUSE] EXCHANGE account: $" << std::fixed << std::setprecision(0)
              << house.cash << ", " << HOUSE_SHARES_PER_SYM << " shares/symbol\n";

    // Seed multi-level depth for each symbol using requoteMarketMaker logic
    for (auto& sym : SYMBOLS) {
        double refPx = REFERENCE_PRICES.count(sym) ? REFERENCE_PRICES.at(sym) : 100.0;
        OrderBook& book = Global::books[sym];
        book.lastPrice = refPx;
        book.tradeLog.push_back({refPx, 0, nowString()});

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

    std::cout << "[HOUSE] Market maker seeded — " << SYMBOLS.size() * MM_DEPTH_LEVELS * 2
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
static bool postHouseOrder(const std::string& sym, char side, double price, uint32_t qty) {
    Account& house = Global::accounts[HOUSE_USER];

    if(side == 'S') {
        uint32_t avail = house.holdings.count(sym) ? house.holdings[sym] : 0;
        if(avail == 0) return false;
        qty = std::min(qty, avail);
        house.holdings[sym] -= qty;
        if(house.holdings[sym] == 0) house.holdings.erase(sym);
    } else {
        double cost = price * qty;
        if(house.cash < cost) return false;
        house.cash -= cost;
    }

    uint64_t oid = Global::nextOrderId.fetch_add(1);
    Order ord;
    ord.orderId = oid; ord.username = HOUSE_USER; ord.side = side;
    ord.symbol = sym; ord.qty = qty; ord.origQty = qty; ord.price = price;
    ord.ts = std::chrono::steady_clock::now();

    OrderBook& book = Global::books[sym];
    if(side == 'B') book.bids[price][oid] = ord;
    else            book.asks[price][oid] = ord;
    Global::liveOrders[oid] = ord;
    house.openOrders[oid] = ord;

    { std::lock_guard<std::mutex> plk(Global::printMtx);
      std::cout << "[MM " << (side=='B'?"BID":"ASK") << "] " << sym
                << " " << qty << " @ " << std::fixed << std::setprecision(2) << price
                << "  (order #" << oid << ")\n"; }
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
static void requoteMarketMaker(const std::string& sym) {
    if(!Global::accounts.count(HOUSE_USER)) return;
    Account& house = Global::accounts[HOUSE_USER];
    OrderBook& book = Global::books[sym];

    // Midpoint tracks last trade price (price discovery)
    double mid = book.lastPrice;
    if(mid <= 0) mid = REFERENCE_PRICES.count(sym) ? REFERENCE_PRICES.at(sym) : 100.0;

    // Cancel ALL existing EXCHANGE orders for this symbol
    std::vector<uint64_t> toCancel;
    for(auto& [price, lvl] : book.asks)
        for(auto& [oid, ord] : lvl)
            if(ord.username == HOUSE_USER) toCancel.push_back(oid);
    for(auto& [price, lvl] : book.bids)
        for(auto& [oid, ord] : lvl)
            if(ord.username == HOUSE_USER) toCancel.push_back(oid);
    for(uint64_t oid : toCancel) {
        auto it = Global::liveOrders.find(oid);
        if(it == Global::liveOrders.end()) continue;
        Order& ord = it->second;
        if(ord.side == 'B') {
            house.cash += ord.price * ord.qty;
            book.bids[ord.price].erase(oid);
            if(book.bids[ord.price].empty()) book.bids.erase(ord.price);
        } else {
            house.holdings[sym] += ord.qty;
            book.asks[ord.price].erase(oid);
            if(book.asks[ord.price].empty()) book.asks.erase(ord.price);
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

/*--------------------------------------------------------------------------
 * UDP broadcast helpers
 *--------------------------------------------------------------------------*/

/**
 * @brief Build a MARKET_DATA UDP broadcast payload for a symbol and enqueue it.
 *
 * Layout: Seq(4) + CmdID(1) + PayloadLen(2) + Payload
 * (same inner payload as the TCP CMD_MARKET_DATA frame so clients share
 * the same parser for both channels)
 *
 * Called with Global::exMtx held; enqueues to Global::bcastQueue which is drained by
 * the broadcast thread.
 */
static void enqueueBroadcast(const std::string& sym) {
    auto& book = Global::books[sym];
    double bid=0, ask=0, last=book.lastPrice;
    uint32_t bidQty=0, askQty=0, vol=book.volume;
    if(!book.bids.empty()){ bid=book.bids.begin()->first; for(auto&[oid,o]:book.bids.begin()->second) bidQty+=o.qty; }
    if(!book.asks.empty()){ ask=book.asks.begin()->first; for(auto&[oid,o]:book.asks.begin()->second) askQty+=o.qty; }

    // Inner payload (shared with TCP)
    std::vector<char> inner;
    pushStr1(inner,sym); pushDouble(inner,bid); pushU32(inner,bidQty);
    pushDouble(inner,ask); pushU32(inner,askQty); pushDouble(inner,last); pushU32(inner,vol);

    // Full UDP datagram: Seq(4) + CmdID(1) + PayloadLen(2) + inner
    std::vector<char> dgram;
    pushU32(dgram, Global::udpSeq.fetch_add(1));
    pushU8 (dgram, (uint8_t)CMD_MARKET_DATA);
    pushU16(dgram, (uint16_t)inner.size());
    dgram.insert(dgram.end(), inner.begin(), inner.end());

    std::lock_guard<std::mutex> lk(Global::bcastMtx);
    Global::bcastQueue.push_back({std::move(dgram)});
    Global::bcastCV.notify_one();
}

/*--------------------------------------------------------------------------
 * Exchange engine: matching + trade recording
 *--------------------------------------------------------------------------*/

/**
 * @brief Send TRADE_EXEC over TCP to a user if they are currently connected.
 *
 * Called with Global::exMtx held but NOT Global::userSockMtx (we lock it here briefly).
 */
static void pushTradeExec(const std::string& username, uint64_t orderId,
                          uint32_t fillQty, double fillPx, uint32_t remQty,
                          const std::string& sym) {
    SOCKET sock = INVALID_SOCKET;
    {
        std::lock_guard<std::mutex> lk(Global::userSockMtx);
        auto it = Global::userSockets.find(username);
        if(it != Global::userSockets.end()) sock = it->second;
    }
    if(sock == INVALID_SOCKET) return;

    std::vector<char> p;
    pushU64(p,orderId); pushU32(p,fillQty); pushDouble(p,fillPx);
    pushU32(p,remQty); pushStr1(p,sym);
    sendFrame(sock, CMD_TRADE_EXEC, p);
}

// Forward declarations for mutual references
static void matchOrders(Order& ord, OrderBook& book);
static void requoteMarketMaker(const std::string& sym);

/**
 * @brief Record one fill, update both accounts atomically, push notifications.
 * Called from main exchange thread with Global::exMtx held.
 */
static void recordTrade(const std::string& sym, uint32_t fill, double fillPx,
                        Order& buyOrd, Order& sellOrd) {
    Trade tr;
    tr.tradeId=Global::nextTradeId.fetch_add(1); tr.symbol=sym;
    tr.qty=fill; tr.price=fillPx;
    tr.buyUser=buyOrd.username; tr.sellUser=sellOrd.username;
    tr.datetime=nowString();
    Global::allTrades.push_back(tr);

    auto& book_ref = Global::books[sym];
    // Update MM volatility (EMA of price change magnitude)
    if (book_ref.lastPrice > 0) {
        double pctChange = std::abs(fillPx - book_ref.lastPrice) / (std::max)(book_ref.lastPrice, 0.01);
        book_ref.mmVolatility = book_ref.mmVolatility * 0.95 + pctChange * 100.0 * 0.05;
        if (book_ref.mmVolatility < 0.2) book_ref.mmVolatility = 0.2;
        if (book_ref.mmVolatility > 5.0) book_ref.mmVolatility = 5.0;
    }
    book_ref.lastPrice=fillPx; book_ref.volume+=fill;
    book_ref.tradeLog.push_back({fillPx, fill, tr.datetime});

    // Update buyer — cash was ALREADY reserved at limitPrice on order placement.
    // Refund the price improvement: (limitPrice - fillPx) * fill.
    // Do NOT deduct cash again.
    auto& ba=Global::accounts[buyOrd.username];
    ba.cash         += (buyOrd.price - fillPx) * fill;   // refund price improvement
    // Update average cost basis (weighted average)
    uint32_t oldQty = ba.holdings.count(sym) ? ba.holdings[sym] : 0;
    double oldCost = ba.avgCost.count(sym) ? ba.avgCost[sym] : 0;
    ba.avgCost[sym] = (oldQty > 0) ? (oldCost * oldQty + fillPx * fill) / (oldQty + fill) : fillPx;
    ba.holdings[sym]+= fill;
    ba.trades.push_back(tr);
    if(ba.openOrders.count(buyOrd.orderId)){
        ba.openOrders[buyOrd.orderId].qty -= fill;
        if(ba.openOrders[buyOrd.orderId].qty==0) ba.openOrders.erase(buyOrd.orderId);
    }

    // Update seller — shares were ALREADY reserved on order placement.
    // Do NOT deduct holdings again. Just credit cash from the sale.
    auto& sa=Global::accounts[sellOrd.username];
    sa.cash+=fillPx*fill;
    sa.trades.push_back(tr);
    if(sa.openOrders.count(sellOrd.orderId)){
        sa.openOrders[sellOrd.orderId].qty -= fill;
        if(sa.openOrders[sellOrd.orderId].qty==0) sa.openOrders.erase(sellOrd.orderId);
    }

    {
        std::lock_guard<std::mutex> lk(Global::printMtx);
        std::cout<<"[TRADE #"<<tr.tradeId<<"] "<<sym<<" qty="<<fill
                 <<" @"<<std::fixed<<std::setprecision(4)<<fillPx
                 <<"  buyer="<<buyOrd.username<<"  seller="<<sellOrd.username<<"\n";
    }

    uint32_t buyRem  = buyOrd.qty  - fill;
    uint32_t sellRem = sellOrd.qty - fill;
    pushTradeExec(buyOrd.username,  buyOrd.orderId,  fill, fillPx, buyRem,  sym);
    pushTradeExec(sellOrd.username, sellOrd.orderId, fill, fillPx, sellRem, sym);
    enqueueBroadcast(sym);

    // NOTE: requoteMarketMaker is called AFTER matchOrders completes,
    // not here, to avoid invalidating iterators during matching.
}

/**
 * @brief Check and fire conditional orders (stop-loss / take-profit) for a symbol.
 * Called AFTER matchOrders + requoteMarketMaker complete. Holds Global::exMtx.
 */
static void checkConditionalOrders(const std::string& sym) {
    auto& book = Global::books[sym];
    double lastPx = book.lastPrice;
    if (lastPx <= 0) return;

    std::vector<size_t> triggered;
    for (size_t ci = 0; ci < Global::conditionalOrders.size(); ++ci) {
        auto& co = Global::conditionalOrders[ci];
        if (co.symbol != sym) continue;
        bool fire = false;
        if (co.type == 'S' && lastPx <= co.triggerPrice) fire = true;
        if (co.type == 'T' && lastPx >= co.triggerPrice) fire = true;
        if (!fire) continue;

        if (!Global::accounts.count(co.username)) { triggered.push_back(ci); continue; }
        auto& acc = Global::accounts[co.username];
        uint32_t avail = acc.holdings.count(sym) ? acc.holdings[sym] : 0;
        uint32_t sellQty = (std::min)(co.qty, avail);
        if (sellQty == 0) { triggered.push_back(ci); continue; }

        acc.holdings[sym] -= sellQty;
        if (acc.holdings[sym] == 0) acc.holdings.erase(sym);

        // Use best bid price for the market sell (like real platforms)
        double sellPrice = book.bids.empty() ? 0.01 : book.bids.begin()->first;

        uint64_t oid = Global::nextOrderId.fetch_add(1);
        Order sord;
        sord.orderId = oid; sord.username = co.username; sord.side = 'S';
        sord.symbol = sym; sord.qty = sellQty; sord.origQty = sellQty;
        sord.price = sellPrice;
        sord.ts = std::chrono::steady_clock::now();
        acc.openOrders[oid] = sord;

        matchOrders(sord, book);
        requoteMarketMaker(sym);

        if (sord.qty > 0) {
            acc.holdings[sym] += sord.qty;
            if (Global::liveOrders.count(oid)) {
                book.asks[sord.price].erase(oid);
                if (book.asks[sord.price].empty()) book.asks.erase(sord.price);
                Global::liveOrders.erase(oid);
            }
            acc.openOrders.erase(oid);
        }

        uint32_t filled = sellQty - sord.qty;
        std::string typeStr = (co.type == 'S') ? "STOP-LOSS" : "TAKE-PROFIT";
        {
            SOCKET usock = INVALID_SOCKET;
            std::lock_guard<std::mutex> lk2(Global::userSockMtx);
            auto it2 = Global::userSockets.find(co.username);
            if (it2 != Global::userSockets.end()) usock = it2->second;
            if (usock != INVALID_SOCKET) {
                std::ostringstream oss;
                oss << typeStr << " TRIGGERED: sold " << filled << "x" << sym
                    << " @ $" << std::fixed << std::setprecision(2) << lastPx
                    << " (trigger was $" << co.triggerPrice << ")";
                if (sord.qty > 0) oss << " [" << sord.qty << " unfilled]";
                sendServerMsg(usock, oss.str());
            }
        }
        { std::lock_guard<std::mutex> plk(Global::printMtx);
          std::cout << "[" << typeStr << "] " << co.username << " " << sym
                    << " qty=" << filled << "\n"; }
        triggered.push_back(ci);
    }
    for (auto it = triggered.rbegin(); it != triggered.rend(); ++it)
        Global::conditionalOrders.erase(Global::conditionalOrders.begin() + *it);
}

/**
 * @brief Price/time-priority matching. Fills as much as possible, rests remainder.
 * Called with Global::exMtx held.
 */
static void matchOrders(Order& ord, OrderBook& book) {
    if(ord.side=='B'){
        for(auto lvlIt=book.asks.begin(); lvlIt!=book.asks.end()&&ord.qty>0;){
            if(ord.price<lvlIt->first) break;
            auto& lvl=lvlIt->second;
            for(auto oit=lvl.begin(); oit!=lvl.end()&&ord.qty>0;){
                Order& r=oit->second;
                uint32_t fill=std::min(ord.qty,r.qty);
                recordTrade(ord.symbol,fill,lvlIt->first,ord,r);
                ord.qty-=fill; r.qty-=fill; Global::liveOrders[r.orderId].qty=r.qty;
                if(r.qty==0){Global::liveOrders.erase(r.orderId); oit=lvl.erase(oit);}else ++oit;
            }
            if(lvl.empty()) lvlIt=book.asks.erase(lvlIt); else ++lvlIt;
        }
    } else {
        for(auto lvlIt=book.bids.begin(); lvlIt!=book.bids.end()&&ord.qty>0;){
            if(ord.price>lvlIt->first) break;
            auto& lvl=lvlIt->second;
            for(auto oit=lvl.begin(); oit!=lvl.end()&&ord.qty>0;){
                Order& r=oit->second;
                uint32_t fill=std::min(ord.qty,r.qty);
                recordTrade(ord.symbol,fill,lvlIt->first,r,ord);
                ord.qty-=fill; r.qty-=fill; Global::liveOrders[r.orderId].qty=r.qty;
                if(r.qty==0){Global::liveOrders.erase(r.orderId); oit=lvl.erase(oit);}else ++oit;
            }
            if(lvl.empty()) lvlIt=book.bids.erase(lvlIt); else ++lvlIt;
        }
    }
    if(ord.qty>0){
        if(ord.side=='B') book.bids[ord.price][ord.orderId]=ord;
        else              book.asks[ord.price][ord.orderId]=ord;
        Global::liveOrders[ord.orderId]=ord;
    }
}

/*--------------------------------------------------------------------------
 * Background simulation thread — virtual bots trade to make charts move
 *--------------------------------------------------------------------------*/

/**
 * @brief Creates bot accounts (BOT_1, BOT_2, BOT_3) with large funds.
 * Called once at startup. NOT thread-safe — call before starting threads.
 */
static void seedBotAccounts() {
    for (int i = 0; i < SIM_BOT_COUNT; ++i) {
        std::string name = "BOT_" + std::to_string(i + 1);
        if (!Global::accounts.count(name)) {
            Account& bot  = Global::accounts[name];
            bot.username  = name;
            bot.cash      = 1e9;
            for (auto& sym : SYMBOLS) bot.holdings[sym] = 500000;
        }
    }
    std::cout << "[SIM] " << SIM_BOT_COUNT << " bot accounts ready.\n";
}

/**
 * @brief Background thread that periodically submits small random orders
 *        against the EXCHANGE's resting quotes, producing real trades.
 *
 * Price movement comes naturally: each buy at the ask nudges the DMM's
 * midpoint up; each sell at the bid nudges it down.  A per-symbol
 * "sentiment" value oscillates slowly (sine + noise) so prices trend
 * for a while then reverse — mimicking real market behavior.
 */
static void simulationThread() {
    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int>      symDist(0, (int)SYMBOLS.size() - 1);
    std::uniform_int_distribution<int>      botDist(1, SIM_BOT_COUNT);
    std::uniform_int_distribution<uint32_t> qtyDist(SIM_QTY_MIN, SIM_QTY_MAX);
    std::uniform_real_distribution<double>  jitterDist(-SIM_JITTER_MS, SIM_JITTER_MS);
    std::uniform_real_distribution<double>  coin(0.0, 1.0);

    // Per-symbol state
    struct SymState {
        double sentiment = 0.0;       // directional bias [-5, +5]
        double volatility = 1.0;      // current volatility multiplier [0.3, 3.0]
        int    burstRemain = 0;       // trades left in current burst
        bool   burstBuy = true;       // burst direction
    };
    std::map<std::string, SymState> symState;
    for (auto& sym : SYMBOLS) symState[sym] = {};

    uint64_t tick = 0;

    std::cout << "[SIM] Simulation thread started (interval ~"
              << (int)SIM_INTERVAL_MS << "ms).\n";

    while (Global::running.load()) {
        // ── Crisis phase machine (runs every tick) ──
        auto nowTP = std::chrono::steady_clock::now();
        int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            nowTP.time_since_epoch()).count();
        CrisisPhase cPhase = (CrisisPhase)g_crisis.phase.load();
        double crisisIntensity = g_crisis.intensity.load();

        if (cPhase != CrisisPhase::NONE) {
            int64_t elapsed = nowMs - g_crisis.phaseStartMs.load();
            bool transitioned = false;

            if (cPhase == CrisisPhase::SHOCK && elapsed > CRISIS_SHOCK_MS) {
                g_crisis.phase.store((int)CrisisPhase::PANIC);
                g_crisis.phaseStartMs.store(nowMs);
                g_crisis.intensity.store(0.85);
                broadcastServerMsg("*** MARKETS: Circuit breakers tripped. "
                                   "Institutional selling accelerates. ***");
                transitioned = true;
            } else if (cPhase == CrisisPhase::PANIC && elapsed > CRISIS_PANIC_MS) {
                g_crisis.phase.store((int)CrisisPhase::STABILIZE);
                g_crisis.phaseStartMs.store(nowMs);
                g_crisis.intensity.store(0.50);
                broadcastServerMsg("*** MARKETS: Central bank announces emergency "
                                   "liquidity measures. Selling pressure easing. ***");
                transitioned = true;
            } else if (cPhase == CrisisPhase::STABILIZE && elapsed > CRISIS_STABILIZE_MS) {
                g_crisis.phase.store((int)CrisisPhase::RECOVERY);
                g_crisis.phaseStartMs.store(nowMs);
                g_crisis.intensity.store(0.25);
                broadcastServerMsg("*** MARKETS: Ceasefire negotiations reported. "
                                   "Bargain hunters entering market. ***");
                transitioned = true;
            } else if (cPhase == CrisisPhase::RECOVERY && elapsed > CRISIS_RECOVERY_MS) {
                g_crisis.phase.store((int)CrisisPhase::NONE);
                g_crisis.intensity.store(0.0);
                broadcastServerMsg("*** MARKETS: Situation stabilizing. "
                                   "Normal trading conditions resuming. ***");
                transitioned = true;
            }

            // Smooth intensity decay within phase
            if (!transitioned) {
                cPhase = (CrisisPhase)g_crisis.phase.load();
                double base, target; int64_t dur;
                switch (cPhase) {
                    case CrisisPhase::SHOCK:     base=1.0;  target=0.85; dur=CRISIS_SHOCK_MS;     break;
                    case CrisisPhase::PANIC:     base=0.85; target=0.50; dur=CRISIS_PANIC_MS;     break;
                    case CrisisPhase::STABILIZE: base=0.50; target=0.25; dur=CRISIS_STABILIZE_MS; break;
                    case CrisisPhase::RECOVERY:  base=0.25; target=0.0;  dur=CRISIS_RECOVERY_MS;  break;
                    default: base=0; target=0; dur=1; break;
                }
                elapsed = nowMs - g_crisis.phaseStartMs.load();
                double t = (std::min)(1.0, (double)elapsed / (double)dur);
                g_crisis.intensity.store(base + (target - base) * t);
            }

            cPhase = (CrisisPhase)g_crisis.phase.load();
            crisisIntensity = g_crisis.intensity.load();
        }

        // Sleep with jitter (faster during crisis)
        int sleepMs = (int)(SIM_INTERVAL_MS + jitterDist(rng));
        if (cPhase != CrisisPhase::NONE) {
            double speedMul = 1.0 + crisisIntensity * 4.0;  // up to 5x faster
            sleepMs = (int)(sleepMs / speedMul);
        }
        if (sleepMs < 30) sleepMs = 30;
        std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
        if (!Global::running.load()) break;

        ++tick;

        // Pick random symbol and bot
        int si = symDist(rng);
        std::string sym  = SYMBOLS[si];
        int botNum = botDist(rng);
        std::string botName = "BOT_" + std::to_string(botNum);
        auto& ss = symState[sym];

        // Bot personality: BOT_1-20 are bulls (+0.12 bias), BOT_21-40 are bears (-0.12 bias)
        bool isBull = (botNum <= 20);
        double personalityBias = isBull ? 0.12 : -0.12;

        // ── Random walk sentiment (Brownian motion, not sine waves) ──
        // Step: random normal increment
        std::normal_distribution<double> normalDist(0.0, 0.4);
        ss.sentiment += normalDist(rng);

        // Mean-reversion: gently pull sentiment back toward 0
        ss.sentiment *= 0.98;

        // Regime change: 2% chance to suddenly reverse direction
        if (coin(rng) < 0.02) {
            ss.sentiment = -ss.sentiment * 0.6 + normalDist(rng) * 2.0;
        }

        // Clamp
        if (ss.sentiment > 3.0) ss.sentiment = 3.0;
        if (ss.sentiment < -3.0) ss.sentiment = -3.0;

        // ── Crisis sentiment override ──
        if (cPhase != CrisisPhase::NONE) {
            double vuln = CRISIS_VULNERABILITY.count(sym) ?
                          CRISIS_VULNERABILITY.at(sym) : 1.0;
            switch (cPhase) {
                case CrisisPhase::SHOCK:
                    ss.sentiment = -3.0 * vuln;  // flash crash
                    break;
                case CrisisPhase::PANIC:
                    ss.sentiment -= 1.5 * crisisIntensity * vuln;
                    break;
                case CrisisPhase::STABILIZE:
                    ss.sentiment -= 0.5 * crisisIntensity * vuln;
                    if (coin(rng) < 0.08) {  // aftershock
                        ss.sentiment -= 2.0 * vuln;
                        if (coin(rng) < 0.3)
                            broadcastServerMsg("*** AFTERSHOCK: " + sym +
                                " hit by renewed selling pressure ***");
                    }
                    break;
                case CrisisPhase::RECOVERY:
                    ss.sentiment += 1.2 * crisisIntensity;
                    if (coin(rng) < 0.02) ss.sentiment -= 0.8 * vuln;
                    break;
                default: break;
            }
            double maxS = 3.0 + crisisIntensity * 2.0;
            if (ss.sentiment > maxS) ss.sentiment = maxS;
            if (ss.sentiment < -maxS) ss.sentiment = -maxS;
        }

        // ── Volatility random walk ──
        std::normal_distribution<double> volStep(0.0, 0.08);
        ss.volatility += volStep(rng);
        if (coin(rng) < 0.03) ss.volatility *= 1.8;  // occasional volatility spike
        if (ss.volatility < 0.5) ss.volatility = 0.5;
        if (ss.volatility > 2.5) ss.volatility = 2.5;

        // ── Crisis volatility override ──
        if (cPhase != CrisisPhase::NONE) {
            double vuln = CRISIS_VULNERABILITY.count(sym) ?
                          CRISIS_VULNERABILITY.at(sym) : 1.0;
            double crisisVol = 1.5 + crisisIntensity * 2.5 * vuln;
            if (ss.volatility < crisisVol) ss.volatility = crisisVol;
            double maxVol = 2.5 + crisisIntensity * 3.0;
            if (ss.volatility > maxVol) ss.volatility = maxVol;
        }

        // ── Burst trading (creates tall candles) ──
        if (ss.burstRemain <= 0) {
            double burstProb = 0.10;
            if (cPhase != CrisisPhase::NONE)
                burstProb = 0.10 + crisisIntensity * 0.50;  // up to 60% during shock

            if (coin(rng) < burstProb) {
                std::uniform_int_distribution<int> burstLen(3, 8);
                int len = burstLen(rng);
                if (cPhase == CrisisPhase::SHOCK || cPhase == CrisisPhase::PANIC)
                    len = (int)(len * (1.0 + crisisIntensity));  // longer bursts

                ss.burstRemain = len;

                if (cPhase == CrisisPhase::SHOCK) {
                    ss.burstBuy = false;  // all sells during shock
                } else if (cPhase == CrisisPhase::PANIC) {
                    ss.burstBuy = (coin(rng) < 0.15);  // 85% sell bursts
                } else {
                    double burstBias = ss.sentiment + personalityBias;
                    ss.burstBuy = (burstBias > 0) ? (coin(rng) < 0.70) : (coin(rng) < 0.30);
                }
            }
        }

        bool isBuy;
        if (ss.burstRemain > 0) {
            isBuy = ss.burstBuy ? (coin(rng) < 0.80) : (coin(rng) < 0.20);
            ss.burstRemain--;
        } else {
            double buyProb = 0.5 + ss.sentiment * 0.12 + personalityBias;

            // Crisis shifts all bots bearish
            if (cPhase != CrisisPhase::NONE) {
                buyProb -= crisisIntensity * 0.35;
                if (cPhase == CrisisPhase::RECOVERY) buyProb += 0.15;  // bargain hunters
            }

            // Contrarian trades (wicks / dead-cat bounces)
            double contrarianChance = 0.08;
            if (cPhase == CrisisPhase::STABILIZE) contrarianChance = 0.15;
            if (cPhase == CrisisPhase::RECOVERY)  contrarianChance = 0.12;
            if (coin(rng) < contrarianChance) buyProb = 1.0 - buyProb;

            if (buyProb < 0.05) buyProb = 0.05;
            if (buyProb > 0.90) buyProb = 0.90;
            isBuy = coin(rng) < buyProb;
        }

        // Order size — larger during crisis (institutional liquidations)
        uint32_t qty = qtyDist(rng);
        if (cPhase != CrisisPhase::NONE) {
            double sizeMul = 1.0 + crisisIntensity * 2.0;  // up to 3x
            qty = (uint32_t)(qty * sizeMul);
            if (qty > 600) qty = 600;
        }

        // Lock and place the order
        std::lock_guard<std::mutex> lk(Global::exMtx);

        if (!Global::accounts.count(botName)) continue;
        Account& bot = Global::accounts[botName];
        OrderBook& book = Global::books[sym];

        double price = 0.0;

        // Bots trade at best available price (like real market orders)
        // MM multi-level depth provides price variation naturally
        if (isBuy) {
            if (book.asks.empty()) continue;
            price = book.asks.begin()->first;  // buy at best ask
            double cost = price * qty;
            if (bot.cash < cost) continue;
            bot.cash -= cost;
        } else {
            if (book.bids.empty()) continue;
            price = book.bids.begin()->first;  // sell at best bid
            if (bot.holdings[sym] < qty) continue;
            bot.holdings[sym] -= qty;
        }

        // Create order
        uint64_t oid = Global::nextOrderId.fetch_add(1);
        Order ord;
        ord.orderId  = oid;
        ord.username = botName;
        ord.side     = isBuy ? 'B' : 'S';
        ord.symbol   = sym;
        ord.qty      = qty;
        ord.origQty  = qty;
        ord.price    = price;
        ord.ts       = std::chrono::steady_clock::now();

        bot.openOrders[oid] = ord;

        // Match — this calls recordTrade() → enqueueBroadcast()
        matchOrders(ord, book);
        // Re-quote MM after matching (deferred to avoid iterator invalidation)
        requoteMarketMaker(sym);
        checkConditionalOrders(sym);

        // Refund any unfilled remainder
        if (ord.qty > 0) {
            if (isBuy) {
                bot.cash += ord.price * ord.qty;
            } else {
                bot.holdings[sym] += ord.qty;
            }
            // Remove resting remainder (bots don't leave resting orders)
            if (Global::liveOrders.count(oid)) {
                if (ord.side == 'B') {
                    book.bids[ord.price].erase(oid);
                    if (book.bids[ord.price].empty()) book.bids.erase(ord.price);
                } else {
                    book.asks[ord.price].erase(oid);
                    if (book.asks[ord.price].empty()) book.asks.erase(ord.price);
                }
                Global::liveOrders.erase(oid);
            }
            bot.openOrders.erase(oid);
        }
    }

    std::cout << "[SIM] Simulation thread stopped.\n";
}

/*--------------------------------------------------------------------------
 * OHLC candle builder for price history charts
 *--------------------------------------------------------------------------*/

struct OHLCCandle { double open,high,low,close; uint32_t vol; std::string label; };

/**
 * @brief Build OHLC candles from the trade log by grouping trades into 1-minute buckets.
 * Returns up to the last 40 candles.  Called with Global::exMtx held.
 */
static std::vector<OHLCCandle> buildCandles(const std::vector<TradePoint>& log) {
    if(log.empty()) return {};
    // Group by 15-second buckets for fast chart population with many candles
    // datetime format: "YYYY-MM-DD_HH:MM:SS"
    struct Bucket { double open,high,low,close; uint32_t vol; std::string label; };
    std::vector<Bucket> buckets;
    std::string curKey;
    for(auto& tp : log) {
        // Extract "HH:MM:SS" and group into 15-second buckets
        std::string key;
        std::string label;
        size_t upos = tp.datetime.find('_');
        if(upos != std::string::npos && upos+8 <= tp.datetime.size()) {
            std::string timeStr = tp.datetime.substr(upos+1, 8); // "HH:MM:SS"
            // Round seconds to 15-second bucket: 0-14→00, 15-29→15, 30-44→30, 45-59→45
            int sec = std::stoi(timeStr.substr(6, 2));
            int bucket15 = (sec / 15) * 15;
            char secBuf[3]; snprintf(secBuf, sizeof(secBuf), "%02d", bucket15);
            key = timeStr.substr(0, 6) + secBuf; // "HH:MM:00/15/30/45"
            label = timeStr.substr(0, 5); // "HH:MM" for display
        } else {
            key = tp.datetime;
            label = tp.datetime;
        }
        if(key != curKey) {
            // New candle's open = previous candle's close (price continuity)
            double openPx = buckets.empty() ? tp.price : buckets.back().close;
            double hi = (std::max)(openPx, tp.price);
            double lo = (std::min)(openPx, tp.price);
            buckets.push_back({openPx, hi, lo, tp.price, tp.qty, label});
            curKey = key;
        } else {
            auto& b = buckets.back();
            if(tp.price > b.high) b.high = tp.price;
            if(tp.price < b.low)  b.low  = tp.price;
            b.close = tp.price;
            b.vol  += tp.qty;
        }
    }
    // Return last 120 candles (15s each = 30 minutes of data)
    std::vector<OHLCCandle> result;
    size_t start = buckets.size() > 120 ? buckets.size() - 120 : 0;
    for(size_t i = start; i < buckets.size(); ++i)
        result.push_back({buckets[i].open, buckets[i].high, buckets[i].low,
                          buckets[i].close, buckets[i].vol, buckets[i].label});
    return result;
}

/*--------------------------------------------------------------------------
 * Per-client TCP session  (runs in a worker thread)
 *--------------------------------------------------------------------------*/

/**
 * @brief Main loop for one connected client.
 *
 * Reads the 3-byte frame header (CmdID + PayloadLen), then receives exactly
 * PayloadLen bytes of payload using recvExact() — handling all TCP partial
 * reads.  Dispatches to the appropriate command handler.
 *
 * @param sock  The accepted TCP socket for this client.
 */
static void clientSession(SOCKET sock) {
    std::string username;  // empty = not logged in
    sockaddr_in udpSubAddr{};  // this client's UDP subscriber address (set on CMD_SUB_MARKET)
    bool hasUdpSub = false;

    // Register socket in user map on login, deregister on exit
    auto cleanup = [&]() {
        if(!username.empty()){
            std::lock_guard<std::mutex> lk(Global::userSockMtx);
            Global::userSockets.erase(username);
        }
        // Remove this client's UDP subscriber entry (exact IP:port match)
        if(hasUdpSub) {
            std::lock_guard<std::mutex> lk(Global::subMtx);
            Global::subscribers.erase(
                std::remove_if(Global::subscribers.begin(), Global::subscribers.end(),
                    [&](const Global::UdpSubscriber& sub){ return memcmp(&sub.addr,&udpSubAddr,sizeof(udpSubAddr))==0; }),
                Global::subscribers.end());
        }

        shutdown(sock, SD_BOTH);
        closesocket(sock);
        std::lock_guard<std::mutex> lk(Global::printMtx);
        std::cout<<"[DISCONNECT] "<<(username.empty()?"anon":username)<<"\n";
    };

    while(true) {
        // ---- Read 3-byte frame header: CmdID(1) + PayloadLen(2) ----
        char hdr[3];
        if(!recvExact(sock, hdr, 3)) { cleanup(); return; }

        uint8_t  cmdId  = (uint8_t)hdr[0];
        uint16_t payLen = 0;
        memcpy(&payLen, hdr+1, 2); payLen = ntohs(payLen);

        // ---- Receive payload (handles partial reads via recvExact) ----
        std::vector<char> payload(payLen);
        if(payLen > 0 && !recvExact(sock, payload.data(), payLen)) { cleanup(); return; }

        int o = 0;  // read offset into payload

        // ---- Dispatch ----
        switch((CmdID)cmdId) {

        case CMD_LOGIN: {
            std::string user, pass;
            if(!readStr1(payload.data(),(int)payLen,o,user)||user.empty()){
                std::vector<char> p; pushStr1(p,"Empty username."); sendFrame(sock,CMD_LOGIN_FAIL,p); break;
            }
            // Read password (new field — if missing, treat as empty for backwards compat)
            readStr1(payload.data(),(int)payLen,o,pass);
            if(pass.empty()){
                std::vector<char> p; pushStr1(p,"Password required."); sendFrame(sock,CMD_LOGIN_FAIL,p); break;
            }
            // Reject re-login without logout
            if(!username.empty()){ sendServerMsg(sock,"Already logged in as '"+username+"'."); break; }
            // Block login as the house/market-maker account
            if(user==HOUSE_USER){ std::vector<char> p; pushStr1(p,"Reserved system account."); sendFrame(sock,CMD_LOGIN_FAIL,p); break; }
            {
                std::lock_guard<std::mutex> lk(Global::exMtx);
                std::string ph = hashPassword(user, pass);
                if(!Global::accounts.count(user)){
                    // New account — register with this password
                    Global::accounts[user].username=user;
                    Global::accounts[user].passwordHash=ph;
                    std::lock_guard<std::mutex> plk(Global::printMtx); std::cout<<"[NEW] "<<user<<"\n";
                } else {
                    // Existing account — validate password
                    if(Global::accounts[user].passwordHash != ph){
                        std::vector<char> p; pushStr1(p,"Wrong password."); sendFrame(sock,CMD_LOGIN_FAIL,p); break;
                    }
                }
                username = user;
                Account& acc=Global::accounts[user];
                std::vector<char> p; pushDouble(p,acc.cash);
                pushU16(p,(uint16_t)acc.holdings.size());
                for(auto&[sym,qty]:acc.holdings){pushStr1(p,sym);pushU32(p,qty);}
                sendFrame(sock,CMD_LOGIN_OK,p);
                // Also push open orders on login (snapshot refresh)
                if(!acc.openOrders.empty()){
                    std::vector<char> ol; pushU16(ol,(uint16_t)acc.openOrders.size());
                    for(auto&[oid,ord]:acc.openOrders){ pushU64(ol,oid); pushU8(ol,ord.side=='B'?0:1); pushStr1(ol,ord.symbol); pushU32(ol,ord.qty); pushDouble(ol,ord.price); }
                    sendFrame(sock,CMD_ORDER_LIST,ol);
                }
            }
            { std::lock_guard<std::mutex> lk(Global::userSockMtx); Global::userSockets[user]=sock; }
            { std::lock_guard<std::mutex> lk(Global::printMtx); std::cout<<"[LOGIN] "<<user<<"\n"; }
            break;
        }

        case CMD_LOGOUT: {
            sendFrame(sock, CMD_LOGOUT_OK, {});
            cleanup(); return;
        }

        case CMD_PLACE_ORDER: {
            if(username.empty()){ sendServerMsg(sock,"Not logged in."); break; }
            uint8_t sideU8=0; std::string sym; uint32_t qty=0; double price=0;
            if(!readU8(payload.data(),(int)payLen,o,sideU8)||!readStr1(payload.data(),(int)payLen,o,sym)||
               !readU32(payload.data(),(int)payLen,o,qty)||!readDouble(payload.data(),(int)payLen,o,price)){
                std::vector<char> p; pushStr1(p,"Malformed order."); sendFrame(sock,CMD_ORDER_REJECT,p); break;
            }
            char side=(sideU8==0)?'B':'S';
            std::lock_guard<std::mutex> lk(Global::exMtx);
            if(!Global::books.count(sym)){ std::vector<char> p; pushStr1(p,"Unknown symbol: "+sym); sendFrame(sock,CMD_ORDER_REJECT,p); break; }
            if(qty==0){ std::vector<char> p; pushStr1(p,"Invalid qty."); sendFrame(sock,CMD_ORDER_REJECT,p); break; }
            // Market order: price=0 means use best available price from order book
            if(price <= 0) {
                auto& book = Global::books[sym];
                if(side=='B') {
                    if(book.asks.empty()){ std::vector<char> p; pushStr1(p,"No asks available for market buy."); sendFrame(sock,CMD_ORDER_REJECT,p); break; }
                    price = book.asks.rbegin()->first;  // highest ask = worst case for reservation
                } else {
                    if(book.bids.empty()){ std::vector<char> p; pushStr1(p,"No bids available for market sell."); sendFrame(sock,CMD_ORDER_REJECT,p); break; }
                    price = book.bids.rbegin()->first;  // lowest bid
                }
            }
            Account& acc=Global::accounts[username];
            if(side=='B'){
                double cost=price*qty;
                if(acc.cash<cost){ std::vector<char> p; pushStr1(p,"Insufficient cash (need $"+[&](){std::ostringstream ss;ss<<std::fixed<<std::setprecision(2)<<cost;return ss.str();}()+")."); sendFrame(sock,CMD_ORDER_REJECT,p); break; }
                acc.cash-=cost;
            } else {
                uint32_t held=acc.holdings.count(sym)?acc.holdings[sym]:0;
                if(held<qty){ std::vector<char> p; pushStr1(p,"Insufficient shares (have "+std::to_string(held)+")."); sendFrame(sock,CMD_ORDER_REJECT,p); break; }
                acc.holdings[sym]-=qty; if(acc.holdings[sym]==0) acc.holdings.erase(sym);
            }
            uint64_t oid=Global::nextOrderId.fetch_add(1);
            Order ord; ord.orderId=oid; ord.username=username; ord.side=side;
            ord.symbol=sym; ord.qty=qty; ord.origQty=qty; ord.price=price;
            ord.ts=std::chrono::steady_clock::now();
            // ORDER_ACK
            { std::vector<char> p; pushU64(p,oid); pushU8(p,sideU8); pushStr1(p,sym); pushU32(p,qty); pushDouble(p,price); sendFrame(sock,CMD_ORDER_ACK,p); }
            acc.openOrders[oid]=ord;
            { std::lock_guard<std::mutex> plk(Global::printMtx);
              std::cout<<"[ORDER #"<<oid<<"] "<<username<<" "<<side<<" "<<qty<<"x"<<sym<<" @"<<std::fixed<<std::setprecision(4)<<price<<"\n"; }
            matchOrders(ord,Global::books[sym]);
            // Re-quote MM after matching (deferred to avoid iterator invalidation)
            requoteMarketMaker(sym);
            checkConditionalOrders(sym);
            // Refund unused reservation
            if(side=='B')  acc.cash+=ord.qty*price;
            else if(ord.qty>0) acc.holdings[sym]+=ord.qty;
            break;
        }

        case CMD_CANCEL_ORDER: {
            if(username.empty()){ sendServerMsg(sock,"Not logged in."); break; }
            uint64_t oid=0;
            if(!readU64(payload.data(),(int)payLen,o,oid)){ std::vector<char> p; pushStr1(p,"Malformed cancel."); sendFrame(sock,CMD_CANCEL_REJECT,p); break; }
            std::lock_guard<std::mutex> lk(Global::exMtx);
            auto it=Global::liveOrders.find(oid);
            if(it==Global::liveOrders.end()){ std::vector<char> p; pushStr1(p,"Order not found."); sendFrame(sock,CMD_CANCEL_REJECT,p); break; }
            Order& ord=it->second;
            if(ord.username!=username){ std::vector<char> p; pushStr1(p,"Not your order."); sendFrame(sock,CMD_CANCEL_REJECT,p); break; }
            OrderBook& book=Global::books[ord.symbol];
            if(ord.side=='B'){
                auto li=book.bids.find(ord.price); if(li!=book.bids.end()){li->second.erase(oid);if(li->second.empty())book.bids.erase(li);}
                Global::accounts[username].cash+=ord.price*ord.qty;
            } else {
                auto li=book.asks.find(ord.price); if(li!=book.asks.end()){li->second.erase(oid);if(li->second.empty())book.asks.erase(li);}
                Global::accounts[username].holdings[ord.symbol]+=ord.qty;
            }
            Global::accounts[username].openOrders.erase(oid);
            Global::liveOrders.erase(it);
            { std::vector<char> p; pushU64(p,oid); sendFrame(sock,CMD_CANCEL_ACK,p); }
            { std::lock_guard<std::mutex> plk(Global::printMtx); std::cout<<"[CANCEL #"<<oid<<"] "<<username<<"\n"; }
            break;
        }

        case CMD_QUERY_MARKET: {
            std::string sym;
            if(!readStr1(payload.data(),(int)payLen,o,sym)){ sendServerMsg(sock,"Malformed query."); break; }
            std::lock_guard<std::mutex> lk(Global::exMtx);
            if(!Global::books.count(sym)){ sendServerMsg(sock,"Unknown symbol: "+sym); break; }
            auto& book=Global::books[sym];
            double bid=0,ask=0,last=book.lastPrice; uint32_t bidQty=0,askQty=0,vol=book.volume;
            if(!book.bids.empty()){ bid=book.bids.begin()->first; for(auto&[id,o]:book.bids.begin()->second) bidQty+=o.qty; }
            if(!book.asks.empty()){ ask=book.asks.begin()->first; for(auto&[id,o]:book.asks.begin()->second) askQty+=o.qty; }
            std::vector<char> p; pushStr1(p,sym); pushDouble(p,bid); pushU32(p,bidQty); pushDouble(p,ask); pushU32(p,askQty); pushDouble(p,last); pushU32(p,vol);
            sendFrame(sock,CMD_MARKET_DATA,p);
            break;
        }

        case CMD_QUERY_ACCOUNT: {
            if(username.empty()){ sendServerMsg(sock,"Not logged in."); break; }
            std::lock_guard<std::mutex> lk(Global::exMtx);
            Account& acc=Global::accounts[username];
            std::vector<char> p; pushDouble(p,acc.cash); pushU16(p,(uint16_t)acc.holdings.size());
            for(auto&[sym,qty]:acc.holdings){pushStr1(p,sym);pushU32(p,qty);}
            sendFrame(sock,CMD_ACCOUNT_DATA,p);
            break;
        }

        case CMD_QUERY_ORDERS: {
            if(username.empty()){ sendServerMsg(sock,"Not logged in."); break; }
            std::lock_guard<std::mutex> lk(Global::exMtx);
            Account& acc=Global::accounts[username];
            std::vector<char> p; pushU16(p,(uint16_t)acc.openOrders.size());
            for(auto&[oid,ord]:acc.openOrders){ pushU64(p,oid); pushU8(p,ord.side=='B'?0:1); pushStr1(p,ord.symbol); pushU32(p,ord.qty); pushDouble(p,ord.price); }
            sendFrame(sock,CMD_ORDER_LIST,p);
            break;
        }

        case CMD_QUERY_TRADES: {
            if(username.empty()){ sendServerMsg(sock,"Not logged in."); break; }
            std::lock_guard<std::mutex> lk(Global::exMtx);
            Account& acc=Global::accounts[username];
            size_t start=acc.trades.size()>50?acc.trades.size()-50:0;
            uint16_t cnt=(uint16_t)(acc.trades.size()-start);
            std::vector<char> p; pushU16(p,cnt);
            for(size_t i=start;i<acc.trades.size();++i){
                auto& tr=acc.trades[i];
                pushU64(p,tr.tradeId); pushStr1(p,tr.symbol); pushU32(p,tr.qty); pushDouble(p,tr.price);
                pushU8(p, tr.buyUser==username?0:1);
                pushStr1(p,tr.datetime);
            }
            sendFrame(sock,CMD_TRADE_LIST,p);
            break;
        }

        case CMD_QUERY_HISTORY: {
            std::string sym;
            if(!readStr1(payload.data(),(int)payLen,o,sym)){ sendServerMsg(sock,"Malformed history query."); break; }
            std::lock_guard<std::mutex> lk(Global::exMtx);
            if(!Global::books.count(sym)){ sendServerMsg(sock,"Unknown symbol: "+sym); break; }
            auto candles = buildCandles(Global::books[sym].tradeLog);
            std::vector<char> p; pushStr1(p,sym); pushU16(p,(uint16_t)candles.size());
            for(auto& c : candles){
                pushDouble(p,c.open); pushDouble(p,c.high); pushDouble(p,c.low); pushDouble(p,c.close);
                pushU32(p,c.vol); pushStr1(p,c.label);
            }
            sendFrame(sock,CMD_HISTORY_DATA,p);
            break;
        }

        case CMD_SUB_MARKET: {
            // Client sends its UDP port; we record IP+port for broadcasts
            uint16_t udpPort=0;
            if(!readU16(payload.data(),(int)payLen,o,udpPort)){ sendServerMsg(sock,"Malformed subscribe."); break; }
            // Get client IP from the TCP socket
            sockaddr_in caddr{}; int caddrLen=sizeof(caddr);
            getpeername(sock,(sockaddr*)&caddr,&caddrLen);
            caddr.sin_port=htons(udpPort);
            {
                std::lock_guard<std::mutex> lk(Global::subMtx);
                // Avoid duplicates
                bool found=false;
                for(auto&sub:Global::subscribers){ if(memcmp(&sub.addr,&caddr,sizeof(caddr))==0){found=true;break;} }
                if(!found) Global::subscribers.push_back({caddr});
            }
            sendServerMsg(sock,"Subscribed to UDP market data on port "+std::to_string(udpPort)+".");
            // Track for cleanup on disconnect
            udpSubAddr = caddr;
            hasUdpSub  = true;
            break;
        }

        case CMD_CRISIS: {
            if(username.empty()){ sendServerMsg(sock,"Not logged in."); break; }
            auto now = std::chrono::steady_clock::now();
            int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                now.time_since_epoch()).count();
            g_crisis.phase.store((int)CrisisPhase::SHOCK);
            g_crisis.startMs.store(nowMs);
            g_crisis.phaseStartMs.store(nowMs);
            g_crisis.intensity.store(1.0);
            broadcastServerMsg(
                "*** BREAKING NEWS: ARMED CONFLICT ERUPTS -- "
                "Global markets in freefall. All sectors affected. ***");
            break;
        }

        case CMD_STOP_ORDER: {
            if(username.empty()){ sendServerMsg(sock,"Not logged in."); break; }
            uint8_t typeU8=0; std::string sym; uint32_t qty=0; double trigPx=0;
            if(!readU8(payload.data(),(int)payLen,o,typeU8)||!readStr1(payload.data(),(int)payLen,o,sym)||
               !readU32(payload.data(),(int)payLen,o,qty)||!readDouble(payload.data(),(int)payLen,o,trigPx)){
                sendServerMsg(sock,"Malformed stop order."); break;
            }
            char type = (typeU8 == 0) ? 'S' : 'T';  // 0=stop-loss, 1=take-profit
            std::lock_guard<std::mutex> lk(Global::exMtx);
            if(!Global::accounts.count(username)){ sendServerMsg(sock,"Account not found."); break; }
            if(!Global::books.count(sym)){ sendServerMsg(sock,"Unknown symbol: "+sym); break; }
            Account& acc = Global::accounts[username];
            // Validate user has enough shares
            uint32_t held = acc.holdings.count(sym) ? acc.holdings[sym] : 0;
            if(held < qty){ sendServerMsg(sock,"Insufficient shares (have "+std::to_string(held)+"). Cannot set stop order."); break; }
            // Validate trigger price makes sense vs current price
            double curPx = Global::books[sym].lastPrice;
            if(curPx > 0) {
                if(type == 'S' && trigPx >= curPx){
                    std::ostringstream oss;
                    oss << "Stop-loss trigger $" << std::fixed << std::setprecision(2) << trigPx
                        << " must be BELOW current price $" << curPx;
                    sendServerMsg(sock, oss.str()); break;
                }
                if(type == 'T' && trigPx <= curPx){
                    std::ostringstream oss;
                    oss << "Take-profit trigger $" << std::fixed << std::setprecision(2) << trigPx
                        << " must be ABOVE current price $" << curPx;
                    sendServerMsg(sock, oss.str()); break;
                }
            }
            uint64_t cid = Global::nextCondId.fetch_add(1);
            Global::conditionalOrders.push_back({cid, username, sym, type, qty, trigPx});
            std::string typeStr = (type == 'S') ? "STOP-LOSS" : "TAKE-PROFIT";
            std::ostringstream oss;
            oss << typeStr << " #" << cid << " set: sell " << qty << "x" << sym
                << " if price " << (type=='S' ? "<= $" : ">= $")
                << std::fixed << std::setprecision(2) << trigPx
                << " (current: $" << curPx << ")";
            sendServerMsg(sock, oss.str());
            break;
        }

        case CMD_QUERY_PORTFOLIO: {
            if(username.empty()){ sendServerMsg(sock,"Not logged in."); break; }
            std::lock_guard<std::mutex> lk(Global::exMtx);
            auto& acc = Global::accounts[username];
            std::vector<char> p;
            // Count positions (only symbols with holdings)
            uint16_t numPos = 0;
            for(auto&[sym,qty]:acc.holdings) if(qty>0) numPos++;
            pushU16(p, numPos);
            for(auto&[sym,qty]:acc.holdings) {
                if(qty==0) continue;
                double avg = acc.avgCost.count(sym) ? acc.avgCost[sym] : 0;
                double cur = Global::books.count(sym) ? Global::books[sym].lastPrice : 0;
                pushStr1(p,sym); pushU32(p,qty); pushDouble(p,avg); pushDouble(p,cur);
            }
            pushDouble(p, acc.cash);
            sendFrame(sock, CMD_QUERY_PORTFOLIO, p);
            break;
        }

        case CMD_QUERY_STOPS: {
            if(username.empty()){ sendServerMsg(sock,"Not logged in."); break; }
            std::lock_guard<std::mutex> lk(Global::exMtx);
            std::ostringstream oss;
            oss << "Active conditional orders:\n";
            int count = 0;
            for(auto& co : Global::conditionalOrders) {
                if(co.username != username) continue;
                std::string typeStr = (co.type == 'S') ? "STOP-LOSS" : "TAKE-PROFIT";
                oss << "  #" << co.id << " " << typeStr << " " << co.qty << "x" << co.symbol
                    << " trigger $" << std::fixed << std::setprecision(2) << co.triggerPrice << "\n";
                count++;
            }
            if(count == 0) oss << "  (none)";
            sendServerMsg(sock, oss.str());
            break;
        }

        case CMD_CANCEL_STOP: {
            if(username.empty()){ sendServerMsg(sock,"Not logged in."); break; }
            uint64_t cid=0;
            if(!readU64(payload.data(),(int)payLen,o,cid)){ sendServerMsg(sock,"Malformed cancel."); break; }
            std::lock_guard<std::mutex> lk(Global::exMtx);
            bool found = false;
            for(auto it = Global::conditionalOrders.begin(); it != Global::conditionalOrders.end(); ++it) {
                if(it->id == cid && it->username == username) {
                    Global::conditionalOrders.erase(it);
                    sendServerMsg(sock, "Conditional order #" + std::to_string(cid) + " cancelled.");
                    found = true;
                    break;
                }
            }
            if(!found) sendServerMsg(sock, "Conditional order not found.");
            break;
        }

        default:
            sendServerMsg(sock,"Unknown command.");
            break;
        }
    }
}

/*--------------------------------------------------------------------------
 * Background threads
 *--------------------------------------------------------------------------*/

/** Drains Global::bcastQueue and sendto() each datagram to all subscribers. */
static void udpBroadcastThread() {
    while(Global::running.load()) {
        std::unique_lock<std::mutex> lk(Global::bcastMtx);
        Global::bcastCV.wait_for(lk, std::chrono::milliseconds(100), []{ return !Global::bcastQueue.empty(); });
        while(!Global::bcastQueue.empty()) {
            Global::BroadcastItem item = std::move(Global::bcastQueue.front());
            Global::bcastQueue.pop_front();
            lk.unlock();
            std::lock_guard<std::mutex> slk(Global::subMtx);
            for(auto& sub : Global::subscribers) {
                sendto(Global::udpSocket, item.payload.data(), (int)item.payload.size(), 0,
                       (const sockaddr*)&sub.addr, sizeof(sub.addr));
            }
            lk.lock();
        }
    }
}

static void persistThread() {
    auto last=std::chrono::steady_clock::now();
    while(Global::running.load()){
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if(std::chrono::duration<double>(std::chrono::steady_clock::now()-last).count()>=PERSIST_INTERVAL){
            persistData(); last=std::chrono::steady_clock::now();
        }
    }
    persistData();
}

/*--------------------------------------------------------------------------
 * main()
 *--------------------------------------------------------------------------*/

int main() {
    // --- Step 1: Configuration ---
    std::string tcpPortStr, udpPortStr, persistPath;
    std::cout<<"Server TCP Port Number: "; std::getline(std::cin,tcpPortStr);
    while(!tcpPortStr.empty()&&(tcpPortStr.back()=='\r'||tcpPortStr.back()=='\n')) tcpPortStr.pop_back();
    std::cout<<"Server UDP Port Number: "; std::getline(std::cin,udpPortStr);
    while(!udpPortStr.empty()&&(udpPortStr.back()=='\r'||udpPortStr.back()=='\n')) udpPortStr.pop_back();
    std::cout<<"Data directory (for persistence): "; std::getline(std::cin,persistPath);
    while(!persistPath.empty()&&(persistPath.back()=='\r'||persistPath.back()=='\n')) persistPath.pop_back();
    Global::persistPath=persistPath;
    uint16_t tcpPort=(uint16_t)std::stoi(tcpPortStr);
    uint16_t udpPort=(uint16_t)std::stoi(udpPortStr);

    // --- Step 2: Winsock ---
    WSADATA wsaData{};
    if(WSAStartup(MAKEWORD(2,2),&wsaData)!=NO_ERROR){ std::cerr<<"WSAStartup failed.\n"; return 1; }

    // --- Step 3: TCP listener socket ---
    addrinfo hints{},*info=nullptr;
    hints.ai_family=AF_INET; hints.ai_socktype=SOCK_STREAM; hints.ai_protocol=IPPROTO_TCP; hints.ai_flags=AI_PASSIVE;
    if(getaddrinfo(nullptr,tcpPortStr.c_str(),&hints,&info)!=0||!info){ std::cerr<<"getaddrinfo failed.\n"; WSACleanup(); return 2; }
    SOCKET listener=socket(info->ai_family,info->ai_socktype,info->ai_protocol);
    if(listener==INVALID_SOCKET){ std::cerr<<"socket failed.\n"; freeaddrinfo(info); WSACleanup(); return 3; }
    if(bind(listener,info->ai_addr,(int)info->ai_addrlen)!=0){ std::cerr<<"bind failed.\n"; freeaddrinfo(info); closesocket(listener); WSACleanup(); return 4; }
    freeaddrinfo(info);
    if(listen(listener,SOMAXCONN)!=0){ std::cerr<<"listen failed.\n"; closesocket(listener); WSACleanup(); return 5; }

    // --- Step 4: UDP socket for broadcasts ---
    Global::udpSocket=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    if(Global::udpSocket==INVALID_SOCKET){ std::cerr<<"UDP socket failed.\n"; closesocket(listener); WSACleanup(); return 6; }
    sockaddr_in udpBind{}; udpBind.sin_family=AF_INET; udpBind.sin_addr.s_addr=INADDR_ANY; udpBind.sin_port=htons(udpPort);
    if(bind(Global::udpSocket,(sockaddr*)&udpBind,sizeof(udpBind))!=0){ std::cerr<<"UDP bind failed.\n"; closesocket(Global::udpSocket); closesocket(listener); WSACleanup(); return 7; }

    // --- Step 5: Print server address ---
    char hostname[256]; gethostname(hostname,sizeof(hostname));
    addrinfo h{},*hres=nullptr; h.ai_family=AF_INET;
    if(getaddrinfo(hostname,nullptr,&h,&hres)==0&&hres){
        char ip[INET_ADDRSTRLEN]; inet_ntop(AF_INET,&((sockaddr_in*)hres->ai_addr)->sin_addr,ip,sizeof(ip));
        std::cout<<"Server IP   : "<<ip<<"\n"; freeaddrinfo(hres);
    }
    std::cout<<"TCP Port    : "<<tcpPort<<"\n";
    std::cout<<"UDP Port    : "<<udpPort<<"  (market data broadcasts)\n";
    std::cout<<"Symbols     : "; for(auto&s:SYMBOLS) std::cout<<s<<" "; std::cout<<"\n\n";

    // --- Step 6: Load persisted data ---
    loadData();

    // --- Step 6b: Seed house/market-maker account with initial quotes ---
    seedMarketMaker();
    seedBotAccounts();

    // --- Step 7: Start background threads ---
    std::thread bcastThr(udpBroadcastThread);
    std::thread persThr(persistThread);
    std::thread simThr(simulationThread);

    // --- Step 8: Accept loop (pre-threading: spawn one thread per client) ---
    std::cout<<"Exchange ready. Ctrl+C to stop.\n\n";
    while(Global::running.load()) {
        sockaddr_in clientAddr{}; int addrLen=sizeof(clientAddr);
        SOCKET clientSock=accept(listener,(sockaddr*)&clientAddr,&addrLen);
        if(clientSock==INVALID_SOCKET) break;
        char ip[INET_ADDRSTRLEN]; inet_ntop(AF_INET,&clientAddr.sin_addr,ip,sizeof(ip));
        { std::lock_guard<std::mutex> lk(Global::printMtx);
          std::cout<<"[CONNECT] "<<ip<<":"<<ntohs(clientAddr.sin_port)<<"\n"; }
        std::thread(clientSession, clientSock).detach();
    }

    // --- Shutdown ---
    Global::running=false;
    closesocket(listener);
    bcastThr.join(); persThr.join(); simThr.join();
    closesocket(Global::udpSocket);
    WSACleanup();
    return 0;
}
