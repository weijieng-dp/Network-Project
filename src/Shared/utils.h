// Shared utils file for client and server.
#pragma once
/*--------------------------------------------------------------------------
 * Protocol command IDs
 *--------------------------------------------------------------------------*/
enum CmdID : uint8_t {
    // Client -> Server
    CMD_LOGIN = 0x01, CMD_LOGOUT = 0x02,
    CMD_PLACE_ORDER = 0x03, CMD_CANCEL_ORDER = 0x04,
    CMD_QUERY_MARKET = 0x05, CMD_QUERY_ACCOUNT = 0x06,
    CMD_QUERY_ORDERS = 0x07, CMD_QUERY_TRADES = 0x08,
    CMD_SUB_MARKET = 0x09,
    CMD_QUERY_HISTORY = 0x0A,
    CMD_CRISIS = 0x0B,
    CMD_STOP_ORDER = 0x0C,   // stop-loss / take-profit
    CMD_QUERY_PORTFOLIO = 0x0D,
    CMD_QUERY_STOPS = 0x0E,
    CMD_CANCEL_STOP = 0x0F,
    // Server -> Client
    CMD_LOGIN_OK = 0x81, CMD_LOGIN_FAIL = 0x82,
    CMD_ORDER_ACK = 0x83, CMD_ORDER_REJECT = 0x84,
    CMD_TRADE_EXEC = 0x85, CMD_CANCEL_ACK = 0x86,
    CMD_CANCEL_REJECT = 0x87, CMD_MARKET_DATA = 0x88,
    CMD_ACCOUNT_DATA = 0x89, CMD_SERVER_MSG = 0x8A,
    CMD_LOGOUT_OK = 0x8B, CMD_ORDER_LIST = 0x8C,
    CMD_TRADE_LIST = 0x8D, CMD_HISTORY_DATA = 0x8E,
};

/*--------------------------------------------------------------------------
 * Serialisation helpers for building payloads
 *--------------------------------------------------------------------------*/

inline void pushU8(std::vector<char>& b, uint8_t  v) { b.push_back((char)v); }
inline void pushU16(std::vector<char>& b, uint16_t v) { v = htons(v);  b.insert(b.end(), (char*)&v, (char*)&v + 2); }
inline void pushU32(std::vector<char>& b, uint32_t v) { v = htonl(v);  b.insert(b.end(), (char*)&v, (char*)&v + 4); }
inline void pushU64(std::vector<char>& b, uint64_t v) {
    uint32_t hi = htonl((uint32_t)(v >> 32)), lo = htonl((uint32_t)(v & 0xFFFFFFFF));
    b.insert(b.end(), (char*)&hi, (char*)&hi + 4); b.insert(b.end(), (char*)&lo, (char*)&lo + 4);
}
inline void pushDouble(std::vector<char>& b, double v) { uint64_t bits; memcpy(&bits, &v, 8); pushU64(b, bits); }
inline void pushStr1(std::vector<char>& b, const std::string& s) {
    uint8_t len = (uint8_t)std::min(s.size(), (size_t)255); pushU8(b, len);
    b.insert(b.end(), s.begin(), s.begin() + len);
}

/*--------------------------------------------------------------------------
 * Deserialisation helpers  (read from raw received payload buffer)
 *--------------------------------------------------------------------------*/

inline bool readU8(const char* b, int n, int& o, uint8_t& v) { if (o + 1 > n)return false; v = (uint8_t)b[o++]; return true; }
inline bool readU16(const char* b, int n, int& o, uint16_t& v) { if (o + 2 > n)return false; memcpy(&v, b + o, 2); v = ntohs(v); o += 2; return true; }
inline bool readU32(const char* b, int n, int& o, uint32_t& v) { if (o + 4 > n)return false; memcpy(&v, b + o, 4); v = ntohl(v); o += 4; return true; }
inline bool readU64(const char* b, int n, int& o, uint64_t& v) {
    if (o + 8 > n)return false; uint32_t hi, lo; memcpy(&hi, b + o, 4); memcpy(&lo, b + o + 4, 4);
    v = ((uint64_t)ntohl(hi) << 32) | ntohl(lo); o += 8; return true;
}
inline bool readDouble(const char* b, int n, int& o, double& v) { uint64_t bits = 0; if (!readU64(b, n, o, bits))return false; memcpy(&v, &bits, 8); return true; }
inline bool readStr1(const char* b, int n, int& o, std::string& v) {
    uint8_t len = 0; if (!readU8(b, n, o, len))return false; if (o + len > n)return false; v.assign(b + o, len); o += len; return true;
}

/*--------------------------------------------------------------------------
 * TCP framing helpers  (mirrors Assignment 4 recvExact / sendAll)
 *--------------------------------------------------------------------------*/

 /**
  * @brief Receive exactly 'len' bytes over TCP, handling partial reads.
  * @return true on success, false on disconnect or error.
  */
inline bool recvExact(SOCKET s, char* buf, int len) {
    int total = 0;
    while (total < len) {
        int r = recv(s, buf + total, len - total, 0);
        if (r <= 0) return false;
        total += r;
    }
    return true;
}

/**
 * @brief Send all 'len' bytes over TCP, handling partial sends.
 * @return true on success, false on error.
 */
inline bool sendAll(SOCKET s, const char* buf, int len) {
    int total = 0;
    while (total < len) {
        int sent = send(s, buf + total, len - total, 0);
        if (sent == SOCKET_ERROR) return false;
        total += sent;
    }
    return true;
}


inline std::string nowString() {
    time_t t = time(nullptr); struct tm tm {}; localtime_s(&tm, &t);
    char buf[32]; strftime(buf, sizeof(buf), "%Y-%m-%d_%H:%M:%S", &tm); return buf;
}