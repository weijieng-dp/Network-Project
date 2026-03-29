/* Start Header
*****************************************************************/
/*!
\file    client.cpp
\author  weixuan.toh@digipen.edu
\date    20 Mar 2026
\brief
  Trading client for CSD2161 Assignment 5 - Online Trading Platform (Option 3).

  Transport design:
    TCP  - All client<->server control messages: login, place order, cancel,
           query account/orders/trades, market query. recvExact() handles
           TCP partial reads correctly (same as Assignment 4).
    UDP  - Receive-only: listens for best-effort market-data BROADCAST
           datagrams from the server after every trade. Out-of-order
           datagrams are detected via the 4-byte sequence number and
           silently discarded; duplicates are ignored.

  -----------------------------------------------------------------------
  Architecture
  -----------------------------------------------------------------------
  Thread roles:
    T_Main   - Runs FTXUI ScreenInteractive (full-screen TUI).
    T_TcpRcv - Receives all TCP responses from the server (CMD_LOGIN_OK,
               CMD_TRADE_EXEC, CMD_ORDER_ACK, etc.) using recvExact().
    T_UdpRcv - Listens on a local UDP port for market-data broadcasts.
               Handles out-of-order (seqNo < lastSeqNo -> discard).

  -----------------------------------------------------------------------
  TCP message framing
  -----------------------------------------------------------------------
  Frame header: CmdID(1) + PayloadLen(2)
  T_TcpRcv reads the 3-byte header then calls recvExact(PayloadLen).

  See server.cpp for the full protocol table.

Copyright (C) 2026 DigiPen Institute of Technology.
Reproduction or disclosure of this file or its contents without the
prior written consent of DigiPen Institute of Technology is prohibited.
*/
/* End Header
*******************************************************************/

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
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
#include <sstream>
#include <iomanip>
#include "utils.h"

// FTXUI - Terminal UI library for interactive TUI
#ifdef DrawText
#undef DrawText
#endif
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/canvas.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/component/event.hpp>
#include <crypto.h>

static const int MAX_PAYLOAD = 8192;
static const std::vector<std::string> SYMBOLS = {"AAPL","GOOGL","MSFT","TSLA","AMZN"};

/*--------------------------------------------------------------------------
 * Send a framed TCP request: CmdID(1) + PayloadLen(2) + Payload
 *--------------------------------------------------------------------------*/

static std::mutex g_sendMtx;

/**
 * @brief Build and send a complete TCP request frame (thread-safe).
 */
static bool sendFrame(SOCKET s, CmdID cmd, const std::vector<char>& payload) {
    std::vector<char> frame;
    uint16_t len=(uint16_t)std::min(payload.size(),(size_t)MAX_PAYLOAD);
    pushU8(frame,(uint8_t)cmd); pushU16(frame,len);
    frame.insert(frame.end(),payload.begin(),payload.begin()+len);
    std::lock_guard<std::mutex> lk(g_sendMtx);
    return sendAll(s, frame.data(), (int)frame.size());
}

/*--------------------------------------------------------------------------
 * Global client state
 *--------------------------------------------------------------------------*/

static std::mutex        g_stateMtx;
static std::atomic<bool> g_running{true};

static SOCKET      g_tcpSocket  = INVALID_SOCKET;
static SOCKET      g_udpSocket  = INVALID_SOCKET;

// Local account cache
static bool                             g_loggedIn = false;
static std::string                      g_username;
static double                           g_cash     = 0.0;
static std::map<std::string,uint32_t>   g_holdings;

// Local order / trade cache
struct LocalOrder { 
    std::string sym{};
    uint64_t id{}; 
    double price{};
    uint32_t qty{};
    char side{};
};
struct LocalTrade { 
    std::string sym;
    std::string dt;
    uint64_t tradeId; 
    double price;
    uint32_t qty; 
    char side;  
};
static std::map<uint64_t,LocalOrder>  g_openOrders;
static std::vector<LocalTrade>        g_trades;

// UDP sequence tracking
static uint32_t g_udpLastSeq = 0;

// Candlestick chart data
struct Candle { 
    double open{}, high{}, low{}, close{}; 
    uint32_t vol{};
    std::string dt{};
};
static std::mutex               g_chartMtx;
static std::string              g_chartSymbol;
static std::vector<Candle>      g_chartCandles;
static int                      g_chartSymIdx = 0;
static int                      g_chartZoom   = 60;  // number of candles to display (scroll to zoom)

// Market data cache (updated from UDP broadcasts and TCP responses)
struct MarketSnapshot { double bid,ask,last; uint32_t bidQty,askQty,vol; };
static std::mutex g_mktMtx;
static std::map<std::string,MarketSnapshot> g_marketData;

// Log messages for the TUI log panel
static std::mutex g_logMtx;
static std::deque<std::string> g_logMessages;
static const size_t MAX_LOG_LINES = 200;

// Encryption
static DiffieHellman g_dh;
static std::vector<uint8_t> g_sessionKey;
static std::atomic<bool> g_dhEstablished{ false };
static std::mutex g_dhMutex;

// FTXUI screen reference for PostEvent from background threads
static ftxui::ScreenInteractive* g_screenPtr = nullptr;

/*--------------------------------------------------------------------------
 * TUI helpers
 *--------------------------------------------------------------------------*/
static std::string fmtMoney(double v) { 
    std::ostringstream s; 
    s << std::fixed << std::setprecision(2) << v;
    std::string str{ s.str() };

    size_t dotPos{ str.find('.') };

    int insertPos{ static_cast<int>(dotPos == std::string::npos ? str.length() - 3 : dotPos - 3) };

    while (insertPos > 0 && str[insertPos - 1] != '-') {
        str.insert(static_cast<size_t>(insertPos), ",");
        insertPos -= 3;
    }
    return "$" + str;
}
static std::string fmtPrice(double v) { std::ostringstream s; s<<std::fixed<<std::setprecision(4)<<v; return s.str(); }

/** Append a message to the TUI log panel and trigger a screen refresh. */
static void logMsg(const std::string& msg) {
    {
        std::lock_guard<std::mutex> lk(g_logMtx);
        g_logMessages.push_back(msg);
        while(g_logMessages.size()>MAX_LOG_LINES) g_logMessages.pop_front();
    }
    if(g_screenPtr) g_screenPtr->Post(ftxui::Event::Custom);
}

/** Trigger a TUI screen refresh (call after updating any shared state). */
static void refreshUI() {
    if(g_screenPtr) g_screenPtr->Post(ftxui::Event::Custom);
}

/** Request chart data for a symbol from the server. */
static void requestChart(const std::string& sym) {
    std::vector<char> p; pushStr1(p,sym);
    sendFrame(g_tcpSocket, CMD_QUERY_HISTORY, p);
}

/*--------------------------------------------------------------------------
 * TCP response handlers  (called from T_TcpRcv — update state + log)
 *--------------------------------------------------------------------------*/

static void onLoginOk(const char* b, int n, int o) {
    double cash=0; uint16_t nh=0;
    if(!readDouble(b,n,o,cash)||!readU16(b,n,o,nh)) return;
    std::lock_guard<std::mutex> lk(g_stateMtx);
    g_loggedIn=true; g_cash=cash; g_holdings.clear();
    for(uint16_t i=0;i<nh;++i){ std::string sym; uint32_t qty; if(!readStr1(b,n,o,sym)||!readU32(b,n,o,qty))break; if(qty>0)g_holdings[sym]=qty; }
    logMsg("LOGIN OK - Welcome, " + g_username + "! Cash: " + fmtMoney(cash));
    // Auto-request chart for current symbol
    requestChart(SYMBOLS[g_chartSymIdx]);
}

static void onLoginFail(const char* b, int n, int o) {
    std::string reason; readStr1(b,n,o,reason);
    { std::lock_guard<std::mutex> lk(g_stateMtx); g_loggedIn=false; g_username.clear(); }
    logMsg("LOGIN FAILED: " + reason);
}

static void onOrderAck(const char* b, int n, int o) {
    uint64_t oid=0; uint8_t sideU8=0; std::string sym; uint32_t qty=0; double price=0;
    if(!readU64(b,n,o,oid)||!readU8(b,n,o,sideU8)||!readStr1(b,n,o,sym)||!readU32(b,n,o,qty)||!readDouble(b,n,o,price)) return;
    char side=(sideU8==0)?'B':'S';
    { 
        std::lock_guard<std::mutex> lk(g_stateMtx); 
        g_openOrders[oid] = { sym, oid, price, qty, side };
    }
    logMsg("ORDER #" + std::to_string(oid) + " ACCEPTED: " +
           std::string(side=='B'?"BUY ":"SELL ") + std::to_string(qty) + "x" + sym + " @ " + fmtPrice(price));
    refreshUI();
}

static void onOrderReject(const char* b, int n, int o) {
    std::string reason; readStr1(b,n,o,reason);
    logMsg("ORDER REJECTED: " + reason);
}

static void onTradeExec(const char* b, int n, int o) {
    uint64_t oid=0; uint32_t fillQty=0,remQty=0; double price=0; std::string sym;
    if(!readU64(b,n,o,oid)||!readU32(b,n,o,fillQty)||!readDouble(b,n,o,price)||!readU32(b,n,o,remQty)||!readStr1(b,n,o,sym)) return;
    char side='?';
    {
        std::lock_guard<std::mutex> lk(g_stateMtx);
        if(g_openOrders.count(oid)){ side=g_openOrders[oid].side; g_openOrders[oid].qty=remQty; if(remQty==0) g_openOrders.erase(oid); }
        time_t t=time(nullptr); struct tm tm{}; localtime_s(&tm,&t); char buf[32]; strftime(buf,sizeof(buf),"%H:%M:%S",&tm);
        g_trades.push_back({ sym, buf, oid, price, fillQty, side});
    }
    logMsg("*** TRADE *** " + std::string(side=='B'?"BOUGHT ":"SOLD ") +
           std::to_string(fillQty) + "x" + sym + " @ " + fmtPrice(price) +
           (remQty>0 ? " (rem: "+std::to_string(remQty)+")" : " (filled)"));
    // Auto-refresh chart after trade
    requestChart(SYMBOLS[g_chartSymIdx]);
    // Also refresh account
    sendFrame(g_tcpSocket, CMD_QUERY_ACCOUNT, {});
}

static void onCancelAck(const char* b, int n, int o) {
    uint64_t oid=0; readU64(b,n,o,oid);
    { std::lock_guard<std::mutex> lk(g_stateMtx); g_openOrders.erase(oid); }
    logMsg("ORDER #" + std::to_string(oid) + " CANCELLED");
    sendFrame(g_tcpSocket, CMD_QUERY_ACCOUNT, {});
    refreshUI();
}

static void onCancelReject(const char* b, int n, int o) {
    std::string reason; readStr1(b,n,o,reason);
    logMsg("CANCEL REJECTED: " + reason);
}

/** Parse market data (shared by TCP response and UDP broadcast). */
static void parseMarketData(const char* b, int n, int o) {
    std::string sym; double bid=0,ask=0,last=0; uint32_t bidQty=0,askQty=0,vol=0;
    if(!readStr1(b,n,o,sym)||!readDouble(b,n,o,bid)||!readU32(b,n,o,bidQty)||
       !readDouble(b,n,o,ask)||!readU32(b,n,o,askQty)||!readDouble(b,n,o,last)||!readU32(b,n,o,vol)) return;
    {
        std::lock_guard<std::mutex> lk(g_mktMtx);
        g_marketData[sym] = {bid, ask, last, bidQty, askQty, vol};
    }
    refreshUI();
}

static void onAccountData(const char* b, int n, int o) {
    double cash=0; uint16_t nh=0;
    if(!readDouble(b,n,o,cash)||!readU16(b,n,o,nh)) return;
    { std::lock_guard<std::mutex> lk(g_stateMtx); g_cash=cash; g_holdings.clear();
      for(uint16_t i=0;i<nh;++i){ std::string sym; uint32_t qty; if(!readStr1(b,n,o,sym)||!readU32(b,n,o,qty))break; if(qty>0)g_holdings[sym]=qty; } }
    refreshUI();
}

static void onServerMsg(const char* b, int n, int o) {
    uint16_t mlen=0; if(!readU16(b,n,o,mlen)||o+mlen>n) return;
    std::string msg(b+o,mlen);
    logMsg("SERVER: " + msg);
}

static std::atomic<bool> g_expectDisconnect{false};  // set before server closes socket

static void onLogoutOk() {
    g_expectDisconnect = true;  // server will close socket after LOGOUT_OK — don't treat as crash
    { std::lock_guard<std::mutex> lk(g_stateMtx); g_loggedIn=false; g_username.clear(); g_cash=0; g_holdings.clear(); g_openOrders.clear(); }
    logMsg("LOGGED OUT - You can /login again or /q to quit.");
    refreshUI();
}

static void onOrderList(const char* b, int n, int o) {
    uint16_t cnt=0; if(!readU16(b,n,o,cnt)) return;
    { 
        std::lock_guard<std::mutex> lk(g_stateMtx); g_openOrders.clear();
        for (uint16_t i = 0; i < cnt; ++i) {
            uint64_t oid = 0; uint8_t su = 0; std::string sym; uint32_t qty = 0; double price = 0;
            if (!readU64(b, n, o, oid) || !readU8(b, n, o, su) || !readStr1(b, n, o, sym) || !readU32(b, n, o, qty) || !readDouble(b, n, o, price))break;
            g_openOrders[oid] = { sym, oid, price, qty, (su == 0) ? 'B' : 'S' };
        }
    };
    refreshUI();
}

static void onTradeList(const char* b, int n, int o) {
    uint16_t cnt=0; if(!readU16(b,n,o,cnt)) return;
    std::lock_guard<std::mutex> lk(g_stateMtx);
    g_trades.clear();
    for(uint16_t i=0;i<cnt;++i){ uint64_t tid=0; std::string sym,dt; uint32_t qty=0; double price=0; uint8_t su=0;
      if(!readU64(b,n,o,tid)||!readStr1(b,n,o,sym)||!readU32(b,n,o,qty)||!readDouble(b,n,o,price)||!readU8(b,n,o,su)||!readStr1(b,n,o,dt))break;
      g_trades.push_back({ sym, dt, tid, price, qty, (su == 0) ? 'B' : 'S' }); }
    refreshUI();
}

static void onHistoryData(const char* b, int n, int o) {
    std::string sym; uint16_t cnt=0;
    if(!readStr1(b,n,o,sym)||!readU16(b,n,o,cnt)) return;
    std::vector<Candle> candles;
    for(uint16_t i=0;i<cnt;++i){
        Candle c; std::string dt;
        if(!readDouble(b,n,o,c.open)||!readDouble(b,n,o,c.high)||
           !readDouble(b,n,o,c.low)||!readDouble(b,n,o,c.close)||
           !readU32(b,n,o,c.vol)||!readStr1(b,n,o,dt)) break;
        c.dt=dt; candles.push_back(c);
    }
    {
        std::lock_guard<std::mutex> lk(g_chartMtx);
        g_chartSymbol=sym; g_chartCandles=std::move(candles);
    }
    refreshUI();
}

/*--------------------------------------------------------------------------
 * T_TcpRcv: TCP receive thread
 *--------------------------------------------------------------------------*/

static void tcpReceiveThread() {
    char hdr[3];
    while(g_running.load()) {
        if(!recvExact(g_tcpSocket, hdr, 3)) {
            if(g_expectDisconnect.load()) {
                // Expected disconnect after /logout — don't kill the terminal
                logMsg("[INFO] Server closed connection after logout.");
                refreshUI();
            } else {
                logMsg("[DISCONNECTED] Server connection closed.");
                g_running=false;
                if(g_screenPtr) g_screenPtr->Exit();
            }
            break;
        }
        uint8_t  cmdId  = (uint8_t)hdr[0];
        uint16_t payLen = 0; memcpy(&payLen,hdr+1,2); payLen=ntohs(payLen);

        std::vector<char> payload(payLen);
        if(payLen>0 && !recvExact(g_tcpSocket, payload.data(), payLen)) {
            if(g_expectDisconnect.load()) {
                logMsg("[INFO] Server closed connection after logout.");
                refreshUI();
            } else {
                logMsg("[DISCONNECTED] Server connection closed mid-payload.");
                g_running=false;
                if(g_screenPtr) g_screenPtr->Exit();
            }
            break;
        }
        int o=0;
        const char* b=payload.data(); int n=(int)payLen;

        switch((CmdID)cmdId) {
        case CMD_LOGIN_OK:      onLoginOk(b,n,o);      break;
        case CMD_LOGIN_FAIL:    onLoginFail(b,n,o);    break;
        case CMD_ORDER_ACK:     onOrderAck(b,n,o);     break;
        case CMD_ORDER_REJECT:  onOrderReject(b,n,o);  break;
        case CMD_TRADE_EXEC:    onTradeExec(b,n,o);    break;
        case CMD_CANCEL_ACK:    onCancelAck(b,n,o);    break;
        case CMD_CANCEL_REJECT: onCancelReject(b,n,o); break;
        case CMD_MARKET_DATA:   parseMarketData(b,n,o); break;
        case CMD_ACCOUNT_DATA:  onAccountData(b,n,o);  break;
        case CMD_SERVER_MSG:    onServerMsg(b,n,o);    break;
        case CMD_LOGOUT_OK:     onLogoutOk();           break;
        case CMD_ORDER_LIST:    onOrderList(b,n,o);    break;
        case CMD_TRADE_LIST:    onTradeList(b,n,o);    break;
        case CMD_HISTORY_DATA:  onHistoryData(b,n,o);  break;
        case CMD_QUERY_PORTFOLIO: {
            // Parse portfolio response: NumPos(2) + [Sym Qty(4) AvgCost(8) CurPrice(8)]*N + Cash(8)
            uint16_t numPos=0;
            if(!readU16(b,n,o,numPos)) break;
            std::ostringstream oss;
            oss << std::fixed << std::setprecision(2);
            oss << "  Symbol   Qty     AvgCost   Current    P&L       P&L%\n";
            oss << "  ------   ----    -------   -------    ---       ----\n";
            double totalValue = 0;
            for(uint16_t i=0;i<numPos;++i){
                std::string sym; uint32_t qty=0; double avg=0,cur=0;
                if(!readStr1(b,n,o,sym)||!readU32(b,n,o,qty)||!readDouble(b,n,o,avg)||!readDouble(b,n,o,cur)) break;
                double pnl = (cur - avg) * qty;
                double pnlPct = (avg > 0) ? ((cur - avg) / avg * 100.0) : 0;
                totalValue += cur * qty;
                oss << "  " << std::setw(6) << std::left << sym << "   "
                    << std::setw(6) << std::right << qty << "   $"
                    << std::setw(7) << avg << "   $"
                    << std::setw(7) << cur << "   "
                    << (pnl>=0?"+":"") << "$" << std::setw(8) << pnl << "  "
                    << (pnlPct>=0?"+":"") << pnlPct << "%\n";
            }
            double cash=0; readDouble(b,n,o,cash);
            totalValue += cash;
            oss.imbue(std::locale("en_SG.UTF-8"));
            oss << "Cash: " << std::showbase << std::put_money(cash * 100)
                << " | Total Value: " << std::showbase << std::put_money(totalValue * 100);
            //oss << "  Cash: $" << cash << "  |  Total Value: ";// << totalValue;
            logMsg(oss.str());
            break;
        }
        case CMD_DH_PUBLIC_KEY_RESPONSE: {
            uint64_t serverPublicKey = 0;
            if (!readU64(b, n, o, serverPublicKey)) {
                logMsg("Invalid DH public key response");
                break;
            }

            logMsg("DH: Received server public key = " + std::to_string(serverPublicKey));

            // Compute shared secret
            g_dh.computeSharedSecret(serverPublicKey);
            g_sessionKey = g_dh.getEncryptionKey(32);

            {
                std::lock_guard<std::mutex> lk(g_dhMutex);
                g_dhEstablished = true;
            }

            uint64_t secret = g_dh.getSharedSecret();
            logMsg("DH: Shared secret = " + std::to_string(secret));

            std::string keyStr;
            for (size_t i = 0; i < std::min(g_sessionKey.size(), (size_t)16); ++i) {
                char buf[4];
                sprintf_s(buf, "%02X ", g_sessionKey[i]);
                keyStr += buf;
            }
            logMsg("DH: Session key (first 16) = " + keyStr);

            logMsg("DH key exchange complete. Secure channel established.");
            break;
        }
        default: logMsg("[WARN] Unknown server response: "+std::to_string(cmdId)); break;
        }
    }
}

/*--------------------------------------------------------------------------
 * T_UdpRcv: UDP market-data broadcast receive thread
 *--------------------------------------------------------------------------*/

static void udpReceiveThread() {
    char buf[MAX_PAYLOAD+16];
    while(g_running.load()) {
        fd_set rs; FD_ZERO(&rs); FD_SET(g_udpSocket,&rs);
        timeval tv{0,50000};
        if(select(0,&rs,nullptr,nullptr,&tv)<=0) continue;

        sockaddr_in from{}; int fromLen=sizeof(from);
        int recvd=recvfrom(g_udpSocket,buf,sizeof(buf),0,(sockaddr*)&from,&fromLen);
        if(recvd<7) continue;

        int o=0;
        uint32_t seq=0; uint8_t cmdId=0; uint16_t payLen=0;
        readU32(buf,recvd,o,seq);
        readU8 (buf,recvd,o,cmdId);
        readU16(buf,recvd,o,payLen);

        if(seq <= g_udpLastSeq) continue;
        g_udpLastSeq = seq;
        if(o+payLen > recvd) continue;

        if((CmdID)cmdId == CMD_MARKET_DATA) {
            int po=o;
            std::string sym; double bid=0,ask=0,last=0; uint32_t bidQty=0,askQty=0,vol=0;
            if(readStr1(buf,recvd,po,sym)&&readDouble(buf,recvd,po,bid)&&readU32(buf,recvd,po,bidQty)&&
               readDouble(buf,recvd,po,ask)&&readU32(buf,recvd,po,askQty)&&readDouble(buf,recvd,po,last)&&readU32(buf,recvd,po,vol)){
                {
                    std::lock_guard<std::mutex> lk(g_mktMtx);
                    g_marketData[sym] = {bid,ask,last,bidQty,askQty,vol};
                }
                // Auto-refresh chart when data arrives for the viewed symbol
                if(sym == SYMBOLS[g_chartSymIdx]) requestChart(sym);
                refreshUI();
            }
        }
    }
}

/*--------------------------------------------------------------------------
 * Command processing  (called from TUI input)
 *--------------------------------------------------------------------------*/

 /**
  * Perform Diffie-Hellman key exchange with server
  * Returns true if handshake successful
  */
static bool performDHHandshake() {
    if (g_dhEstablished.load()) {
        return true;
    }

    // Create DH with random keys
    DiffieHellman clientDH;

    uint64_t clientPublic = clientDH.getPublicKey();


    // Send public key to server
    std::vector<char> p;
    pushU64(p, clientPublic);
    if (!sendFrame(g_tcpSocket, CMD_DH_PUBLIC_KEY, p)) {
        logMsg("Failed to send DH public key");
        return false;
    }

    // Store for later use
    g_dh = clientDH;

    // Wait for server's response
    auto start = std::chrono::steady_clock::now();
    while (!g_dhEstablished.load() &&
        std::chrono::steady_clock::now() - start < std::chrono::seconds(5)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    return g_dhEstablished.load();
}

static void cmdLogin(const std::string& user, const std::string& pass) {
    if(user.empty()||pass.empty()){logMsg("Usage: /login <username> <password>"); return;}
    {std::lock_guard<std::mutex> lk(g_stateMtx); g_username=user;}

    // Perform DH handshake if not already done
    if (!g_dhEstablished.load()) {
        logMsg("Establishing secure channel...");
        if (!performDHHandshake()) {
            logMsg("Warning: Using plaintext password (insecure)");
        }
    }

    std::vector<char> p;

    // ===== USERNAME SECTION =====
    // This pushes: [UsernameLen:1] [Username:var]
    pushStr1(p, user);

    // ===== PASSWORD SECTION =====
    if (g_dhEstablished.load() && !g_sessionKey.empty()) {
        // Encrypted password format:
        // [Flag:1] [EncryptedLen:2] [EncryptedData:var]

        // Push flag (1 = encrypted)
        pushU8(p, 1);

        // Encrypt the password
        std::vector<uint8_t> passBytes(pass.begin(), pass.end());
        auto encrypted = DiffieHellman::xorEncryptDecrypt(passBytes, g_sessionKey);

        // Push encrypted data length as uint16_t
        pushU16(p, (uint16_t)encrypted.size());

        // Push encrypted data
        p.insert(p.end(), encrypted.begin(), encrypted.end());

        logMsg("Sending encrypted login for '" + user + "'");
        logMsg("Username length: " + std::to_string(user.length()));
        logMsg("Encrypted password length: " + std::to_string(encrypted.size()));
    }
    else {
        // Plaintext password format:
        // [Flag:0] [Password using pushStr1 which pushes: PasswordLen:1 + Password:var]

        // Push flag (0 = plaintext)
        pushU8(p, 0);

        // This pushes: [PasswordLen:1] [Password:var]
        pushStr1(p, pass);

        logMsg("Sending plaintext login for '" + user + "'");
        logMsg("Username length: " + std::to_string(user.length()));
        logMsg("Password length: " + std::to_string(pass.length()));
    }

    if (!sendFrame(g_tcpSocket, CMD_LOGIN, p)) {
        logMsg("Failed to send login request");
    }
    else
        logMsg("Logging in as '" + user + "'...");
}
static void cmdLogout() { sendFrame(g_tcpSocket,CMD_LOGOUT,{}); }
static void cmdPlaceOrder(char side, const std::string& sym, uint32_t qty, double price) {
    if(!g_loggedIn){logMsg("Must be logged in."); return;}
    if(sym.empty()||qty==0||price<=0){logMsg(std::string("Usage: ")+(side=='B'?"/buy":"/sell")+" <SYM> <qty> <price>"); return;}
    std::vector<char> p; pushU8(p,side=='B'?0:1); pushStr1(p,sym); pushU32(p,qty); pushDouble(p,price);
    sendFrame(g_tcpSocket,CMD_PLACE_ORDER,p);
    logMsg("Sending " + std::string(side=='B'?"BUY ":"SELL ") + std::to_string(qty) + "x" + sym + " @ " + fmtPrice(price));
}
static void cmdCancelOrder(uint64_t oid) {
    if(!g_loggedIn){logMsg("Must be logged in."); return;}
    std::vector<char> p; pushU64(p,oid); sendFrame(g_tcpSocket,CMD_CANCEL_ORDER,p);
    logMsg("Cancelling order #" + std::to_string(oid));
}
static void cmdQueryMarket(const std::string& sym) {
    if(sym.empty()){logMsg("Usage: /market <SYM>"); return;}
    std::vector<char> p; pushStr1(p,sym); sendFrame(g_tcpSocket,CMD_QUERY_MARKET,p);
}
static void cmdSubMarket(uint16_t udpPort) {
    std::vector<char> p; pushU16(p,udpPort); sendFrame(g_tcpSocket,CMD_SUB_MARKET,p);
}

static void processCommand(const std::string& line) {
    if(line.empty()) return;
    std::istringstream iss(line); std::string cmd; iss>>cmd;

    if(cmd=="/q"||cmd=="/quit"||cmd=="/exit") {
        g_running=false;
        if(g_screenPtr) g_screenPtr->Exit();
    }
    else if(cmd=="/login")   { std::string u,pw; iss>>u>>pw; cmdLogin(u,pw); }
    else if(cmd=="/logout")  { cmdLogout(); }
    else if(cmd=="/buy"||cmd=="/sell") {
        std::string sym; uint32_t qty=0; double price=0; iss>>sym>>qty>>price;
        cmdPlaceOrder(cmd=="/buy"?'B':'S',sym,qty,price);
    }
    else if(cmd=="/cancel")  { uint64_t oid=0; iss>>oid; if(!oid){logMsg("Usage: /cancel <orderID>");}else cmdCancelOrder(oid); }
    else if(cmd=="/market")  { std::string sym; iss>>sym; cmdQueryMarket(sym); }
    else if(cmd=="/account") { sendFrame(g_tcpSocket,CMD_QUERY_ACCOUNT,{}); }
    else if(cmd=="/orders")  { sendFrame(g_tcpSocket,CMD_QUERY_ORDERS,{}); }
    else if(cmd=="/trades")  { sendFrame(g_tcpSocket,CMD_QUERY_TRADES,{}); }
    else if(cmd=="/graph") {
        std::string sym; iss>>sym;
        if(!sym.empty()){
            // Find symbol index
            for(int i=0;i<(int)SYMBOLS.size();++i) if(SYMBOLS[i]==sym){ g_chartSymIdx=i; break; }
        }
        requestChart(SYMBOLS[g_chartSymIdx]);
    }
    else if(cmd=="/mbuy") {
        std::string sym; uint32_t qty=0; iss>>sym>>qty;
        if(sym.empty()||qty==0){logMsg("Usage: /mbuy <SYM> <qty>"); }
        else if(!g_loggedIn){logMsg("Must be logged in.");}
        else {
            // Send price=0 to signal market order; server will use best ask
            std::vector<char> p; pushU8(p,0); pushStr1(p,sym); pushU32(p,qty); pushDouble(p,0.0);
            sendFrame(g_tcpSocket,CMD_PLACE_ORDER,p);
            logMsg("Sending MARKET BUY " + std::to_string(qty) + "x" + sym);
        }
    }
    else if(cmd=="/msell") {
        std::string sym; uint32_t qty=0; iss>>sym>>qty;
        if(sym.empty()||qty==0){logMsg("Usage: /msell <SYM> <qty>"); }
        else if(!g_loggedIn){logMsg("Must be logged in.");}
        else {
            // Send price=0 to signal market order; server will use best bid
            std::vector<char> p; pushU8(p,1); pushStr1(p,sym); pushU32(p,qty); pushDouble(p,0.0);
            sendFrame(g_tcpSocket,CMD_PLACE_ORDER,p);
            logMsg("Sending MARKET SELL " + std::to_string(qty) + "x" + sym);
        }
    }
    else if(cmd=="/stoploss") {
        std::string sym; uint32_t qty=0; double trigPx=0; iss>>sym>>qty>>trigPx;
        if(sym.empty()||qty==0||trigPx<=0){logMsg("Usage: /stoploss <SYM> <qty> <triggerPrice>"); }
        else if(!g_loggedIn){logMsg("Must be logged in."); }
        else { std::vector<char> p; pushU8(p,0); pushStr1(p,sym); pushU32(p,qty); pushDouble(p,trigPx); sendFrame(g_tcpSocket,CMD_STOP_ORDER,p); }
    }
    else if(cmd=="/takeprofit") {
        std::string sym; uint32_t qty=0; double trigPx=0; iss>>sym>>qty>>trigPx;
        if(sym.empty()||qty==0||trigPx<=0){logMsg("Usage: /takeprofit <SYM> <qty> <triggerPrice>"); }
        else if(!g_loggedIn){logMsg("Must be logged in."); }
        else { std::vector<char> p; pushU8(p,1); pushStr1(p,sym); pushU32(p,qty); pushDouble(p,trigPx); sendFrame(g_tcpSocket,CMD_STOP_ORDER,p); }
    }
    else if(cmd=="/portfolio") {
        if(!g_loggedIn){logMsg("Must be logged in."); }
        else { sendFrame(g_tcpSocket,CMD_QUERY_PORTFOLIO,{}); }
    }
    else if(cmd=="/stops") {
        if(!g_loggedIn){logMsg("Must be logged in."); }
        else { sendFrame(g_tcpSocket,CMD_QUERY_STOPS,{}); }
    }
    else if(cmd=="/cancelstop") {
        uint64_t cid=0; iss>>cid;
        if(!cid){logMsg("Usage: /cancelstop <id>"); }
        else if(!g_loggedIn){logMsg("Must be logged in."); }
        else { std::vector<char> p; pushU64(p,cid); sendFrame(g_tcpSocket,CMD_CANCEL_STOP,p); }
    }
    else if(cmd=="/crisis") {
        if(!g_loggedIn){ logMsg("Must be logged in to trigger crisis."); }
        else { sendFrame(g_tcpSocket, CMD_CRISIS, {}); }
    }
    else if(cmd=="/help") {
        logMsg("Commands: /login <user> <pass> | /logout | /buy <SYM> <qty> <price> | /sell <SYM> <qty> <price>");
        logMsg("          /mbuy <SYM> <qty> | /msell <SYM> <qty>  (market orders)");
        logMsg("          /stoploss <SYM> <qty> <price> | /takeprofit <SYM> <qty> <price>");
        logMsg("          /portfolio | /stops | /cancelstop <id>");
        logMsg("          /cancel <id> | /market <SYM> | /account | /orders | /trades | /graph [SYM]");
        logMsg("          /crisis | /q");
        logMsg("Use Tab to switch chart symbol. Symbols: AAPL GOOGL MSFT TSLA AMZN");
    }
    else { logMsg("Unknown command: " + cmd + " (type /help)"); }
}

/*--------------------------------------------------------------------------
 * SMA (Simple Moving Average) computation
 *--------------------------------------------------------------------------*/
static std::vector<double> computeSMA(const std::vector<Candle>& candles, int period) {
    std::vector<double> sma(candles.size(), 0.0);
    double sum = 0;
    for (int i = 0; i < (int)candles.size(); ++i) {
        sum += candles[i].close;
        if (i >= period) sum -= candles[i - period].close;
        if (i >= period - 1) sma[i] = sum / period;
    }
    return sma;
}

/*--------------------------------------------------------------------------
 * FTXUI Candlestick chart renderer (Canvas) — TradingView style
 *--------------------------------------------------------------------------*/

static ftxui::Element buildCandlestickChart() {
    using namespace ftxui;

    std::lock_guard<std::mutex> clk(g_chartMtx);
    std::string sym = g_chartSymbol.empty() ? SYMBOLS[g_chartSymIdx] : g_chartSymbol;
    auto& candles = g_chartCandles;

    if(candles.empty()) {
        return vbox({
            text(" CANDLESTICK CHART: " + sym) | bold,
            separator(),
            text("  No data yet. Login and trade to see the chart.") | center | flex,
        }) | border | color(Color::White);
    }

    // Snapshot candle data (we hold the lock), apply zoom
    std::vector<Candle> candlesCopy = candles;
    std::string symCopy = sym;
    // Zoom: only show the last g_chartZoom candles
    if ((int)candlesCopy.size() > g_chartZoom) {
        candlesCopy = std::vector<Candle>(candlesCopy.end() - g_chartZoom, candlesCopy.end());
    }
    int numCandles=(int)candlesCopy.size();

    // Find price range
    double minLow=1e18, maxHigh=-1e18;
    uint32_t totalVol=0;
    for(auto&c:candlesCopy){
        if(c.low<minLow) minLow=c.low;
        if(c.high>maxHigh) maxHigh=c.high;
        totalVol+=c.vol;
    }
    double range=maxHigh-minLow;
    if(range<0.01) range=1.0;
    // Just add padding — let the candle data drive the range naturally
    double pad=range*0.08;
    double priceLow=minLow-pad, priceHigh=maxHigh+pad;
    double priceRange=priceHigh-priceLow;

    // Find max volume for volume bar scaling
    uint32_t maxVol = 0;
    for(auto&c:candlesCopy){
        if(c.vol > maxVol) maxVol = c.vol;
    }

    auto lastC = candlesCopy.back();
    auto firstC = candlesCopy.front();
    double pctChange = (firstC.close > 0.001)
        ? ((lastC.close - firstC.close) / firstC.close * 100.0) : 0.0;
    bool lastBullish = (lastC.close >= lastC.open);

    // Compute SMA-10
    auto sma10 = computeSMA(candlesCopy, 10);

    // Format helper
    auto fmt2 = [](double v) -> std::string {
        std::ostringstream s; s << std::fixed << std::setprecision(2) << v; return s.str();
    };

    // OHLC header strings
    std::string ohlcStr = "O:" + fmt2(lastC.open) + "  H:" + fmt2(lastC.high)
                        + "  L:" + fmt2(lastC.low) + "  C:" + fmt2(lastC.close)
                        + "  V:" + std::to_string(totalVol);
    std::string chgStr = (pctChange >= 0 ? "+" : "") + fmt2(pctChange) + "%";

    // ── Canvas rendering (TradingView lightweight-charts style) ─────────
    // Rendering logic ported from tradingview/lightweight-charts:
    //   src/renderers/candlesticks-renderer.ts
    //   src/renderers/optimal-bar-width.ts
    auto chartElement = canvas([=](Canvas& chart) {
        int cw = chart.width();
        int ch = chart.height();
        if(cw < 30 || ch < 16) return;

        int leftMargin   = 2;
        int rightMargin  = 20;
        int topMargin    = 2;
        int bottomMargin = 6;

        int plotW = cw - leftMargin - rightMargin;
        int plotH = ch - topMargin - bottomMargin;
        if(plotW < 10 || plotH < 8) return;

        // Bar spacing (pixels per candle slot) — fit all candles with room
        double barSpacing = (double)plotW / std::max(numCandles, 1);
        if(barSpacing > 10.0) barSpacing = 10.0;
        if(barSpacing < 2.0)  barSpacing = 2.0;
        int candleWidth = (int)barSpacing;

        // TradingView's optimalCandlestickWidth (from optimal-bar-width.ts)
        // coeff = 1 - 0.2 * atan(max(4, barSpacing) - 4) / (PI/2)
        // bodyWidth = floor(barSpacing * coeff)
        int bodyWidth;
        if(barSpacing >= 2.5 && barSpacing <= 4.0) {
            bodyWidth = 3;  // special case: fixed 3px
        } else {
            double coeff = 1.0 - 0.2 * std::atan(std::max(4.0, barSpacing) - 4.0) / (3.14159265 * 0.5);
            bodyWidth = (int)(barSpacing * coeff);
            bodyWidth = std::min(bodyWidth, candleWidth);
            bodyWidth = std::max(1, bodyWidth);
        }
        int bodyHalf = bodyWidth / 2;

        // Wick width = 1 block (TradingView: floor(pixelRatio) = 1)
        // Ensure body and wick have matching parity for centering
        if(bodyWidth >= 2 && (1 % 2) != (bodyWidth % 2)) {
            bodyWidth--;
            bodyHalf = bodyWidth / 2;
        }

        auto priceToY = [&](double price) -> int {
            return topMargin + (int)((priceHigh - price) / priceRange * plotH);
        };

        // ── Price axis with "nice" round numbers + horizontal grid lines ──
        // Pick a nice tick interval based on the price range (like TradingView)
        // Target ~4-8 labels on the axis
        double rawTick = priceRange / 6.0;
        // Round to a "nice" number: 0.25, 0.50, 1.00, 2.00, 5.00, 10.00, 25.00, 50.00...
        double niceSteps[] = {0.10, 0.25, 0.50, 1.00, 2.00, 5.00, 10.00, 25.00, 50.00, 100.00};
        double tickSize = niceSteps[0];
        for (double ns : niceSteps) {
            if (ns >= rawTick) { tickSize = ns; break; }
        }

        int axisX = leftMargin + plotW + 2;
        // Draw labels at each tick from below priceLow to above priceHigh
        double firstTick = std::floor(priceLow / tickSize) * tickSize;
        for (double px = firstTick; px <= priceHigh + tickSize * 0.5; px += tickSize) {
            if (px < priceLow) continue;
            int y = priceToY(px);
            if (y < topMargin || y > topMargin + plotH) continue;

            // Horizontal grid line (dashed, subtle)
            for (int gx = leftMargin; gx < leftMargin + plotW; gx += 4) {
                chart.DrawBlock(gx, y, true, Color::GrayDark);
            }

            // Price label on the right
            std::string lbl = fmt2(px);
            int textX = axisX + 2;
            if (textX < 0) textX = 0;
            chart.DrawText(textX, (y / 4) * 4, lbl, Color::GrayLight);
        }

        // Always show the last price with a highlighted label
        {
            int yLast = priceToY(lastC.close);
            if (yLast >= topMargin && yLast <= topMargin + plotH) {
                // Dashed line across for current price
                Color lastClr = lastBullish ? Color::Green : Color::Red;
                for (int gx = leftMargin; gx < leftMargin + plotW; gx += 3) {
                    chart.DrawBlock(gx, yLast, true, lastClr);
                }
                // Price label highlighted
                std::string lbl = fmt2(lastC.close);
                int textX = axisX + 2;
                if (textX < 0) textX = 0;
                chart.DrawText(textX, (yLast / 4) * 4, lbl, lastClr);
            }
        }

        // ── Draw candles (right-aligned, TradingView draw order) ─────
        int totalCandlesW = numCandles * candleWidth;
        int candleStartX = leftMargin;  // left-aligned: oldest candle on the left
        (void)totalCandlesW;

        int prevRightEdge = -1;  // overlap prevention (from candlesticks-renderer.ts)

        for(int i = 0; i < numCandles; ++i) {
            auto& c = candlesCopy[i];
            int cx = candleStartX + i * candleWidth + candleWidth / 2;
            if(cx >= leftMargin + plotW) break;

            int yHigh  = priceToY(c.high);
            int yLow   = priceToY(c.low);
            int yOpen  = priceToY(c.open);
            int yClose = priceToY(c.close);

            bool bullish = (c.close >= c.open);
            Color clr    = bullish ? Color::Green : Color::Red;

            int bodyTop = bullish ? yClose : yOpen;
            int bodyBot = bullish ? yOpen  : yClose;
            if(bodyTop == bodyBot) bodyBot = bodyTop + 1;

            // Overlap prevention: clamp left edge to prevRightEdge + 1
            int left  = cx - bodyHalf;
            int right = cx + bodyHalf;
            if(prevRightEdge >= 0 && left <= prevRightEdge) {
                left = prevRightEdge + 1;
                if(left > right) left = right;
            }
            prevRightEdge = right;

            // --- TradingView draw order: wicks first, then body ---

            // Upper wick (high → body top) — thin, 1 block wide
            if(yHigh < bodyTop) {
                chart.DrawBlockLine(cx, yHigh, cx, bodyTop - 1, clr);
            }
            // Lower wick (body bottom → low)
            if(yLow > bodyBot) {
                chart.DrawBlockLine(cx, bodyBot + 1, cx, yLow, clr);
            }

            // Body — solid filled rectangle (left to right, overlap-aware)
            for(int y = bodyTop; y <= bodyBot; ++y) {
                for(int dx = left; dx <= right; ++dx) {
                    chart.DrawBlock(dx, y, true, clr);
                }
            }
        }

        // ── Time labels at bottom ────────────────────────────────────
        int labelInterval = numCandles <= 8 ? 1 : (numCandles <= 20 ? 2 : (numCandles <= 60 ? 4 : 8));
        int xAxisY = topMargin + plotH;
        int timeLabelY = xAxisY + 2;
        for(int i = 0; i < numCandles; i += labelInterval) {
            int cx = candleStartX + i * candleWidth + candleWidth / 2;
            if(cx >= leftMargin + plotW) break;
            std::string lbl = candlesCopy[i].dt;
            if(lbl.size() > 5) lbl = lbl.substr(0, 5);
            int textX = cx - (int)lbl.size();
            if(textX < 0) textX = 0;
            chart.DrawText(textX, (timeLabelY / 4) * 4, lbl, Color::GrayLight);
        }
    });

    // ── Build the FTXUI element tree ──────────────────────────────────
    auto chgColor = pctChange >= 0 ? Color::Green : Color::Red;
    auto lastClrUI = lastBullish ? Color::Green : Color::Red;

    return vbox({
        // Header: SYMBOL  %change | O: H: L: C: V:
        hbox({
            text(" " + symCopy + " ") | bold | color(Color::Yellow),
            text(" " + chgStr + " ") | bold | color(chgColor),
            text(" | ") | dim,
            text("O:" + fmt2(lastC.open) + " "),
            text("H:" + fmt2(lastC.high) + " ") | color(Color::GreenLight),
            text("L:" + fmt2(lastC.low) + " ")  | color(Color::RedLight),
            text("C:" + fmt2(lastC.close) + " ") | bold | color(lastClrUI),
            text("V:" + std::to_string(totalVol) + " ") | dim,
            filler(),
            text(" [Tab] switch ") | dim,
        }),
        separator(),
        chartElement | flex,
        separator(),
        hbox({
            text(" ") | bgcolor(Color::Green), text(" Bull "),
            text("  "),
            text(" ") | bgcolor(Color::Red), text(" Bear "),
            filler(),
            text(" " + std::to_string(numCandles) + " candles | Scroll to zoom ") | dim,
        }),
    }) | border;
}

/*--------------------------------------------------------------------------
 * FTXUI Market data panel
 *--------------------------------------------------------------------------*/

static ftxui::Element buildMarketPanel() {
    using namespace ftxui;

    std::lock_guard<std::mutex> lk(g_mktMtx);
    Elements rows;
    rows.push_back(hbox({text(" MARKET DATA ") | bold | color(Color::Cyan)}));
    rows.push_back(separator());
    rows.push_back(hbox({
        text(" SYM  ") | bold, text(" Bid      ") | bold,
        text(" Ask      ") | bold, text(" Last     ") | bold, text(" Vol  ") | bold,
    }));

    for(auto& sym : SYMBOLS) {
        auto it = g_marketData.find(sym);
        if(it != g_marketData.end()) {
            auto& m = it->second;
            std::ostringstream bid,ask,last;
            bid<<std::fixed<<std::setprecision(2)<<m.bid;
            ask<<std::fixed<<std::setprecision(2)<<m.ask;
            last<<std::fixed<<std::setprecision(2)<<m.last;
            bool isViewed = (sym == SYMBOLS[g_chartSymIdx]);
            auto row = hbox({
                text(" " + sym + " ") | (isViewed ? color(Color::Yellow) : color(Color::White)),
                text(" " + bid.str() + " ") | color(Color::Green),
                text(" " + ask.str() + " ") | color(Color::Red),
                text(" " + last.str() + " "),
                text(" " + std::to_string(m.vol) + " ") | dim,
            });
            rows.push_back(row);
        } else {
            rows.push_back(text(" " + sym + "  --") | dim);
        }
    }
    return vbox(rows) | border | flex;
}

/*--------------------------------------------------------------------------
 * FTXUI Account panel
 *--------------------------------------------------------------------------*/

static ftxui::Element buildAccountPanel() {
    using namespace ftxui;

    std::lock_guard<std::mutex> lk(g_stateMtx);
    Elements rows;
    if(!g_loggedIn) {
        rows.push_back(text(" ACCOUNT ") | bold | color(Color::Cyan));
        rows.push_back(separator());
        rows.push_back(text(" Not logged in") | dim);
        rows.push_back(text(" /login <user> <pass>") | dim);
    } else {
        rows.push_back(hbox({text(" ACCOUNT: ") | bold | color(Color::Cyan), text(g_username) | bold}));
        rows.push_back(separator());
        rows.push_back(hbox({text(" Cash: ") | bold, text(fmtMoney(g_cash)) | color(Color::Green)}));
        if(!g_holdings.empty()){
            rows.push_back(text(" Holdings:") | bold);
            for(auto&[sym,qty]:g_holdings)
                rows.push_back(text("  " + sym + " x" + std::to_string(qty)));
        }
        if(!g_openOrders.empty()){
            rows.push_back(separator());
            rows.push_back(text(" Orders (" + std::to_string(g_openOrders.size()) + "):") | bold);
            for(auto&[oid,lo]:g_openOrders)
                rows.push_back(text("  #" + std::to_string(oid) + " " +
                    std::string(lo.side=='B'?"B":"S") + " " + std::to_string(lo.qty) + "x" + lo.sym));
        }
    }
    return vbox(rows) | border | flex;
}

/*--------------------------------------------------------------------------
 * FTXUI Log panel
 *--------------------------------------------------------------------------*/

static ftxui::Element buildLogPanel() {
    using namespace ftxui;

    std::lock_guard<std::mutex> lk(g_logMtx);
    Elements lines;
    // Show last ~8 messages
    size_t start = g_logMessages.size() > 8 ? g_logMessages.size() - 8 : 0;
    for(size_t i=start; i<g_logMessages.size(); ++i) {
        auto& msg = g_logMessages[i];
        Color clr = Color::White;
        if(msg.find("TRADE")!=std::string::npos || msg.find("BOUGHT")!=std::string::npos || msg.find("SOLD")!=std::string::npos)
            clr = Color::Yellow;
        else if(msg.find("REJECTED")!=std::string::npos || msg.find("FAILED")!=std::string::npos)
            clr = Color::Red;
        else if(msg.find("ACCEPTED")!=std::string::npos || msg.find("LOGIN OK")!=std::string::npos)
            clr = Color::Green;
        lines.push_back(text(" " + msg) | color(clr));
    }
    if(lines.empty()) lines.push_back(text(" Welcome! Type /login <username> to begin.") | dim);
    return vbox(lines);
}

/*--------------------------------------------------------------------------
 * main()
 *--------------------------------------------------------------------------*/

int main() {
    // --- Step 1: Configuration (pre-TUI, uses normal console) ---
    std::string serverIP, tcpPortStr, udpPortStr, localUdpPortStr;
    std::cout<<"Server IP Address: ";      std::getline(std::cin,serverIP);
    while(!serverIP.empty()&&(serverIP.back()=='\r'||serverIP.back()=='\n')) serverIP.pop_back();
    std::cout<<"Server TCP Port Number: "; std::getline(std::cin,tcpPortStr);
    while(!tcpPortStr.empty()&&(tcpPortStr.back()=='\r'||tcpPortStr.back()=='\n')) tcpPortStr.pop_back();
    std::cout<<"Server UDP Port Number: "; std::getline(std::cin,udpPortStr);
    while(!udpPortStr.empty()&&(udpPortStr.back()=='\r'||udpPortStr.back()=='\n')) udpPortStr.pop_back();
    std::cout<<"Local UDP Port Number: ";  std::getline(std::cin,localUdpPortStr);
    while(!localUdpPortStr.empty()&&(localUdpPortStr.back()=='\r'||localUdpPortStr.back()=='\n')) localUdpPortStr.pop_back();

    uint16_t localUdpPort  = (uint16_t)std::stoi(localUdpPortStr);

    // --- Step 2: Winsock ---
    WSADATA wsaData{};
    if(WSAStartup(MAKEWORD(2,2),&wsaData)!=NO_ERROR){ std::cerr<<"WSAStartup failed.\n"; return 1; }

    // --- Step 3: TCP socket + connect ---
    addrinfo hints{},*info=nullptr;
    hints.ai_family=AF_INET; hints.ai_socktype=SOCK_STREAM; hints.ai_protocol=IPPROTO_TCP;
    if(getaddrinfo(serverIP.c_str(),tcpPortStr.c_str(),&hints,&info)!=0||!info){
        std::cerr<<"getaddrinfo failed.\n"; WSACleanup(); return 2;
    }
    g_tcpSocket=socket(info->ai_family,info->ai_socktype,info->ai_protocol);
    if(g_tcpSocket==INVALID_SOCKET){ std::cerr<<"socket failed.\n"; freeaddrinfo(info); WSACleanup(); return 3; }
    if(connect(g_tcpSocket,info->ai_addr,(int)info->ai_addrlen)==SOCKET_ERROR){
        std::cerr<<"connect failed.\n"; freeaddrinfo(info); closesocket(g_tcpSocket); WSACleanup(); return 4;
    }
    freeaddrinfo(info);
    std::cout<<"Connected to exchange at "<<serverIP<<":"<<tcpPortStr<<" (TCP)\n";

    // --- Step 4: UDP socket + bind ---
    g_udpSocket=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    if(g_udpSocket==INVALID_SOCKET){ std::cerr<<"UDP socket failed.\n"; closesocket(g_tcpSocket); WSACleanup(); return 5; }
    sockaddr_in udpBind{}; udpBind.sin_family=AF_INET; udpBind.sin_addr.s_addr=INADDR_ANY; udpBind.sin_port=htons(localUdpPort);
    if(bind(g_udpSocket,(sockaddr*)&udpBind,sizeof(udpBind))!=0){
        std::cerr<<"UDP bind failed.\n"; closesocket(g_udpSocket); closesocket(g_tcpSocket); WSACleanup(); return 6;
    }
    std::cout<<"Listening for UDP broadcasts on port "<<localUdpPort<<"\n";
    std::cout<<"Starting TUI...\n";

    // --- Step 5: Start background threads ---
    std::thread tcpRcvThr(tcpReceiveThread);
    std::thread udpRcvThr(udpReceiveThread);

    // --- Step 6: Subscribe to UDP market data ---
    cmdSubMarket(localUdpPort);

    // --- Step 7: FTXUI full-screen TUI ---
    {
        using namespace ftxui;
        auto screen = ScreenInteractive::Fullscreen();
        g_screenPtr = &screen;

        // Command input
        std::string inputStr;
        auto inputBox = Input(&inputStr, " Type command here (/help for commands)...");

        // Wrap to handle Enter key and Tab
        auto inputComponent = CatchEvent(inputBox, [&](Event event) {
            if(event == Event::Return) {
                std::string cmd = inputStr;
                inputStr.clear();
                processCommand(cmd);
                return true;
            }
            // Tab to switch chart symbol
            if(event == Event::Tab) {
                g_chartSymIdx = (g_chartSymIdx + 1) % (int)SYMBOLS.size();
                requestChart(SYMBOLS[g_chartSymIdx]);
                cmdQueryMarket(SYMBOLS[g_chartSymIdx]);
                return true;
            }
            return false;
        });

        // Build the TUI layout
        auto innerRenderer = Renderer(inputComponent, [&] {
            return vbox({
                // Top: Candlestick chart (takes most space)
                buildCandlestickChart() | flex,
                // Middle: Market data + Account side by side
                hbox({
                    buildMarketPanel(),
                    buildAccountPanel(),
                }) | size(HEIGHT, LESS_THAN, 12),
                // Log messages
                hbox({text(" LOG ") | bold | color(Color::Cyan)}) | borderLight,
                buildLogPanel() | size(HEIGHT, LESS_THAN, 9),
                // Input
                hbox({
                    text(" > ") | bold | color(Color::Green),
                    inputComponent->Render() | flex,
                }) | border,
            });
        });

        // Wrap entire UI to catch mouse scroll anywhere on screen
        auto renderer = CatchEvent(innerRenderer, [&](Event event) {
            if(event.is_mouse()) {
                if(event.mouse().button == Mouse::WheelUp) {
                    g_chartZoom = (std::max)(10, g_chartZoom - 5);
                    return true;
                }
                if(event.mouse().button == Mouse::WheelDown) {
                    g_chartZoom = (std::min)(120, g_chartZoom + 5);
                    return true;
                }
            }
            return false;
        });

        // Initial log
        logMsg("Connected to " + serverIP + ":" + tcpPortStr + ". Type /login <username> <password> to start.");
        logMsg("Commands: /help | Tab = switch chart symbol | /q = quit");

        // Request initial market data for all symbols
        for(auto& sym : SYMBOLS) cmdQueryMarket(sym);

        screen.Loop(renderer);
        g_screenPtr = nullptr;
    }

    // --- Step 8: Shutdown ---
    g_running=false;
    shutdown(g_tcpSocket,SD_BOTH);
    tcpRcvThr.join(); udpRcvThr.join();
    closesocket(g_tcpSocket); closesocket(g_udpSocket);
    WSACleanup();
    return 0;
}
