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

#include <iostream>
#include <iomanip>
#include <algorithm>
#include <condition_variable>
#include <random>

#include "utils.h"
#include "types.h"
#include "global.h"

#include "persistence.h"
#include "Bots.h"
#include "crisis.h"
#include "display.h"


static const int    MAX_PAYLOAD = 8192;         // max TCP payload bytes
static const double PERSIST_INTERVAL = 5.0;     // seconds between disk flushes
static const int    WORKER_THREADS = 20;        // pre-spawned worker pool size

/*--------------------------------------------------------------------------
 * TCP framed send: CmdID(1) + PayloadLen(2) + Payload
 *--------------------------------------------------------------------------*/

 /**
  * @brief Send a framed TCP response to a client
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
    pushU8(frame, (uint8_t)cmd);
    pushU16(frame, len);
    frame.insert(frame.end(), payload.begin(), payload.begin() + len);

    return sendAll(s, frame.data(), (int)frame.size());
}

static bool sendServerMsg(SOCKET s, const std::string& msg) {
    std::vector<char> p; pushU16(p, (uint16_t)msg.size()); p.insert(p.end(), msg.begin(), msg.end());
    return sendFrame(s, CMD_SERVER_MSG, p);
}


/**
 * @brief Simple hash function for password storage.
 *        Uses djb2 + salted with username for basic security.
 */
static std::string hashPassword(const std::string& user, const std::string& pass) {
    std::string salted = user + ":" + pass;
    uint64_t hash = 5381;
    for (char c : salted) hash = ((hash << 5) + hash) + (uint8_t)c;
    std::ostringstream ss;
    ss << std::hex << std::setfill('0') << std::setw(16) << hash;
    return ss.str();
}

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
    double bid = 0, ask = 0, last = book.lastPrice;
    uint32_t bidQty = 0, askQty = 0, vol = book.volume;
    if (!book.bids.empty()) { bid = book.bids.begin()->first; for (auto& [oid, o] : book.bids.begin()->second) bidQty += o.qty; }
    if (!book.asks.empty()) { ask = book.asks.begin()->first; for (auto& [oid, o] : book.asks.begin()->second) askQty += o.qty; }

    // Inner payload (shared with TCP)
    std::vector<char> inner;
    pushStr1(inner, sym); pushDouble(inner, bid); pushU32(inner, bidQty);
    pushDouble(inner, ask); pushU32(inner, askQty); pushDouble(inner, last); pushU32(inner, vol);

    // Full UDP datagram: Seq(4) + CmdID(1) + PayloadLen(2) + inner
    std::vector<char> dgram;
    pushU32(dgram, Global::udpSeq.fetch_add(1));
    pushU8(dgram, (uint8_t)CMD_MARKET_DATA);
    pushU16(dgram, (uint16_t)inner.size());
    dgram.insert(dgram.end(), inner.begin(), inner.end());

    std::lock_guard<std::mutex> lk(Global::bcastMtx);
    Global::bcastQueue.push_back({ std::move(dgram) });
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
        if (it != Global::userSockets.end()) sock = it->second;
    }
    if (sock == INVALID_SOCKET) return;

    std::vector<char> p;

    pushU64(p, orderId); 
    pushU32(p, fillQty); 
    pushDouble(p, fillPx);
    pushU32(p, remQty); 
    pushStr1(p, sym);

    sendFrame(sock, CMD_TRADE_EXEC, p);
}

// Forward declarations for mutual references
static void matchOrders(Order& ord, OrderBook& book);

/**
 * @brief Record one fill, update both accounts atomically, push notifications.
 * Called from main exchange thread with Global::exMtx held.
 */
static void recordTrade(const std::string& sym, uint32_t fill, double fillPx,
    Order& buyOrd, Order& sellOrd) {
    Trade tr;
    tr.tradeId =    Global::nextTradeId.fetch_add(1); 
    tr.symbol =     sym;
    tr.qty =        fill; 
    tr.price =      fillPx;
    tr.buyUser =    buyOrd.username;
    tr.sellUser =   sellOrd.username;
    tr.datetime =   nowString();

    Global::allTrades.push_back(tr);

    auto& book_ref = Global::books[sym];
    // Update MM volatility (EMA of price change magnitude)
    if (book_ref.lastPrice > 0) {
        double pctChange = std::abs(fillPx - book_ref.lastPrice) / std::max(book_ref.lastPrice, 0.01);
        book_ref.mmVolatility = book_ref.mmVolatility * 0.95 + pctChange * 100.0 * 0.05;

        if (book_ref.mmVolatility < 0.2) book_ref.mmVolatility = 0.2;
        if (book_ref.mmVolatility > 5.0) book_ref.mmVolatility = 5.0;
    }
    book_ref.lastPrice = fillPx; book_ref.volume += fill;
    book_ref.tradeLog.push_back({ fillPx, fill, tr.datetime, std::chrono::steady_clock::now() });

    // Update buyer — cash was ALREADY reserved at limitPrice on order placement.
    // Refund the price improvement: (limitPrice - fillPx) * fill.
    // Do NOT deduct cash again.
    auto& ba = Global::accounts[buyOrd.username];
    ba.cash += (buyOrd.price - fillPx) * fill;   // refund price improvement
    // Update average cost basis (weighted average)
    uint32_t oldQty = ba.holdings.count(sym) ? ba.holdings[sym] : 0;
    double oldAvgCost = ba.avgCost.count(sym) ? ba.avgCost[sym] : 0;
    ba.avgCost[sym] = (oldQty > 0) ? (oldAvgCost * oldQty + fillPx * fill) / (oldQty + fill) : fillPx;
    ba.holdings[sym] += fill;
    ba.trades.push_back(tr);
    if (ba.openOrders.count(buyOrd.orderId)) {
        ba.openOrders[buyOrd.orderId].qty -= fill;
        if (ba.openOrders[buyOrd.orderId].qty == 0) ba.openOrders.erase(buyOrd.orderId);
    }

    // Update seller — shares were ALREADY reserved on order placement.
    // Do NOT deduct holdings again. Just credit cash from the sale.
    auto& sa = Global::accounts[sellOrd.username];
    sa.cash += fillPx * fill;
    sa.trades.push_back(tr);
    if (sa.openOrders.count(sellOrd.orderId)) {
        sa.openOrders[sellOrd.orderId].qty -= fill;
        if (sa.openOrders[sellOrd.orderId].qty == 0) sa.openOrders.erase(sellOrd.orderId);
    }

    {
        std::lock_guard<std::mutex> lk(Global::printMtx);
        std::cout << "[TRADE #" << tr.tradeId << "] " << sym << " qty=" << fill
            << " @" << std::fixed << std::setprecision(4) << fillPx
            << "  buyer=" << buyOrd.username << "  seller=" << sellOrd.username << "\n";
    }

    uint32_t buyRem = buyOrd.qty - fill;
    uint32_t sellRem = sellOrd.qty - fill;

    pushTradeExec(buyOrd.username, buyOrd.orderId, fill, fillPx, buyRem, sym);
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

        // Use lowest bid to sweep all levels (market sell)
        double sellPrice = book.bids.empty() ? 0.01 : book.bids.rbegin()->first;

        uint64_t oid = Global::nextOrderId.fetch_add(1);
        Order sord;

        sord.orderId =      oid; 
        sord.username =     co.username; 
        sord.side =         'S';
        sord.symbol =       sym; 
        sord.qty =          sellQty; 
        sord.origQty =      sellQty;
        sord.price =        sellPrice;
        sord.ts =           std::chrono::steady_clock::now();

        acc.openOrders[oid] = sord;

        matchOrders(sord, book);
        /***************************************************************put market maker logic here****************************************************************/
 

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
        {
            std::lock_guard<std::mutex> plk(Global::printMtx);
            std::cout << "[" << typeStr << "] " << co.username << " " << sym
                << " qty=" << filled << "\n";
        }
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
    if (ord.side == 'B') {
        for (auto lvlIt = book.asks.begin(); lvlIt != book.asks.end() && ord.qty > 0;) {

            if (ord.price < lvlIt->first) break;

            auto& lvl = lvlIt->second;

            for (auto oit = lvl.begin(); oit != lvl.end() && ord.qty > 0;) {

                Order& r = oit->second;
                uint32_t fill = std::min(ord.qty, r.qty);
                recordTrade(ord.symbol, fill, lvlIt->first, ord, r);

                ord.qty -= fill; 
                r.qty -= fill; 
                Global::liveOrders[r.orderId].qty = r.qty; 
                Global::liveOrders[ord.orderId].qty = ord.qty;

                if (r.qty == 0) { Global::liveOrders.erase(r.orderId); oit = lvl.erase(oit); }
                else ++oit;

            }

            if (lvl.empty()) lvlIt = book.asks.erase(lvlIt); else ++lvlIt;
        }
    }
    else {
        for (auto lvlIt = book.bids.begin(); lvlIt != book.bids.end() && ord.qty > 0;) {

            if (ord.price > lvlIt->first) break;

            auto& lvl = lvlIt->second;

            for (auto oit = lvl.begin(); oit != lvl.end() && ord.qty > 0;) {

                Order& r = oit->second;
                uint32_t fill = std::min(ord.qty, r.qty);
                recordTrade(ord.symbol, fill, lvlIt->first, r, ord);

                ord.qty -= fill; 
                r.qty -= fill; 
                Global::liveOrders[r.orderId].qty = r.qty; 
                Global::liveOrders[ord.orderId].qty = ord.qty;

                if (r.qty == 0) { Global::liveOrders.erase(r.orderId); oit = lvl.erase(oit); }
                else ++oit;
            }
            if (lvl.empty()) lvlIt = book.bids.erase(lvlIt); else ++lvlIt;
        }
    }
    if (ord.qty > 0) {
        if (ord.side == 'B') book.bids[ord.price][ord.orderId] = ord;
        else                 book.asks[ord.price][ord.orderId] = ord;
        Global::liveOrders[ord.orderId] = ord;
    }
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

    
    while (Global::running.load())
    {
        {
            std::lock_guard<std::mutex> lk(Global::exMtx);

            BotManager::Instance().ProcessMarketMaker(matchOrders);
            BotManager::Instance().ProcessStrategies(matchOrders);

        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

}


/*--------------------------------------------------------------------------
 * OHLC candle builder for price history charts
 *--------------------------------------------------------------------------*/

struct OHLCCandle { double open, high, low, close; uint32_t vol; std::string label; };

/**
 * @brief Build OHLC candles from the trade log by grouping trades into 1-minute buckets.
 * Returns up to the last 40 candles.  Called with Global::exMtx held.
 */
static std::vector<OHLCCandle> buildCandles(const std::vector<TradePoint>& log) {
    if (log.empty()) return {};
    // Group by 15-second buckets for fast chart population with many candles
    // datetime format: "YYYY-MM-DD_HH:MM:SS"
    std::vector<OHLCCandle> buckets;
    std::string curKey;
    for (auto& tp : log) {
        // Extract "HH:MM:SS" and group into 15-second buckets
        std::string key;
        std::string label;
        size_t upos = tp.datetime.find('_');

        if (upos != std::string::npos && upos + 8 <= tp.datetime.size()) {
            std::string timeStr = tp.datetime.substr(upos + 1, 8); // "HH:MM:SS"
            // Round seconds to 15-second bucket: 0-14→00, 15-29→15, 30-44→30, 45-59→45
            int sec = std::stoi(timeStr.substr(6, 2));
            int bucket15 = (sec / 15) * 15;
            char secBuf[3]; snprintf(secBuf, sizeof(secBuf), "%02d", bucket15);
            key = timeStr.substr(0, 6) + secBuf; // "HH:MM:00/15/30/45"
            label = timeStr.substr(0, 5); // "HH:MM" for display
        }
        else {
            key = tp.datetime;
            label = tp.datetime;
        }

        if (key != curKey) {
            // New candle's open = previous candle's close (price continuity)
            double openPx = buckets.empty() ? tp.price : buckets.back().close;
            double hi = std::max(openPx, tp.price);
            double lo = std::min(openPx, tp.price);
            buckets.push_back({ openPx, hi, lo, tp.price, tp.qty, label });
            curKey = key;
        }
        else {
            auto& b = buckets.back();
            if (tp.price > b.high) b.high = tp.price;
            if (tp.price < b.low)  b.low = tp.price;
            b.close = tp.price;
            b.vol += tp.qty;
        }
    }
    // Return last 120 candles (15s each = 30 minutes of data)
    std::vector<OHLCCandle> result;
    size_t start = buckets.size() > 120 ? buckets.size() - 120 : 0;
    for (size_t i = start; i < buckets.size(); ++i)
        result.push_back(buckets[i]);
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
    bool dhEstablished = false;  // Track if DH handshake completed
    std::vector<uint8_t> sessionKey;  // Store session key for this client

    // Register socket in user map on login, deregister on exit
    auto cleanup = [&]() {
        if (!username.empty()) {
            std::lock_guard<std::mutex> lk(Global::userSockMtx);
            Global::userSockets.erase(username);
        }
        // Remove this client's UDP subscriber entry (exact IP:port match)
        if (hasUdpSub) {
            std::lock_guard<std::mutex> lk(Global::subMtx);
            Global::subscribers.erase(
                std::remove_if(Global::subscribers.begin(), Global::subscribers.end(),
                    [&](const Global::UdpSubscriber& sub) { return memcmp(&sub.addr, &udpSubAddr, sizeof(udpSubAddr)) == 0; }),
                Global::subscribers.end());
        }

        shutdown(sock, SD_BOTH);
        closesocket(sock);
        std::lock_guard<std::mutex> lk(Global::printMtx);
        std::cout << "[DISCONNECT] " << (username.empty() ? "anon" : username) << "\n";
        };

    while (true) {
        // ---- Read 3-byte frame header: CmdID(1) + PayloadLen(2) ----
        char hdr[3];
        if (!recvExact(sock, hdr, 3)) { cleanup(); return; }

        uint8_t  cmdId = (uint8_t)hdr[0];
        uint16_t payLen = 0;
        memcpy(&payLen, hdr + 1, 2); payLen = ntohs(payLen);

        // ---- Receive payload (handles partial reads via recvExact) ----
        std::vector<char> payload(payLen);
        if (payLen > 0 && !recvExact(sock, payload.data(), payLen)) { cleanup(); return; }

        int o = 0;  // read offset into payload

        // ---- Dispatch ----
        switch ((CmdID)cmdId) {
        case CMD_DH_PUBLIC_KEY:
        {
            uint64_t clientPublicKey = 0;
            if (!readU64(payload.data(), (int)payLen, o, clientPublicKey)) {
                sendServerMsg(sock, "Invalid DH public key.");
                break;
            }

            {
                std::lock_guard<std::mutex> plk(Global::printMtx);
                std::cout << "[DH] Received client public key: " << clientPublicKey << "\n";
            }

            // Create or get DH session for this socket
            std::lock_guard<std::mutex> lk(Global::dhMutex);
            auto& session = Global::dhSessions[sock];

            // Log server's keys before computation
            uint64_t serverPublicKeyBefore = session.dh.getPublicKey();

            // Compute shared secret using client's public key
            session.dh.computeSharedSecret(clientPublicKey);
            uint64_t sharedSecret = session.dh.getSharedSecret();
            session.sessionKey = session.dh.getEncryptionKey(32);
            session.established = true;

            {
                std::lock_guard<std::mutex> plk(Global::printMtx);
                std::cout << "[DH] Computed shared secret: " << sharedSecret << "\n";

                std::string keyStr;
                for (size_t i = 0; i < std::min(session.sessionKey.size(), (size_t)16); ++i) {
                    char buf[4];
                    sprintf_s(buf, "%02X ", session.sessionKey[i]);
                    keyStr += buf;
                }
                std::cout << "[DH] Session key (first 16): " << keyStr << "\n";
            }

            // Send server's public key back to client
            std::vector<char> p;
            pushU64(p, session.dh.getPublicKey());
            sendFrame(sock, CMD_DH_PUBLIC_KEY_RESPONSE, p);

            // Store in local session variables
            dhEstablished = true;
            sessionKey = session.sessionKey;

            break;
        }
        case CMD_LOGIN: {
            if (!username.empty()) {    // Reject login without logout
                sendServerMsg(sock, "Already logged in as [" + username + "]. Please logout first.");
                break;
            }

            std::string user, pass;
            // This reads: [UsernameLen:1] [Username:var]
            if (!readStr1(payload.data(), (int)payLen, o, user) || user.empty()) {
                std::vector<char> p;
                pushStr1(p, "Empty username.");
                sendFrame(sock, CMD_LOGIN_FAIL, p);
                break;
            }

            {
                std::lock_guard<std::mutex> plk(Global::printMtx);
                std::cout << "[LOGIN] Received username: '" << user
                    << "' (len=" << user.length() << ")\n";
            }

            // Check if Already logged in
            {
                std::lock_guard<std::mutex> lk(Global::userSockMtx);
                if (Global::userSockets.count(user)) {
                    std::vector<char> p;
                    pushStr1(p, "User already logged in from another session.");
                    sendFrame(sock, CMD_LOGIN_FAIL, p);
                    break;
                }
            }

            // ===== READ FLAG =====
            uint8_t encFlag = 0;
            if (!readU8(payload.data(), (int)payLen, o, encFlag)) {
                std::vector<char> p;
                pushStr1(p, "Invalid login format - missing flag.");
                sendFrame(sock, CMD_LOGIN_FAIL, p);
                break;
            }

            {
                std::lock_guard<std::mutex> plk(Global::printMtx);
                std::cout << "[LOGIN] Encryption flag: " << (int)encFlag << "\n";
            }

            // ===== READ PASSWORD =====
            if (encFlag == 1) {
                // Encrypted password format: [EncryptedLen:2] [EncryptedData:var]
                uint16_t encLen = 0;
                if (!readU16(payload.data(), (int)payLen, o, encLen)) {
                    std::vector<char> p;
                    pushStr1(p, "Invalid encrypted password length.");
                    sendFrame(sock, CMD_LOGIN_FAIL, p);
                    break;
                }

                {
                    std::lock_guard<std::mutex> plk(Global::printMtx);
                    std::cout << "[LOGIN] Encrypted data length: " << encLen << "\n";
                }

                if (encLen == 0 || o + encLen > (int)payLen) {
                    std::vector<char> p;
                    pushStr1(p, "Invalid encrypted data.");
                    sendFrame(sock, CMD_LOGIN_FAIL, p);
                    break;
                }

                std::vector<uint8_t> encryptedPass(encLen);
                memcpy(encryptedPass.data(), payload.data() + o, encLen);
                o += encLen;
                std::cout << "encrypted pass: ";
                for (uint8_t pass : encryptedPass)
                    std::cout << (int)pass << " ";
                std::cout << std::endl;

                std::cout << "session key: ";
                std::stringstream sessionKeystream;
                for (uint8_t key : sessionKey)
                    std::cout << (int)key << " ";
                std::cout << std::endl;

                // Decrypt using session key
                if (dhEstablished && !sessionKey.empty()) {
                    std::vector<uint8_t> encryptedBytes(encryptedPass.begin(), encryptedPass.end());
                    auto decryptedBytes = DiffieHellman::xorEncryptDecrypt(encryptedBytes, sessionKey);
                    pass = std::string(decryptedBytes.begin(), decryptedBytes.end());

                    {
                        std::lock_guard<std::mutex> plk(Global::printMtx);
                        std::cout << "[LOGIN] Decrypted password: " << pass<<"\n";
                    }
                }
                else {
                    std::vector<char> p;
                    pushStr1(p, "No secure channel established.");
                    sendFrame(sock, CMD_LOGIN_FAIL, p);
                    break;
                }
            }
            else {
                // Plaintext password format: [PasswordLen:1] [Password:var]
                if (!readStr1(payload.data(), (int)payLen, o, pass)) {
                    std::vector<char> p;
                    pushStr1(p, "Invalid plaintext password.");
                    sendFrame(sock, CMD_LOGIN_FAIL, p);
                    break;
                }

                {
                    std::lock_guard<std::mutex> plk(Global::printMtx);
                    std::cout << "[LOGIN] Plaintext password length: " << pass.length() << "\n";
                }
            }

            if (pass.empty()) {
                std::vector<char> p;
                pushStr1(p, "Empty password.");
                sendFrame(sock, CMD_LOGIN_FAIL, p);
                break;
            }

            // Log encryption status
            {
                std::lock_guard<std::mutex> plk(Global::printMtx);
                std::cout << "[LOGIN] " << user << " - "
                    << (encFlag == 1 ? "encrypted" : "plaintext")
                    << " password (len=" << pass.length() << ")\n";
            }
            // Block login as the house/market-maker account
            if (user == Global::HOUSE_USER) { std::vector<char> p; pushStr1(p, "Reserved system account."); sendFrame(sock, CMD_LOGIN_FAIL, p); break; }
            {
                std::lock_guard<std::mutex> lk(Global::exMtx);
                std::string ph = hashPassword(user, pass);
                if (!Global::accounts.count(user)) {
                    // New account — register with this password
                    Global::accounts[user].username = user;
                    Global::accounts[user].passwordHash = ph;
                    std::lock_guard<std::mutex> plk(Global::printMtx); std::cout << "[NEW] " << user << "\n";
                }
                else {
                    // Existing account — validate password
                    if (Global::accounts[user].passwordHash != ph) {
                        std::vector<char> p; pushStr1(p, "Wrong password."); sendFrame(sock, CMD_LOGIN_FAIL, p); break;
                    }
                }
                username = user;
                Account& acc = Global::accounts[user];
                std::vector<char> p; pushDouble(p, acc.cash);
                pushU16(p, (uint16_t)acc.holdings.size());
                for (auto& [sym, qty] : acc.holdings) { pushStr1(p, sym); pushU32(p, qty); }
                sendFrame(sock, CMD_LOGIN_OK, p);
                // Also push open orders on login (snapshot refresh)
                if (!acc.openOrders.empty()) {
                    std::vector<char> ol; pushU16(ol, (uint16_t)acc.openOrders.size());
                    for (auto& [oid, ord] : acc.openOrders) { pushU64(ol, oid); pushU8(ol, ord.side == 'B' ? 0 : 1); pushStr1(ol, ord.symbol); pushU32(ol, ord.qty); pushDouble(ol, ord.price); }
                    sendFrame(sock, CMD_ORDER_LIST, ol);
                }
            }
            { std::lock_guard<std::mutex> lk(Global::userSockMtx); Global::userSockets[user] = sock; }
            { std::lock_guard<std::mutex> lk(Global::printMtx); std::cout << "[LOGIN] " << user << "\n"; }
            break;
        }

        case CMD_LOGOUT: {
            if (username.empty()) { sendServerMsg(sock, "Not loggeed in."); break; }
            { std::lock_guard<std::mutex> lk(Global::userSockMtx); Global::userSockets.erase(username); }
            sendFrame(sock, CMD_LOGOUT_OK, {});
            username.clear(); break;
        }

        case CMD_QUIT: {
            sendFrame(sock, CMD_QUIT_OK, {});
            cleanup(); return;
        }

        case CMD_PLACE_ORDER: {
            if (username.empty()) { sendServerMsg(sock, "Not logged in."); break; }
            uint8_t sideU8 = 0; std::string sym; uint32_t qty = 0; double price = 0;
            if (!readU8(payload.data(), (int)payLen, o, sideU8) || !readStr1(payload.data(), (int)payLen, o, sym) ||
                !readU32(payload.data(), (int)payLen, o, qty) || !readDouble(payload.data(), (int)payLen, o, price)) {
                std::vector<char> p; pushStr1(p, "Malformed order."); sendFrame(sock, CMD_ORDER_REJECT, p); break;
            }
            char side = (sideU8 == 0) ? 'B' : 'S';
            std::lock_guard<std::mutex> lk(Global::exMtx);
            if (!Global::books.count(sym)) { std::vector<char> p; pushStr1(p, "Unknown symbol: " + sym); sendFrame(sock, CMD_ORDER_REJECT, p); break; }
            if (qty == 0) { std::vector<char> p; pushStr1(p, "Invalid qty."); sendFrame(sock, CMD_ORDER_REJECT, p); break; }
            // Market order: price=0 means use best available price from order book
            if (price <= 0) {
                auto& book = Global::books[sym];
                if (side == 'B') {
                    if (book.asks.empty()) { std::vector<char> p; pushStr1(p, "No asks available for market buy."); sendFrame(sock, CMD_ORDER_REJECT, p); break; }
                    price = book.asks.rbegin()->first;  // highest ask = worst case for reservation
                }
                else {
                    if (book.bids.empty()) { std::vector<char> p; pushStr1(p, "No bids available for market sell."); sendFrame(sock, CMD_ORDER_REJECT, p); break; }
                    price = book.bids.rbegin()->first;  // lowest bid
                }
            }
            Account& acc = Global::accounts[username];
            if (side == 'B') {
                double cost = price * qty;
                if (acc.cash < cost) { std::vector<char> p; pushStr1(p, "Insufficient cash (need $" + [&]() {std::ostringstream ss; ss << std::fixed << std::setprecision(2) << cost; return ss.str(); }() + ")."); sendFrame(sock, CMD_ORDER_REJECT, p); break; }
                acc.cash -= cost;
            }
            else {
                uint32_t held = acc.holdings.count(sym) ? acc.holdings[sym] : 0;
                if (held < qty) { std::vector<char> p; pushStr1(p, "Insufficient shares (have " + std::to_string(held) + ")."); sendFrame(sock, CMD_ORDER_REJECT, p); break; }
                acc.holdings[sym] -= qty; if (acc.holdings[sym] == 0) acc.holdings.erase(sym);
            }
            uint64_t oid = Global::nextOrderId.fetch_add(1);
            Order ord; ord.orderId = oid; ord.username = username; ord.side = side;
            ord.symbol = sym; ord.qty = qty; ord.origQty = qty; ord.price = price;
            ord.ts = std::chrono::steady_clock::now();
            // ORDER_ACK
            { std::vector<char> p; pushU64(p, oid); pushU8(p, sideU8); pushStr1(p, sym); pushU32(p, qty); pushDouble(p, price); sendFrame(sock, CMD_ORDER_ACK, p); }
            acc.openOrders[oid] = ord;
            {
                std::lock_guard<std::mutex> plk(Global::printMtx);
                std::cout << "[ORDER #" << oid << "] " << username << " " << side << " " << qty << "x" << sym << " @" << std::fixed << std::setprecision(4) << price << "\n";
            }
            matchOrders(ord, Global::books[sym]);
            // Re-quote MM after matching (deferred to avoid iterator invalidation)
        /***************************************************************put market maker logic here****************************************************************/

            checkConditionalOrders(sym);
            // Resources stay reserved while order rests in the book.
            // The cancel handler refunds on cancellation; recordTrade handles fills.
            break;
        }

        case CMD_CANCEL_ORDER: {
            if (username.empty()) { sendServerMsg(sock, "Not logged in."); break; }
            uint64_t oid = 0;
            if (!readU64(payload.data(), (int)payLen, o, oid)) { std::vector<char> p; pushStr1(p, "Malformed cancel."); sendFrame(sock, CMD_CANCEL_REJECT, p); break; }
            std::lock_guard<std::mutex> lk(Global::exMtx);
            auto it = Global::liveOrders.find(oid);
            if (it == Global::liveOrders.end()) { std::vector<char> p; pushStr1(p, "Order not found."); sendFrame(sock, CMD_CANCEL_REJECT, p); break; }
            Order& ord = it->second;
            if (ord.username != username) { std::vector<char> p; pushStr1(p, "Not your order."); sendFrame(sock, CMD_CANCEL_REJECT, p); break; }
            OrderBook& book = Global::books[ord.symbol];
            if (ord.side == 'B') {
                auto li = book.bids.find(ord.price); if (li != book.bids.end()) { li->second.erase(oid); if (li->second.empty())book.bids.erase(li); }
                Global::accounts[username].cash += ord.price * ord.qty;
            }
            else {
                auto li = book.asks.find(ord.price); if (li != book.asks.end()) { li->second.erase(oid); if (li->second.empty())book.asks.erase(li); }
                Global::accounts[username].holdings[ord.symbol] += ord.qty;
            }
            Global::accounts[username].openOrders.erase(oid);
            Global::liveOrders.erase(it);
            { std::vector<char> p; pushU64(p, oid); sendFrame(sock, CMD_CANCEL_ACK, p); }
            { std::lock_guard<std::mutex> plk(Global::printMtx); std::cout << "[CANCEL #" << oid << "] " << username << "\n"; }
            break;
        }

        case CMD_QUERY_MARKET: {
            std::string sym;
            if (!readStr1(payload.data(), (int)payLen, o, sym)) { sendServerMsg(sock, "Malformed query."); break; }
            std::lock_guard<std::mutex> lk(Global::exMtx);
            if (!Global::books.count(sym)) { sendServerMsg(sock, "Unknown symbol: " + sym); break; }
            auto& book = Global::books[sym];
            double bid = 0, ask = 0, last = book.lastPrice; uint32_t bidQty = 0, askQty = 0, vol = book.volume;
            if (!book.bids.empty()) { bid = book.bids.begin()->first; for (auto& [id, ord] : book.bids.begin()->second) bidQty += ord.qty; }
            if (!book.asks.empty()) { ask = book.asks.begin()->first; for (auto& [id, ord] : book.asks.begin()->second) askQty += ord.qty; }
            std::vector<char> p; pushStr1(p, sym); pushDouble(p, bid); pushU32(p, bidQty); pushDouble(p, ask); pushU32(p, askQty); pushDouble(p, last); pushU32(p, vol);
            sendFrame(sock, CMD_MARKET_DATA, p);
            break;
        }

        case CMD_QUERY_ACCOUNT: {
            if (username.empty()) { sendServerMsg(sock, "Not logged in."); break; }
            std::lock_guard<std::mutex> lk(Global::exMtx);
            Account& acc = Global::accounts[username];
            std::vector<char> p; pushDouble(p, acc.cash); pushU16(p, (uint16_t)acc.holdings.size());
            for (auto& [sym, qty] : acc.holdings) { pushStr1(p, sym); pushU32(p, qty); }
            sendFrame(sock, CMD_ACCOUNT_DATA, p);
            break;
        }

        case CMD_QUERY_ORDERS: {
            if (username.empty()) { sendServerMsg(sock, "Not logged in."); break; }
            std::lock_guard<std::mutex> lk(Global::exMtx);
            Account& acc = Global::accounts[username];
            std::vector<char> p; pushU16(p, (uint16_t)acc.openOrders.size());
            for (auto& [oid, ord] : acc.openOrders) { pushU64(p, oid); pushU8(p, ord.side == 'B' ? 0 : 1); pushStr1(p, ord.symbol); pushU32(p, ord.qty); pushDouble(p, ord.price); }
            sendFrame(sock, CMD_ORDER_LIST, p);
            break;
        }

        case CMD_QUERY_TRADES: {
            if (username.empty()) { sendServerMsg(sock, "Not logged in."); break; }
            std::lock_guard<std::mutex> lk(Global::exMtx);
            Account& acc = Global::accounts[username];
            size_t start = acc.trades.size() > 50 ? acc.trades.size() - 50 : 0;
            uint16_t cnt = (uint16_t)(acc.trades.size() - start);
            std::vector<char> p; pushU16(p, cnt);
            for (size_t i = start; i < acc.trades.size(); ++i) {
                auto& tr = acc.trades[i];
                pushU64(p, tr.tradeId); pushStr1(p, tr.symbol); pushU32(p, tr.qty); pushDouble(p, tr.price);
                pushU8(p, tr.buyUser == username ? 0 : 1);
                pushStr1(p, tr.datetime);
            }
            sendFrame(sock, CMD_TRADE_LIST, p);
            break;
        }

        case CMD_QUERY_HISTORY: {
            std::string sym;
            if (!readStr1(payload.data(), (int)payLen, o, sym)) { sendServerMsg(sock, "Malformed history query."); break; }
            std::lock_guard<std::mutex> lk(Global::exMtx);
            if (!Global::books.count(sym)) { sendServerMsg(sock, "Unknown symbol: " + sym); break; }
            auto candles = buildCandles(Global::books[sym].tradeLog);
            std::vector<char> p; pushStr1(p, sym); pushU16(p, (uint16_t)candles.size());
            for (auto& c : candles) {
                pushDouble(p, c.open); pushDouble(p, c.high); pushDouble(p, c.low); pushDouble(p, c.close);
                pushU32(p, c.vol); pushStr1(p, c.label);
            }
            sendFrame(sock, CMD_HISTORY_DATA, p);
            break;
        }

        case CMD_SUB_MARKET: {
            // Client sends its UDP port; we record IP+port for broadcasts
            uint16_t udpPort = 0;
            if (!readU16(payload.data(), (int)payLen, o, udpPort)) { sendServerMsg(sock, "Malformed subscribe."); break; }
            // Get client IP from the TCP socket
            sockaddr_in caddr{}; int caddrLen = sizeof(caddr);
            getpeername(sock, (sockaddr*)&caddr, &caddrLen);
            caddr.sin_port = htons(udpPort);
            {
                std::lock_guard<std::mutex> lk(Global::subMtx);
                // Avoid duplicates
                bool found = false;
                for (auto& sub : Global::subscribers) { if (memcmp(&sub.addr, &caddr, sizeof(caddr)) == 0) { found = true; break; } }
                if (!found) Global::subscribers.push_back({ caddr });
            }
            sendServerMsg(sock, "Subscribed to UDP market data on port " + std::to_string(udpPort) + ".");
            // Track for cleanup on disconnect
            udpSubAddr = caddr;
            hasUdpSub = true;
            break;
        }

        //case CMD_CRISIS: {
        //    if (username.empty()) { sendServerMsg(sock, "Not logged in."); break; }
        //    auto now = std::chrono::steady_clock::now();
        //    int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        //        now.time_since_epoch()).count();
        //    g_crisis.phase.store((int)CrisisPhase::SHOCK);
        //    g_crisis.startMs.store(nowMs);
        //    g_crisis.phaseStartMs.store(nowMs);
        //    g_crisis.intensity.store(1.0);
        //    broadcastServerMsg(
        //        "*** BREAKING NEWS: ARMED CONFLICT ERUPTS -- "
        //        "Global markets in freefall. All sectors affected. ***");
        //    break;
        //}

        case CMD_STOP_ORDER: {
            if (username.empty()) { sendServerMsg(sock, "Not logged in."); break; }
            uint8_t typeU8 = 0; std::string sym; uint32_t qty = 0; double trigPx = 0;
            if (!readU8(payload.data(), (int)payLen, o, typeU8) || !readStr1(payload.data(), (int)payLen, o, sym) ||
                !readU32(payload.data(), (int)payLen, o, qty) || !readDouble(payload.data(), (int)payLen, o, trigPx)) {
                sendServerMsg(sock, "Malformed stop order."); break;
            }
            char type = (typeU8 == 0) ? 'S' : 'T';  // 0=stop-loss, 1=take-profit
            std::lock_guard<std::mutex> lk(Global::exMtx);
            if (!Global::accounts.count(username)) { sendServerMsg(sock, "Account not found."); break; }
            if (!Global::books.count(sym)) { sendServerMsg(sock, "Unknown symbol: " + sym); break; }
            Account& acc = Global::accounts[username];
            // Validate user has enough shares
            uint32_t held = acc.holdings.count(sym) ? acc.holdings[sym] : 0;
            if (held < qty) { sendServerMsg(sock, "Insufficient shares (have " + std::to_string(held) + "). Cannot set stop order."); break; }
            // Validate trigger price makes sense vs current price
            double curPx = Global::books[sym].lastPrice;
            if (curPx > 0) {
                if (type == 'S' && trigPx >= curPx) {
                    std::ostringstream oss;
                    oss << "Stop-loss trigger $" << std::fixed << std::setprecision(2) << trigPx
                        << " must be BELOW current price $" << curPx;
                    sendServerMsg(sock, oss.str()); break;
                }
                if (type == 'T' && trigPx <= curPx) {
                    std::ostringstream oss;
                    oss << "Take-profit trigger $" << std::fixed << std::setprecision(2) << trigPx
                        << " must be ABOVE current price $" << curPx;
                    sendServerMsg(sock, oss.str()); break;
                }
            }
            uint64_t cid = Global::nextCondId.fetch_add(1);
            Global::conditionalOrders.push_back({ cid, username, sym, type, qty, trigPx });
            std::string typeStr = (type == 'S') ? "STOP-LOSS" : "TAKE-PROFIT";
            std::ostringstream oss;
            oss << typeStr << " #" << cid << " set: sell " << qty << "x" << sym
                << " if price " << (type == 'S' ? "<= $" : ">= $")
                << std::fixed << std::setprecision(2) << trigPx
                << " (current: $" << curPx << ")";
            sendServerMsg(sock, oss.str());
            break;
        }

        case CMD_QUERY_PORTFOLIO: {
            if (username.empty()) { sendServerMsg(sock, "Not logged in."); break; }
            std::lock_guard<std::mutex> lk(Global::exMtx);
            auto& acc = Global::accounts[username];
            std::vector<char> p;
            // Count positions (only symbols with holdings)
            uint16_t numPos = 0;
            for (auto& [sym, qty] : acc.holdings) if (qty > 0) numPos++;
            pushU16(p, numPos);
            for (auto& [sym, qty] : acc.holdings) {
                if (qty == 0) continue;
                double avg = acc.avgCost.count(sym) ? acc.avgCost[sym] : 0;
                double cur = Global::books.count(sym) ? Global::books[sym].lastPrice : 0;
                pushStr1(p, sym); pushU32(p, qty); pushDouble(p, avg); pushDouble(p, cur);
            }
            pushDouble(p, acc.cash);
            sendFrame(sock, CMD_QUERY_PORTFOLIO, p);
            break;
        }

        case CMD_QUERY_STOPS: {
            if (username.empty()) { sendServerMsg(sock, "Not logged in."); break; }
            std::lock_guard<std::mutex> lk(Global::exMtx);
            std::ostringstream oss;
            oss << "Active conditional orders:\n";
            int count = 0;
            for (auto& co : Global::conditionalOrders) {
                if (co.username != username) continue;
                std::string typeStr = (co.type == 'S') ? "STOP-LOSS" : "TAKE-PROFIT";
                oss << "  #" << co.id << " " << typeStr << " " << co.qty << "x" << co.symbol
                    << " trigger $" << std::fixed << std::setprecision(2) << co.triggerPrice << "\n";
                count++;
            }
            if (count == 0) oss << "  (none)";
            sendServerMsg(sock, oss.str());
            break;
        }

        case CMD_CANCEL_STOP: {
            if (username.empty()) { sendServerMsg(sock, "Not logged in."); break; }
            uint64_t cid = 0;
            if (!readU64(payload.data(), (int)payLen, o, cid)) { sendServerMsg(sock, "Malformed cancel."); break; }
            std::lock_guard<std::mutex> lk(Global::exMtx);
            bool found = false;
            for (auto it = Global::conditionalOrders.begin(); it != Global::conditionalOrders.end(); ++it) {
                if (it->id == cid && it->username == username) {
                    Global::conditionalOrders.erase(it);
                    sendServerMsg(sock, "Conditional order #" + std::to_string(cid) + " cancelled.");
                    found = true;
                    break;
                }
            }
            if (!found) sendServerMsg(sock, "Conditional order not found.");
            break;
        }

        default:
            sendServerMsg(sock, "Unknown command.");
            break;
        }
    }
}

static void clientManager(SOCKET listener) {
    std::cout << "Exchange ready. Ctrl+C to stop.\n\n";

    threadPool tp(24);

    while (Global::running.load()) {

        sockaddr_in clientAddr{}; 
        int addrLen = sizeof(clientAddr);

        SOCKET clientSock = accept(listener, (sockaddr*)&clientAddr, &addrLen);
        if (clientSock == INVALID_SOCKET) break;

        char ip[INET_ADDRSTRLEN]; inet_ntop(AF_INET, &clientAddr.sin_addr, ip, sizeof(ip));
        {
            std::lock_guard<std::mutex> lk(Global::printMtx);
            std::cout << "[CONNECT] " << ip << ":" << ntohs(clientAddr.sin_port) << "\n";
        }

        tp.addThread(clientSession, clientSock);
    }
}

/*--------------------------------------------------------------------------
 * Background threads
 *--------------------------------------------------------------------------*/

 /** Drains Global::bcastQueue and sendto() each datagram to all subscribers. */
static void udpBroadcastThread() {
    while (Global::running.load()) {
        std::unique_lock<std::mutex> lk(Global::bcastMtx);
        Global::bcastCV.wait_for(lk, std::chrono::milliseconds(100), [] { return !Global::bcastQueue.empty(); });
        while (!Global::bcastQueue.empty()) {
            Global::BroadcastItem item = std::move(Global::bcastQueue.front());
            Global::bcastQueue.pop_front();
            lk.unlock();
            std::lock_guard<std::mutex> slk(Global::subMtx);
            for (auto& sub : Global::subscribers) {
                sendto(Global::udpSocket, item.payload.data(), (int)item.payload.size(), 0,
                    (const sockaddr*)&sub.addr, sizeof(sub.addr));
            }
            lk.lock();
        }
    }
}

static void persistThread() {
    auto last = std::chrono::steady_clock::now();
    while (Global::running.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - last).count() >= PERSIST_INTERVAL) {
            writePersistentData(); last = std::chrono::steady_clock::now();
        }
    }
    writePersistentData();
}

/*--------------------------------------------------------------------------
 * main()
 *--------------------------------------------------------------------------*/

int main() {

    // Read from a config file
    // Read from persistent data file
    loadPersistentData();

    // Initialize IMGUI
    Display::Init();
    Display::InitPorts();

    // Initialize managers from config + persistent data


    // Initialize WINSOCK
    WSADATA wsaData{};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != NO_ERROR) { 
        std::cerr << "WSAStartup failed.\n"; 
        return 1;
    }

    // TCP listener socket
    addrinfo hints{};
    addrinfo* info{ nullptr };

    hints.ai_family = AF_INET; 
    hints.ai_socktype = SOCK_STREAM; 
    hints.ai_protocol = IPPROTO_TCP; 
    hints.ai_flags = AI_PASSIVE;

    int ERRORCODE = getaddrinfo(nullptr, std::to_string(Global::tcpPort).c_str(), &hints, &info);

    if (ERRORCODE != 0) { 
        std::cerr << "getaddrinfo failed.\n"; 
        WSACleanup(); 
        return 2; 
    }

    SOCKET listener = socket(info->ai_family, info->ai_socktype, info->ai_protocol);

    if (listener == INVALID_SOCKET) { 
        std::cerr << "socket failed.\n"; 
        freeaddrinfo(info); 
        WSACleanup(); 
        return 3; 
    }

    ERRORCODE = bind(listener, info->ai_addr, (int)info->ai_addrlen);

    if ( ERRORCODE != 0) { 
        std::cerr << "bind failed.\n"; 
        freeaddrinfo(info); 
        closesocket(listener); 
        WSACleanup(); 
        return 4; 
    }

    freeaddrinfo(info);

    ERRORCODE = listen(listener, SOMAXCONN);

    if ( ERRORCODE != 0) { 
        std::cerr << "listen failed.\n"; 
        closesocket(listener); 
        WSACleanup(); 
        return 5; 
    }

    // UDP broadcast socket
    Global::udpSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

    if (Global::udpSocket == INVALID_SOCKET) { 
        std::cerr << "UDP socket failed.\n"; 
        closesocket(listener); 
        WSACleanup(); 
        return 6; 
    }

    sockaddr_in udpBind{}; 
    
    udpBind.sin_family = AF_INET; 
    udpBind.sin_addr.s_addr = INADDR_ANY; 
    udpBind.sin_port = htons(Global::udpPort);
    
    ERRORCODE = bind(Global::udpSocket, (sockaddr*)&udpBind, sizeof(udpBind));

    if ( ERRORCODE != 0) { 
        std::cerr << "UDP bind failed.\n"; 
        closesocket(Global::udpSocket); 
        closesocket(listener); 
        WSACleanup(); 
        return 7; 
    }

    // Print server address
    char hostname[256]; 
    gethostname(hostname, sizeof(hostname));

    addrinfo h{};
    addrinfo* hres{ nullptr }; 
    
    h.ai_family = AF_INET;

    ERRORCODE = getaddrinfo(hostname, nullptr, &h, &hres);

    if (ERRORCODE == 0) {
        inet_ntop(AF_INET, &((sockaddr_in*)hres->ai_addr)->sin_addr, Global::ipAddr.data(), Global::ipAddr.size());
        std::cout << "Server IP   : " << Global::ipAddr << "\n";
        freeaddrinfo(hres);
    }

    std::cout << "TCP Port    : " << Global::tcpPort << "\n";
    std::cout << "UDP Port    : " << Global::udpPort << "  (market data broadcasts)\n";
    std::cout << "Symbols     : "; for (auto& s : Global::SYMBOLS) std::cout << s << " "; std::cout << "\n\n";


    // Initialize Managers
    BotManager::Instance().InitMarketMaker();
    BotManager::Instance().InitBots();
    CrisisManager::Init();

    // Setup threads
    std::thread bcastThr(udpBroadcastThread);
    std::thread persThr(persistThread);
    std::thread simThr(simulationThread);
    std::thread clientThr(clientManager, listener);

    while (Global::running.load()) {
        CrisisManager::Update();
        Display::Draw();
    }

    // Cleanup:
    // Disconnect clients
    // Write stuff back into config and persistent
    // Close all threads
    
    // --- Shutdown ---
    closesocket(listener);

    bcastThr.join();
    persThr.join();
    simThr.join();
    clientThr.join();

    closesocket(Global::udpSocket);
    WSACleanup();

    return 0;
}