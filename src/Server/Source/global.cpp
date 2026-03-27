#include "global.h"

std::vector<std::string>					Global::SYMBOLS{ "AAPL" };

std::mutex									Global::exMtx;   

std::unordered_map<std::string, Account>	Global::accounts;
std::unordered_map<std::string, OrderBook>	Global::books;
std::vector<Trade>							Global::allTrades;
std::unordered_map<uint64_t, Order>			Global::liveOrders;
std::atomic<uint64_t>						Global::nextOrderId{ 1 };
std::atomic<uint64_t>						Global::nextTradeId{ 1 };

std::unordered_map<std::string, SOCKET>		Global::userSockets;
std::mutex									Global::userSockMtx;

SOCKET										Global::udpSocket{ INVALID_SOCKET };
std::atomic<uint32_t>						Global::udpSeq{ 1 };

std::mutex									Global::subMtx;
std::vector<Global::UdpSubscriber>			Global::subscribers;

std::mutex									Global::bcastMtx;
std::condition_variable						Global::bcastCV;
std::deque<Global::BroadcastItem>			Global::bcastQueue;


std::string									Global::persistPath;
std::mutex									Global::printMtx;
std::atomic<bool>							Global::running{ true };

std::vector<ConditionalOrder>				Global::conditionalOrders;
std::atomic<uint64_t>						Global::nextCondId{ 1 };