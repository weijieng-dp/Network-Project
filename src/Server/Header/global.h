// static class to store all global variables.
#pragma once
#include "WinSock2.h"
#include "Windows.h"
#include "ws2tcpip.h"
#pragma comment(lib, "ws2_32.lib")

#include <mutex>
#include <unordered_map>
#include <deque>

#include "types.h"

class Global {
public:

	static std::vector<std::string> SYMBOLS;

	/*--------------------------------------------------------------------------
	 * Global exchange state  (protected by exMtx)
	 *--------------------------------------------------------------------------*/

	static std::mutex exMtx;   // guards all exchange state below

	static std::unordered_map<std::string, Account>   accounts;
	static std::unordered_map<std::string, OrderBook> books;
	static std::vector<Trade>                         allTrades;
	static std::unordered_map<uint64_t, Order>        liveOrders;
	static std::atomic<uint64_t> nextOrderId, nextTradeId;

	// Map username -> TCP socket (for pushing TRADE_EXEC to counterparty)
	static std::unordered_map<std::string, SOCKET> userSockets;
	static std::mutex userSockMtx;

	/*--------------------------------------------------------------------------
	 * UDP broadcast state
	 *--------------------------------------------------------------------------*/

	static SOCKET udpSocket;
	static std::atomic<uint32_t> udpSeq;

	struct UdpSubscriber { sockaddr_in addr; };
	static std::mutex              subMtx;
	static std::vector<UdpSubscriber> subscribers;

	struct BroadcastItem { std::vector<char> payload; };
	static std::mutex              bcastMtx;
	static std::condition_variable bcastCV;
	static std::deque<BroadcastItem> bcastQueue;

	/*--------------------------------------------------------------------------
	 * Persistence path + print mutex
	 *--------------------------------------------------------------------------*/

	static std::string persistPath;
	static std::mutex  printMtx;
	static std::atomic<bool> running;

	/*--------------------------------------------------------------------------
	 * Conditional orders (stop-loss / take-profit)
	 *--------------------------------------------------------------------------*/
	static std::vector<ConditionalOrder> conditionalOrders;
	static std::atomic<uint64_t> nextCondId;

};