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
#include <deque>
#include "utils.h"
#include "crypto.h"

// ImGui + ImPlot + OpenGL for windowed GUI
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <implot.h>

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

// GLFW window reference for background threads to signal close
static GLFWwindow* g_window = nullptr;

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

/** Append a message to the log panel. ImGui redraws every frame, no explicit refresh needed. */
static void logMsg(const std::string& msg) {
    std::lock_guard<std::mutex> lk(g_logMtx);
    g_logMessages.push_back(msg);
    while(g_logMessages.size()>MAX_LOG_LINES) g_logMessages.pop_front();
}

/** No-op with ImGui — it redraws every frame at ~60 FPS. */
static void refreshUI() {}

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
    //g_expectDisconnect = true;  // server will close socket after LOGOUT_OK — don't treat as crash
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
                if(g_window) glfwSetWindowShouldClose(g_window, GLFW_TRUE);
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
                if(g_window) glfwSetWindowShouldClose(g_window, GLFW_TRUE);
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
    if (g_loggedIn) {   // if client is already logged, don't process login command till logout
        logMsg("Already logged in as [" + g_username + "]. Logout with /logout first.");
        return;
    }

    if(user.empty() || pass.empty()) {  // Invalid arguments provided
        logMsg("Usage: /login <username> <password>"); 
        return;
    }

    {   // Update username
        std::lock_guard<std::mutex> lk(g_stateMtx); 
        g_username = user;
    }

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
static void cmdLogout() { 
    if (!g_loggedIn) { logMsg("Not logged in."); return; }
    sendFrame(g_tcpSocket,CMD_LOGOUT,{}); 
}
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
        sendFrame(g_tcpSocket, CMD_QUIT, {});
        g_running=false;
        if(g_window) glfwSetWindowShouldClose(g_window, GLFW_TRUE);
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
 * ImPlot Candlestick chart renderer
 *--------------------------------------------------------------------------*/

/** Custom candlestick renderer using ImPlot public API (draw list after BeginPlot). */
static void PlotCandlestick(const double* xs, const double* opens,
    const double* closes, const double* lows, const double* highs, int count, float width_pct = 0.6f) {
    ImDrawList* dl = ImPlot::GetPlotDrawList();
    for (int i = 0; i < count; ++i) {
        ImVec2 openPos  = ImPlot::PlotToPixels(xs[i], opens[i]);
        ImVec2 closePos = ImPlot::PlotToPixels(xs[i], closes[i]);
        ImVec2 lowPos   = ImPlot::PlotToPixels(xs[i], lows[i]);
        ImVec2 highPos  = ImPlot::PlotToPixels(xs[i], highs[i]);

        bool bullish = closes[i] >= opens[i];
        ImU32 color = bullish ? IM_COL32(0, 200, 80, 255) : IM_COL32(220, 50, 50, 255);

        float halfW = (count > 1)
            ? (float)std::abs(ImPlot::PlotToPixels(xs[0], 0).x - ImPlot::PlotToPixels(xs[0] + 1, 0).x) * width_pct * 0.5f
            : 8.0f;
        halfW = (std::max)(halfW, 1.0f);

        // Wick
        dl->AddLine(ImVec2(highPos.x, highPos.y), ImVec2(lowPos.x, lowPos.y), color, 1.0f);
        // Body
        float top = (std::min)(openPos.y, closePos.y);
        float bot = (std::max)(openPos.y, closePos.y);
        if (bot - top < 1.0f) bot = top + 1.0f;
        dl->AddRectFilled(ImVec2(openPos.x - halfW, top), ImVec2(openPos.x + halfW, bot), color);
    }
}

static void drawCandlestickChart() {
    std::lock_guard<std::mutex> clk(g_chartMtx);
    std::string sym = g_chartSymbol.empty() ? SYMBOLS[g_chartSymIdx] : g_chartSymbol;

    if (g_chartCandles.empty()) {
        ImGui::TextDisabled("No data yet. Login and trade to see the chart.");
        return;
    }

    // Apply zoom
    int start = (int)g_chartCandles.size() > g_chartZoom ? (int)g_chartCandles.size() - g_chartZoom : 0;
    int count = (int)g_chartCandles.size() - start;

    // OHLC header
    auto& lastC = g_chartCandles.back();
    auto& firstC = g_chartCandles[start];
    double pctChange = (firstC.close > 0.001) ? ((lastC.close - firstC.close) / firstC.close * 100.0) : 0.0;
    uint32_t totalVol = 0;
    for (int i = start; i < (int)g_chartCandles.size(); ++i) totalVol += g_chartCandles[i].vol;

    ImGui::TextColored(ImVec4(1,1,0,1), "%s", sym.c_str()); ImGui::SameLine();
    ImVec4 chgClr = pctChange >= 0 ? ImVec4(0,1,0,1) : ImVec4(1,0,0,1);
    ImGui::TextColored(chgClr, "%+.2f%%", pctChange); ImGui::SameLine();
    ImGui::TextDisabled("|"); ImGui::SameLine();
    ImGui::Text("O:%.2f", lastC.open); ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.5f,1,0.5f,1), "H:%.2f", lastC.high); ImGui::SameLine();
    ImGui::TextColored(ImVec4(1,0.5f,0.5f,1), "L:%.2f", lastC.low); ImGui::SameLine();
    bool bullish = lastC.close >= lastC.open;
    ImGui::TextColored(bullish ? ImVec4(0,1,0,1) : ImVec4(1,0,0,1), "C:%.2f", lastC.close); ImGui::SameLine();
    ImGui::TextDisabled("V:%u", totalVol);

    // Prepare plot data arrays
    static std::vector<double> xs, opens, highs, lows, closes;
    xs.resize(count); opens.resize(count); highs.resize(count); lows.resize(count); closes.resize(count);
    double minLow = 1e18, maxHigh = -1e18;
    for (int i = 0; i < count; ++i) {
        auto& c = g_chartCandles[start + i];
        xs[i] = i;
        opens[i] = c.open; highs[i] = c.high;
        lows[i] = c.low;   closes[i] = c.close;
        if (c.low < minLow) minLow = c.low;
        if (c.high > maxHigh) maxHigh = c.high;
    }
    double pad = (maxHigh - minLow) * 0.08;

    if (ImPlot::BeginPlot("##Candles", ImVec2(-1, -1))) {
        ImPlot::SetupAxes("Time", "Price", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
        ImPlot::SetupAxisLimits(ImAxis_X1, -1, count + 1, ImPlotCond_Always);
        ImPlot::SetupAxisLimits(ImAxis_Y1, minLow - pad, maxHigh + pad, ImPlotCond_Always);

        PlotCandlestick(xs.data(), opens.data(), closes.data(),
            lows.data(), highs.data(), count);

        // SMA-10 overlay
        auto sma = computeSMA(
            std::vector<Candle>(g_chartCandles.begin() + start, g_chartCandles.end()), 10);
        static std::vector<double> smaXs, smaVals;
        smaXs.clear(); smaVals.clear();
        for (int i = 9; i < count; ++i) {
            if (sma[i] > 0) { smaXs.push_back(xs[i]); smaVals.push_back(sma[i]); }
        }
        if (!smaXs.empty()) {
            ImPlot::SetNextLineStyle(ImVec4(1, 0.8f, 0, 1), 1.5f);
            ImPlot::PlotLine("SMA-10", smaXs.data(), smaVals.data(), (int)smaXs.size());
        }

        ImPlot::EndPlot();
    }
}

/*--------------------------------------------------------------------------
 * ImGui Market data panel
 *--------------------------------------------------------------------------*/

static void drawMarketPanel() {
    ImGui::TextColored(ImVec4(0,1,1,1), "MARKET DATA");
    ImGui::Separator();

    if (ImGui::BeginTable("##mkt", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("SYM");
        ImGui::TableSetupColumn("Bid");
        ImGui::TableSetupColumn("Ask");
        ImGui::TableSetupColumn("Last");
        ImGui::TableSetupColumn("Vol");
        ImGui::TableHeadersRow();

        std::lock_guard<std::mutex> lk(g_mktMtx);
        for (auto& sym : SYMBOLS) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            bool viewed = (sym == SYMBOLS[g_chartSymIdx]);
            if (viewed) ImGui::TextColored(ImVec4(1,1,0,1), "%s", sym.c_str());
            else ImGui::Text("%s", sym.c_str());

            auto it = g_marketData.find(sym);
            if (it != g_marketData.end()) {
                auto& m = it->second;
                ImGui::TableNextColumn(); ImGui::TextColored(ImVec4(0,1,0,1), "%.2f", m.bid);
                ImGui::TableNextColumn(); ImGui::TextColored(ImVec4(1,0,0,1), "%.2f", m.ask);
                ImGui::TableNextColumn(); ImGui::Text("%.2f", m.last);
                ImGui::TableNextColumn(); ImGui::TextDisabled("%u", m.vol);
            } else {
                ImGui::TableNextColumn(); ImGui::TextDisabled("--");
                ImGui::TableNextColumn(); ImGui::TextDisabled("--");
                ImGui::TableNextColumn(); ImGui::TextDisabled("--");
                ImGui::TableNextColumn(); ImGui::TextDisabled("--");
            }
        }
        ImGui::EndTable();
    }
}

/*--------------------------------------------------------------------------
 * ImGui Account panel
 *--------------------------------------------------------------------------*/

static void drawAccountPanel() {
    std::lock_guard<std::mutex> lk(g_stateMtx);
    if (!g_loggedIn) {
        ImGui::TextColored(ImVec4(0,1,1,1), "ACCOUNT");
        ImGui::Separator();
        ImGui::TextDisabled("Not logged in");
        ImGui::TextDisabled("/login <user> <pass>");
    } else {
        ImGui::TextColored(ImVec4(0,1,1,1), "ACCOUNT: %s", g_username.c_str());
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0,1,0,1), "Cash: %s", fmtMoney(g_cash).c_str());
        if (!g_holdings.empty()) {
            ImGui::Text("Holdings:");
            for (auto& [sym, qty] : g_holdings)
                ImGui::BulletText("%s x%u", sym.c_str(), qty);
        }
        if (!g_openOrders.empty()) {
            ImGui::Separator();
            ImGui::Text("Orders (%zu):", g_openOrders.size());
            for (auto& [oid, lo] : g_openOrders)
                ImGui::BulletText("#%llu %c %ux%s", (unsigned long long)oid, lo.side, lo.qty, lo.sym.c_str());
        }
    }
}

/*--------------------------------------------------------------------------
 * ImGui Log panel
 *--------------------------------------------------------------------------*/

static void drawLogPanel() {
    std::lock_guard<std::mutex> lk(g_logMtx);
    for (auto& msg : g_logMessages) {
        ImVec4 clr(1,1,1,1);
        if (msg.find("TRADE") != std::string::npos || msg.find("BOUGHT") != std::string::npos || msg.find("SOLD") != std::string::npos)
            clr = ImVec4(1,1,0,1);
        else if (msg.find("REJECTED") != std::string::npos || msg.find("FAILED") != std::string::npos)
            clr = ImVec4(1,0,0,1);
        else if (msg.find("ACCEPTED") != std::string::npos || msg.find("LOGIN OK") != std::string::npos)
            clr = ImVec4(0,1,0,1);
        ImGui::TextColored(clr, "%s", msg.c_str());
    }
    if (g_logMessages.empty()) ImGui::TextDisabled("Welcome! Type /login <username> to begin.");
    // Auto-scroll to bottom
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 10.0f)
        ImGui::SetScrollHereY(1.0f);
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

    // --- Step 7: ImGui/ImPlot windowed GUI ---
    {
        // GLFW init
        if (!glfwInit()) { std::cerr << "GLFW init failed\n"; return 7; }
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        GLFWwindow* window = glfwCreateWindow(1400, 900, "Trading Platform", nullptr, nullptr);
        if (!window) { glfwTerminate(); return 8; }
        glfwMakeContextCurrent(window);
        glfwSwapInterval(1); // vsync
        g_window = window;

        // GLEW init
        glewExperimental = GL_TRUE;
        if (glewInit() != GLEW_OK) { std::cerr << "GLEW init failed\n"; return 9; }

        // ImGui + ImPlot init
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImPlot::CreateContext();
        // Build imgui.ini path relative to this source file's directory
        static std::string iniPath = []() {
            std::string dir = __FILE__;
            size_t pos = dir.find_last_of("\\/");
            if (pos != std::string::npos) dir = dir.substr(0, pos + 1);
            return dir + "imgui.ini";
        }();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        io.IniFilename = iniPath.c_str();
        ImGui::StyleColorsDark();
        ImGui_ImplGlfw_InitForOpenGL(window, true);
        ImGui_ImplOpenGL3_Init("#version 330");

        // Command input buffer
        static char inputBuf[256] = {};
        static bool focusInput = true;

        logMsg("Connected to " + serverIP + ":" + tcpPortStr + ". Type /login <username> <password> to start.");
        logMsg("Commands: /help | /q = quit");

        // Request initial market data for all symbols
        for (auto& sym : SYMBOLS) cmdQueryMarket(sym);

        // Main render loop
        while (!glfwWindowShouldClose(window) && g_running) {
            glfwPollEvents();

            // Mouse wheel zoom (when not over an ImGui scroll region)
            if (io.MouseWheel != 0 && !ImGui::IsAnyItemActive()) {
                g_chartZoom = std::clamp(g_chartZoom - (int)(io.MouseWheel * 5), 10, 120);
            }

            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();

            // Full-window dockspace
            ImGui::DockSpaceOverViewport();

            // --- Candlestick Chart Window ---
            ImGui::Begin("Chart");
            if (ImGui::BeginTabBar("##symbols")) {
                for (int i = 0; i < (int)SYMBOLS.size(); ++i) {
                    if (ImGui::BeginTabItem(SYMBOLS[i].c_str())) {
                        if (g_chartSymIdx != i) {
                            g_chartSymIdx = i;
                            requestChart(SYMBOLS[i]);
                            cmdQueryMarket(SYMBOLS[i]);
                        }
                        ImGui::EndTabItem();
                    }
                }
                ImGui::EndTabBar();
            }
            ImGui::TextDisabled("%d candles | Scroll to zoom", g_chartZoom);
            drawCandlestickChart();
            ImGui::End();

            // --- Market Data Window ---
            ImGui::Begin("Market Data");
            drawMarketPanel();
            ImGui::End();

            // --- Account Window ---
            ImGui::Begin("Account");
            drawAccountPanel();
            ImGui::End();

            // --- Log Window ---
            ImGui::Begin("Log");
            drawLogPanel();
            ImGui::End();

            // --- Command Input Window ---
            ImGui::Begin("Command");
            ImGui::TextColored(ImVec4(0,1,0,1), ">"); ImGui::SameLine();
            ImGui::SetNextItemWidth(-1);
            if (focusInput) { ImGui::SetKeyboardFocusHere(); focusInput = false; }
            if (ImGui::InputText("##cmd", inputBuf, sizeof(inputBuf), ImGuiInputTextFlags_EnterReturnsTrue)) {
                processCommand(std::string(inputBuf));
                inputBuf[0] = '\0';
                focusInput = true;  // re-focus after command
            }
            ImGui::End();

            // Render
            ImGui::Render();
            int dw, dh;
            glfwGetFramebufferSize(window, &dw, &dh);
            glViewport(0, 0, dw, dh);
            glClearColor(0.06f, 0.06f, 0.08f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            glfwSwapBuffers(window);
        }

        // Cleanup
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImPlot::DestroyContext();
        ImGui::DestroyContext();
        glfwDestroyWindow(window);
        glfwTerminate();
        g_window = nullptr;
    }

    // --- Step 8: Shutdown ---
    g_running=false;
    shutdown(g_tcpSocket,SD_BOTH);
    tcpRcvThr.join(); udpRcvThr.join();
    closesocket(g_tcpSocket); closesocket(g_udpSocket);
    WSACleanup();
    return 0;
}
