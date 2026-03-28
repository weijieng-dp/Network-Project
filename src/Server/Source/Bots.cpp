#include "Bots.h"
#include "persistence.h"
#include <string>
#include <iostream>
#include <mutex>
#include <fstream>

#include "global.h"
#include "utils.h"
#include "types.h"
#include <random>

std::array<Bot, TotalBots> BotManager::bots{}; // momentum, mean-reversion
std::array<Bot, 1> BotManager::MarketMakers{}; // momentum, mean-reversion


void Bot::InitBot(double currentPriceMarket, std::string botName,bool isMarketMaker)
{
	static std::mt19937 rng(std::random_device{}());
	std::uniform_int_distribution<int> dist(0, StrategyCount - 1);
	std::uniform_real_distribution<float> reaction(0.2f, 1.2f);
	std::uniform_int_distribution<int> hold(50, 200);
	std::uniform_int_distribution<int> sym(0, Global::SYMBOLS.size() - 1);
	std::uniform_real_distribution<float> cd(0.2f, 1.2f);
	std::uniform_real_distribution<float> mw(0.2f, 1.0f);
	std::uniform_real_distribution<float> mrw(0.2f, 1.0f);
	std::uniform_real_distribution<float> hw(0.0f, 1.0f);

	Symbol = Symbol = Global::SYMBOLS[sym(rng)];


	if (isMarketMaker)
	{
		bot = Strategy::MarketMaker;
		Global::accounts[botName].cash = 500000;
		Global::accounts[botName].holdings[Symbol] = 10000;
	}
	else
	{
		bot = Strategy::Mean_Reversion;
		Global::accounts[botName].cash = 50000;
		Global::accounts[botName].holdings[Symbol] = 100;
	}



	Global::accounts[botName].username = botName;

	botname = botName;
	reactionSpeed = reaction(rng);
	lastPriceSeen = currentPriceMarket;
	cooldown = cd(rng);

	if (Global::accounts[botName].holdings[Symbol] > 0) averageEntryPrice = lastPriceSeen;
	else averageEntryPrice = 0.0f;

	momentumWeight = mw(rng);
	meanReversionWeight = mrw(rng);
	herdWeight = hw(rng);

	float minRisk = 0.2f;
	float maxRisk = 1.0f;

	float minBias = -0.2f;
	float maxBias = 0.2f;

	switch (bot) {
	case Mean_Reversion:
		minRisk = 0.2f; maxRisk = 0.5f;
		minBias = -0.2f; maxBias = 0.2f;
		momentumWeight *= 0.3f;
		meanReversionWeight *= 1.2f;
		break;

	case Momentum:
		minRisk = 0.5f; maxRisk = 1.0f;
		minBias = 0.0f; maxBias = 0.3f;
		momentumWeight *= 1.2f;
		meanReversionWeight *= 0.3f;
		break;

	case Trend_Following:
		minRisk = 0.7f; maxRisk = 1.2f;
		minBias = 0.2f; maxBias = 0.6f;
		momentumWeight *= 1.2f;
		break;
	case MarketMaker:
		minRisk = 0.3f; maxRisk = 0.6f;
		minBias = 0.0f; maxBias = 0.0f;
		momentumWeight = 0.0f;
		meanReversionWeight = 0.0f;
		herdWeight = 0.0f;
		bias = 0.0f;
		break;

	case HerdBehavior:
		minRisk = 0.8f; maxRisk = 1.6f;
		minBias = -0.5f; maxBias = 0.5f;
		herdWeight *= 1.5f;
		break;
	case PanicSelling:
		minRisk = 1.2f; maxRisk = 1.8f;
		minBias = -1.0f; maxBias = -0.5f;
		herdWeight *= 1.2f;
		break;
	}


	std::uniform_real_distribution<float> risk(minRisk, maxRisk);
	std::uniform_real_distribution<float> biasProb(minBias, maxBias);

	riskTolerance = risk(rng);
	bias = biasProb(rng);
}

void BotManager::InitBots(double currentPriceMarket)
{
	for (int i = 0; i < TotalBots; i++)
	{
		bots[i].InitBot(currentPriceMarket, "bot_" + std::to_string(i));
	}
}

void BotManager::InitMarketMaker(double currentPriceMarket)
{
	for (int i = 0; i < 1; i++)
	{
		MarketMakers[i].InitBot(currentPriceMarket, "MarketMaker_" + std::to_string(i),true);
	}
}

std::array<Order, 2> BotManager::MarketMakerStrategy(Bot& bot, double const& bestbid, double const& bestask)
{
	double midprice = bot.lastPriceSeen;

	double spread = midprice * 0.002;

	double bidPrice = midprice - spread * 0.5;
	double askPrice = midprice + spread * 0.5;

	auto& acc = Global::accounts[bot.botname];

	double targetInventory = 100.0;
	double inventoryError = Global::accounts[bot.botname].holdings[bot.Symbol] - targetInventory;
	double skew = inventoryError * 0.001;

	double maxSkew = spread * 0.4;
	skew = std::clamp(skew, -maxSkew, maxSkew);
	bidPrice -= skew;
	askPrice -= skew;

	if (bidPrice < 0.01)
		bidPrice = 0.01;
	if (askPrice <= bidPrice)
		askPrice = bidPrice + 0.01;

	uint32_t qty = static_cast<uint32_t>(50 * bot.riskTolerance);
	if (qty < 10) qty = 10;

	bool canbuy = (qty * bidPrice) < Global::accounts[bot.botname].cash;
	bool cansell = qty < Global::accounts[bot.botname].holdings[bot.Symbol];



	Order ordB;
	Order ordS;

	if (canbuy)
	{
		ordB.symbol = bot.Symbol;
		ordB.orderId = Global::nextOrderId.fetch_add(1);
		ordB.username = bot.botname;
		ordB.side = 'B';
		ordB.price = bidPrice;
		ordB.origQty = qty;
		ordB.qty = qty;
		ordB.ts = std::chrono::steady_clock::now();
	}

	if (cansell)
	{
		ordS.symbol = bot.Symbol;
		ordS.orderId = Global::nextOrderId.fetch_add(1);
		ordS.username = bot.botname;
		ordS.side = 'S';
		ordS.price = askPrice;
		ordS.origQty = qty;
		ordS.qty = qty;
		ordS.ts = std::chrono::steady_clock::now();

	}

	std::array<Order, 2> orders;
	orders[0] = ordB;
	orders[1] = ordS;



	return orders;
}

void BotManager::MomentumStrategy()
{
}

void BotManager::TrendFollowingStrategy()
{
}

void BotManager::MeanReversionStrategy(Bot& bot,
	std::function<void(Order&, OrderBook&)> matchingfunction)
{
	auto& book = Global::books[bot.Symbol];
	auto& house = Global::accounts[bot.botname];

	double mmMidprice = 0.0;
	for (int i = 0; i < 1; i++)
		if (MarketMakers[i].Symbol == bot.Symbol)
		{
			mmMidprice = MarketMakers[i].lastPriceSeen; break;
		}
	if (mmMidprice <= 0) return;

	double reference = bot.lastPriceSeen;
	double deviation = (mmMidprice - reference) / reference;

	// Always update reference so it doesn't get permanently stale
	bot.lastPriceSeen = mmMidprice;

	if (std::abs(deviation) < 0.001) return;

	uint32_t qty = 10;
	Order ord;
	ord.orderId = Global::nextOrderId.fetch_add(1);
	ord.username = bot.botname;
	ord.symbol = bot.Symbol;
	ord.ts = std::chrono::steady_clock::now();
	ord.origQty = qty;
	ord.qty = qty;

	// In MeanReversionStrategy, swap the conditions:
if (deviation > 0 && !book.bids.empty())
{
    // Price rose above reference — sell into bid
    ord.side = 'S';
    ord.price = book.bids.begin()->first;
    if (house.holdings[bot.Symbol] < qty) return;
    house.holdings[bot.Symbol] -= qty;
}
else if (deviation < 0 && !book.asks.empty())
{
    // Price dropped below reference — buy into ask
    ord.side = 'B';
    ord.price = book.asks.begin()->first;
    if (house.cash < ord.price * qty) return;
    house.cash -= ord.price * qty;
}
	else return;

	Global::liveOrders[ord.orderId] = ord;
	house.openOrders[ord.orderId] = ord;

	std::cout << "[MR] " << bot.botname
		<< " sym=" << bot.Symbol
		<< " side=" << ord.side
		<< " price=" << std::fixed << std::setprecision(4) << ord.price
		<< " qty=" << ord.qty
		<< " deviation=" << deviation << "\n";

	matchingfunction(ord, book);

	// Update reference to MM midprice so bot tracks the walk
	bot.lastPriceSeen = mmMidprice;

	// Refund unmatched remainder
	if (ord.qty > 0)
	{
		if (ord.side == 'B')
		{
			house.cash += ord.price * ord.qty;
			book.bids[ord.price].erase(ord.orderId);
			if (book.bids[ord.price].empty()) book.bids.erase(ord.price);
		}
		else
		{
			house.holdings[bot.Symbol] += ord.qty;
			book.asks[ord.price].erase(ord.orderId);
			if (book.asks[ord.price].empty()) book.asks.erase(ord.price);
		}
		Global::liveOrders.erase(ord.orderId);
		house.openOrders.erase(ord.orderId);
	}
}
void BotManager::HerdBehaviorStrategy()
{
}

void BotManager::PanicSellingStrategy()
{
}

void BotManager::CancelOrder(Bot& bot)
{

	Account& house = Global::accounts[bot.botname];
	OrderBook& book = Global::books[bot.Symbol];


	std::vector<uint64_t> toCancel;
	for (auto& [price, lvl] : book.asks)
		for (auto& [oid, ord] : lvl)
			if (ord.username == bot.botname) toCancel.push_back(oid);
	for (auto& [price, lvl] : book.bids)
		for (auto& [oid, ord] : lvl)
			if (ord.username == bot.botname) toCancel.push_back(oid);
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
			house.holdings[bot.Symbol] += ord.qty;
			book.asks[ord.price].erase(oid);
			if (book.asks[ord.price].empty()) book.asks.erase(ord.price);
		}
		Global::liveOrders.erase(oid);
		house.openOrders.erase(oid);
	}
}

void BotManager::PlaceOrder(Bot& bot, Order orders, char side)
{
	if (orders.orderId > 0)
	{
		auto& book = Global::books[bot.Symbol];
		Account& house = Global::accounts[bot.botname];

		if (side == 'B')
		{
			house.cash -= orders.price * orders.qty;
			book.bids[orders.price][orders.orderId] = orders;
			Global::liveOrders[orders.orderId] = orders;
			house.openOrders[orders.orderId] = orders;
		}
		else
		{
			house.holdings[bot.Symbol] -= orders.qty;
			book.asks[orders.price][orders.orderId] = orders;
			Global::liveOrders[orders.orderId] = orders;
			house.openOrders[orders.orderId] = orders;
		}
	}
}

void BotManager::ProcessStrategies(std::function<void(Order& ord, OrderBook& book)> matchingfunction)
{
	for (int i = 0; i < TotalBots; i++)
	{
		auto& book = Global::books[bots[i].Symbol];
		Account& house = Global::accounts[bots[i].botname];

		CancelOrder(bots[i]);


		switch (bots[i].bot)
		{
		case Mean_Reversion:

			MeanReversionStrategy(bots[i], matchingfunction);  // ← fill this in
			break;

		case Momentum:
			break;

		case Trend_Following:
			break;
		case HerdBehavior:
			break;
		case PanicSelling:
			break;
		}
	}
}

void BotManager::ProcessMarketMaker(std::function<void(Order& ord, OrderBook& book)> matchingfunction)
{
	for (int i = 0; i < 1; i++)
	{
		auto& book = Global::books[MarketMakers[i].Symbol];
		Account& house = Global::accounts[MarketMakers[i].botname];

		CancelOrder(MarketMakers[i]);


		static std::mt19937 rng(std::random_device{}());
		std::normal_distribution<double> noise(0.0, 0.001);
		MarketMakers[i].lastPriceSeen *= (1.0 + noise(rng)); // ← correct


		double bid = book.bids.empty() ? 0.0 : book.bids.begin()->first;
		double ask = book.asks.empty() ? 0.0 : book.asks.begin()->first;

		std::array<Order, 2> orders = MarketMakerStrategy(MarketMakers[i], bid, ask);

		PlaceOrder(MarketMakers[i], orders[0], 'B');

		std::cout << "[MM] " << MarketMakers[i].botname
			<< " sym=" << orders[0].symbol  // will print empty string if bug is present
			<< " bid=" << orders[0].price
			<< " ask=" << orders[1].price << "\n";


		PlaceOrder(MarketMakers[i], orders[1], 'S');

		std::cout << "[MM] " << MarketMakers[i].botname
			<< " sym=" << orders[0].symbol  // will print empty string if bug is present
			<< " bid=" << orders[0].price
			<< " ask=" << orders[1].price << "\n";
		matchingfunction(orders[0], Global::books[MarketMakers[i].Symbol]);
		matchingfunction(orders[1], Global::books[MarketMakers[i].Symbol]);

	}
}

