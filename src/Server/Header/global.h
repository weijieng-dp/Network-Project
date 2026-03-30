// static class to store all global variables.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "WinSock2.h"
#include "Windows.h"
#include "ws2tcpip.h"
#pragma comment(lib, "ws2_32.lib")

#include <mutex>
#include <unordered_map>
#include <deque>
#include <atomic>

#include "types.h"
#include "crypto.h"

class Global {
public:

	static std::vector<std::string> SYMBOLS;
	static std::map<std::string, double> REFERENCE_PRICES;

	/*--------------------------------------------------------------------------
	 * Port numbers
	 *--------------------------------------------------------------------------*/

	static uint16_t tcpPort;
	static uint16_t udpPort;
	static std::string ipAddr;

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

	struct UdpSubscriber { sockaddr_in addr; };				// !! needs changing
	static std::mutex              subMtx;
	static std::vector<UdpSubscriber> subscribers;

	struct BroadcastItem { std::vector<char> payload; };	// !! needs changing
	static std::mutex              bcastMtx;
	static std::condition_variable bcastCV;
	static std::deque<BroadcastItem> bcastQueue;

	/*--------------------------------------------------------------------------
	 * Persistence path + print mutex
	 *--------------------------------------------------------------------------*/

	static std::string persistPath;
	static std::mutex  printMtx;
	static std::atomic<bool> running;

	static std::string password;

	/*--------------------------------------------------------------------------
	 * Conditional orders (stop-loss / take-profit)
	 *--------------------------------------------------------------------------*/
	static std::vector<ConditionalOrder> conditionalOrders;
	static std::atomic<uint64_t> nextCondId;

	/*--------------------------------------------------------------------------
	 * House / Market-Maker account
	 *--------------------------------------------------------------------------*/

	static std::string HOUSE_USER;

	// DH key exchange state per client (store in session)
	struct DHSession {
		DiffieHellman dh;
		std::vector<uint8_t> sessionKey;
		bool established;

		DHSession() : established(false) {}
	};

	// Map of DH sessions by client socket (or username after login)
	static std::unordered_map<SOCKET, DHSession> dhSessions;
	static std::mutex dhMutex;
};