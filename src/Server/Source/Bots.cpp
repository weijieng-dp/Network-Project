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
std::array<Bot, 5> BotManager::MarketMakers{}; // momentum, mean-reversion
std::array<Bot, 5> BotManager::NoiseTraders{}; // momentum, mean-reversion
static std::mt19937 rng(std::random_device{}());


void Bot::InitBot( std::string botName,Strategy strategy)
{
	std::uniform_int_distribution<int> dist(1, StrategyCount - 3);
	std::uniform_real_distribution<float> reaction(0.2f, 1.2f);
	std::uniform_int_distribution<int> hold(50, 200);
	std::uniform_int_distribution<int> sym(0, Global::SYMBOLS.size() - 1);
	std::uniform_real_distribution<float> cd(0.2f, 1.2f);
	std::uniform_real_distribution<float> mw(0.2f, 1.0f);
	std::uniform_real_distribution<float> mrw(0.2f, 1.0f);
	std::uniform_real_distribution<float> hw(0.0f, 1.0f);



	if (strategy == MarketMaker)
	{
		bot = Strategy::MarketMaker;
		botname = "MarketMaker_" + botName;
		Symbol = botName;
		auto& acc = Global::accounts[botname];
		acc.username = botname;
		acc.cash = 500000;
		acc.holdings[botName] = 10000;
		lastPriceSeen = Global::REFERENCE_PRICES[botName];
		if (Global::accounts[botname].holdings[botName] > 0) averageEntryPrice = lastPriceSeen;
		else averageEntryPrice = 0.0f;
	}
	else if (strategy == NoiseTrader)
	{
		bot = Strategy::NoiseTrader;
		botname = "NoiseTrader_" + botName;
		Symbol = botName;
		auto& acc = Global::accounts[botname];
		acc.username = botname;
		acc.cash = 50000;
		acc.holdings[botName] = 100;
		lastPriceSeen = Global::REFERENCE_PRICES[botName];
		if (Global::accounts[botname].holdings[botName] > 0) averageEntryPrice = lastPriceSeen;
		else averageEntryPrice = 0.0f;
	}
	else
	{
		bot = static_cast<Strategy>(dist(rng));
		Symbol = Global::SYMBOLS[sym(rng)];

		Global::accounts[botName].cash = 50000;
		Global::accounts[botName].holdings[Symbol] = 100;
		Global::accounts[botName].username = botName;
		botname = botName;
		lastPriceSeen = Global::REFERENCE_PRICES[Symbol];
		if (Global::accounts[botname].holdings[Symbol] > 0) averageEntryPrice = lastPriceSeen;
		else averageEntryPrice = 0.0f;
	}




	reactionSpeed = reaction(rng);
	cooldown = cd(rng);



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

void BotManager::InitBots()
{
	for (int i = 0; i < TotalBots; i++)
	{
		bots[i].InitBot( "bot_" + std::to_string(i));
	}
}

void BotManager::InitMarketMaker()
{
	for (int i = 0; i < 5; i++)
	{
		MarketMakers[i].InitBot(Global::SYMBOLS[i], MarketMaker);
		NoiseTraders[i].InitBot( Global::SYMBOLS[i], NoiseTrader);
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

void BotManager::MomentumStrategy(Bot& bot,
	std::function<void(Order&, OrderBook&)> matchingfunction)
{
	auto& book = Global::books[bot.Symbol];
	auto& house = Global::accounts[bot.botname];

	// --- 1. Gather price history from tradeLog ---
	const auto& log = book.tradeLog;

	static const int FAST_WINDOW = 8;
	static const int SLOW_WINDOW = 24;

	if ((int)log.size() < SLOW_WINDOW) return;  // not enough history yet

	// Fast MA: average of last FAST_WINDOW trade prices
	double fastMA = 0.0;
	for (int i = (int)log.size() - FAST_WINDOW; i < (int)log.size(); i++)
		fastMA += log[i].price;
	fastMA /= FAST_WINDOW;

	// Slow MA: average of last SLOW_WINDOW trade prices
	double slowMA = 0.0;
	for (int i = (int)log.size() - SLOW_WINDOW; i < (int)log.size(); i++)
		slowMA += log[i].price;
	slowMA /= SLOW_WINDOW;

	if (slowMA <= 0) return;

	double signal = (fastMA - slowMA) / slowMA;  // normalised momentum

	// --- 2. Dead zone: ignore weak signals (flat/choppy market) ---
	// Scale threshold by volatility so the dead zone widens in choppy markets
	double baseThreshold = 0.002 * bot.momentumWeight;  // ~0.2% baseline
	double threshold = baseThreshold * (1.0 + book.mmVolatility * 0.5);

	if (std::abs(signal) < threshold) return;  // hovering — sit out

	// --- 3. Volume confirmation ---
	// Compare recent volume (last FAST_WINDOW trades) vs baseline (full window)
	double recentVol = 0.0;
	for (int i = (int)log.size() - FAST_WINDOW; i < (int)log.size(); i++)
		recentVol += log[i].qty;

	double baseVol = 0.0;
	for (int i = (int)log.size() - SLOW_WINDOW; i < (int)log.size(); i++)
		baseVol += log[i].qty;
	baseVol /= (SLOW_WINDOW / FAST_WINDOW);  // normalise to same window size

	// Weak signal without volume backing → skip
	if (recentVol < baseVol * 0.8) return;

	// --- 4. Build order ---
	// Scale qty by risk tolerance and dampen when volatility is high
	double volDampener = 1.0 / (1.0 + book.mmVolatility * 0.3);
	uint32_t qty = static_cast<uint32_t>(15 * bot.riskTolerance * volDampener);
	if (qty < 1) return;

	Order ord;
	ord.orderId = Global::nextOrderId.fetch_add(1);
	ord.username = bot.botname;
	ord.symbol = bot.Symbol;
	ord.ts = std::chrono::steady_clock::now();
	ord.origQty = qty;
	ord.qty = qty;

	if (signal > threshold)  // uptrend — buy into ask
	{
		if (book.asks.empty()) return;
		ord.side = 'B';
		ord.price = book.asks.begin()->first;
		if (house.cash < ord.price * qty) return;
		house.cash -= ord.price * qty;
	}
	else  // downtrend — sell into bid
	{
		if (book.bids.empty()) return;
		ord.side = 'S';
		ord.price = book.bids.begin()->first;
		if (house.holdings[bot.Symbol] < qty) return;
		house.holdings[bot.Symbol] -= qty;
	}

	Global::liveOrders[ord.orderId] = ord;
	house.openOrders[ord.orderId] = ord;

#ifdef _DEBUG
	std::cout << "[MOM] " << bot.botname
		<< " sym=" << bot.Symbol
		<< " side=" << ord.side
		<< " price=" << std::fixed << std::setprecision(4) << ord.price
		<< " qty=" << ord.qty
		<< " signal=" << signal
		<< " fastMA=" << fastMA
		<< " slowMA=" << slowMA << "\n";
#endif
	matchingfunction(ord, book);

	// Refund any unmatched remainder
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

void BotManager::TrendFollowingStrategy(Bot& bot,
	std::function<void(Order&, OrderBook&)> matchingfunction)
{
	auto& book = Global::books[bot.Symbol];
	auto& house = Global::accounts[bot.botname];

	const auto& log = book.tradeLog;

	const int FAST = 20;
	const int SLOW = 60;

	if ((int)log.size() < SLOW) return;

	double fastMA = 0.0;
	for (int i = log.size() - FAST; i < log.size(); i++)
		fastMA += log[i].price;
	fastMA /= FAST;

	double slowMA = 0.0;
	for (int i = log.size() - SLOW; i < log.size(); i++)
		slowMA += log[i].price;
	slowMA /= SLOW;

	if (slowMA <= 0) return;

	double signal = (fastMA - slowMA) / slowMA;

	// Trend bot needs stronger confirmation
	double threshold = 0.003 * (1.0 + book.mmVolatility);

	if (std::abs(signal) < threshold) return;

	uint32_t qty = static_cast<uint32_t>(20 * bot.riskTolerance);
	if (qty < 1) return;

	Order ord;
	ord.orderId = Global::nextOrderId.fetch_add(1);
	ord.username = bot.botname;
	ord.symbol = bot.Symbol;
	ord.ts = std::chrono::steady_clock::now();
	ord.origQty = qty;
	ord.qty = qty;

	if (signal > 0) // uptrend → buy
	{
		if (book.asks.empty()) return;
		ord.side = 'B';
		ord.price = book.asks.begin()->first;

		if (house.cash < ord.price * qty) return;
		house.cash -= ord.price * qty;
	}
	else // downtrend → sell
	{
		if (book.bids.empty()) return;
		ord.side = 'S';
		ord.price = book.bids.begin()->first;

		if (house.holdings[bot.Symbol] < qty) return;
		house.holdings[bot.Symbol] -= qty;
	}

	Global::liveOrders[ord.orderId] = ord;
	house.openOrders[ord.orderId] = ord;
#ifdef _DEBUG
	std::cout << "[TREND] " << bot.botname
		<< " sym=" << bot.Symbol
		<< " side=" << ord.side
		<< " price=" << ord.price
		<< " signal=" << signal << "\n";
#endif
	matchingfunction(ord, book);

	if (ord.qty > 0)
	{
		CancelOrder(bot);
	}
}

void BotManager::MeanReversionStrategy(Bot& bot,
	std::function<void(Order&, OrderBook&)> matchingfunction)
{
	auto& book = Global::books[bot.Symbol];
	auto& house = Global::accounts[bot.botname];

	double Mean = 0.0;
	int const& MeanWindow = 20;
	auto& Log = book.tradeLog;
	
	if (Log.size() < MeanWindow) return;

	for (int i = Log.size() - MeanWindow; i < Log.size(); i++)
	{
		Mean += Log[i].price;
	
	}
	Mean /= MeanWindow;

	if (Mean <= 0) return;

	double deviation = (book.lastPrice - Mean);

	// Always update reference so it doesn't get permanently stale
	double standardDeviation = 0.0;

	for (int i = Log.size() - MeanWindow; i < Log.size(); i++)
	{
		double diff = Log[i].price - Mean;
		standardDeviation += diff * diff;
	}

	standardDeviation /= MeanWindow;
	standardDeviation = std::sqrt(standardDeviation);

	if (standardDeviation == 0.0) return;

	uint32_t qty = 10;
	Order ord;
	ord.orderId = Global::nextOrderId.fetch_add(1);
	ord.username = bot.botname;
	ord.symbol = bot.Symbol;
	ord.ts = std::chrono::steady_clock::now();
	ord.origQty = qty;
	ord.qty = qty;

	double z_score = deviation / standardDeviation;
	// In MeanReversionStrategy, swap the conditions:
if (z_score > 1.5 && !book.bids.empty())
{
    // Price rose above reference — sell into bid
    ord.side = 'S';
    ord.price = book.bids.begin()->first;
    if (house.holdings[bot.Symbol] < qty) return;
    house.holdings[bot.Symbol] -= qty;
}
else if (z_score < -1.5 && !book.asks.empty())
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
#ifdef _DEBUG
	std::cout << "[MR] " << bot.botname
		<< " sym=" << bot.Symbol
		<< " side=" << ord.side
		<< " price=" << std::fixed << std::setprecision(4) << ord.price
		<< " qty=" << ord.qty
		<< " deviation=" << deviation << "\n";
#endif
	matchingfunction(ord, book);

	// Update reference to MM midprice so bot tracks the walk
	bot.lastPriceSeen = book.lastPrice;

	// Refund unmatched remainder
	CancelOrder(bot);
}
void BotManager::NoiseTradingStrategy(
	Bot& bot,
	std::function<void(Order&, OrderBook&)> matchingfunction)
{
	auto& book = Global::books[bot.Symbol];
	auto& house = Global::accounts[bot.botname];

	if (book.bids.empty() || book.asks.empty()) return;
	std::uniform_int_distribution<int> rollc(0, 99);
	std::uniform_int_distribution<int> qty(1, 10);
	std::uniform_int_distribution<int> buy(0, 1);
	// Only act sometimes
	int roll = rollc(rng);
	if (roll >= 25) return; // 25% chance to trade this tick

	double bestBid = book.bids.begin()->first;
	double bestAsk = book.asks.begin()->first;

	Order ord;
	ord.orderId = Global::nextOrderId.fetch_add(1);
	ord.username = bot.botname;
	ord.symbol = bot.Symbol;
	ord.ts = std::chrono::steady_clock::now();
	ord.origQty = qty(rng); // qty 1 to 10
	ord.qty = ord.origQty;

	bool buySide = buy(rng);

	if (buySide)
	{
		ord.side = 'B';
		ord.price = bestAsk; // aggressive buy, hits ask

		double cost = ord.price * ord.qty;
		if (house.cash < cost) return;

		house.cash -= cost;
	}
	else
	{
		ord.side = 'S';
		ord.price = bestBid; // aggressive sell, hits bid

		if (house.holdings[bot.Symbol] < ord.qty) return;

		house.holdings[bot.Symbol] -= ord.qty;
	}

	Global::liveOrders[ord.orderId] = ord;
	house.openOrders[ord.orderId] = ord;

	matchingfunction(ord, book);
#ifdef _DEBUG
	std::cout << "[NT] " << bot.botname
		<< " sym=" << bot.Symbol
		<< " side=" << ord.side
		<< " price=" << std::fixed << std::setprecision(4) << ord.price
		<< " qty=" << ord.qty;
#endif
	// Cancel any leftover unmatched qty and refund
	if (ord.qty > 0)
	{
		if (ord.side == 'B')
		{
			house.cash += ord.price * ord.qty;
			auto bidIt = book.bids.find(ord.price);
			if (bidIt != book.bids.end())
			{
				bidIt->second.erase(ord.orderId);
				if (bidIt->second.empty()) book.bids.erase(bidIt);
			}
		}
		else
		{
			house.holdings[bot.Symbol] += ord.qty;
			auto askIt = book.asks.find(ord.price);
			if (askIt != book.asks.end())
			{
				askIt->second.erase(ord.orderId);
				if (askIt->second.empty()) book.asks.erase(askIt);
			}
		}


		Global::liveOrders.erase(ord.orderId);
		house.openOrders.erase(ord.orderId);
	}
	else
	{
		Global::liveOrders.erase(ord.orderId);
		house.openOrders.erase(ord.orderId);
	}
}
void BotManager::HerdBehaviorStrategy(Bot& bot,
	std::function<void(Order&, OrderBook&)> matchingfunction)
{
	auto& book = Global::books[bot.Symbol];
	auto& house = Global::accounts[bot.botname];
	auto& Log = book.tradeLog;

	int window = 10;
	if ((int)Log.size() < window) return;

	double buyPressure = 0.0;
	double sellPressure = 0.0;

	for (int i = Log.size() - window + 1; i < Log.size(); i++)
	{
		if (Log[i].price > Log[i - 1].price)
			buyPressure += Log[i].qty;
		else
			sellPressure += Log[i].qty;
	}

	double total = buyPressure + sellPressure;
	if (total == 0) return;

	double herdSignal = (buyPressure - sellPressure) / total;

	double threshold = 0.3;

	uint32_t qty = 10;

	Order ord;
	ord.orderId = Global::nextOrderId.fetch_add(1);
	ord.username = bot.botname;
	ord.symbol = bot.Symbol;
	ord.ts = std::chrono::steady_clock::now();
	ord.origQty = qty;
	ord.qty = qty;

	if (herdSignal > threshold && !book.asks.empty())
	{
		ord.side = 'B';
		ord.price = book.asks.begin()->first;

		if (house.cash < ord.price * qty) return;
		house.cash -= ord.price * qty;
	}
	else if (herdSignal < -threshold && !book.bids.empty())
	{
		ord.side = 'S';
		ord.price = book.bids.begin()->first;

		if (house.holdings[bot.Symbol] < qty) return;
		house.holdings[bot.Symbol] -= qty;
	}
	else return;

	Global::liveOrders[ord.orderId] = ord;
	house.openOrders[ord.orderId] = ord;
#ifdef _DEBUG
	std::cout << "[HERD] " << bot.botname
		<< " sym=" << bot.Symbol
		<< " signal=" << herdSignal << "\n";
#endif
	matchingfunction(ord, book);

	if (ord.qty > 0)
		CancelOrder(bot);
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

			MeanReversionStrategy(bots[i], matchingfunction);
			break;

		case Momentum:
			MomentumStrategy(bots[i],matchingfunction);
			break;

		case Trend_Following:
			TrendFollowingStrategy(bots[i], matchingfunction);
			break;
		case HerdBehavior:
			HerdBehaviorStrategy(bots[i], matchingfunction);
			break;
		case PanicSelling:
			break;
		}
	}
}

void BotManager::ProcessMarketMaker(std::function<void(Order& ord, OrderBook& book)> matchingfunction)
{
	for (int i = 0; i < 5; i++)
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
#ifdef _DEBUG
		std::cout << "[MM] " << MarketMakers[i].botname
			<< " sym=" << orders[0].symbol  // will print empty string if bug is present
			<< " bid=" << orders[0].price
			<< " ask=" << orders[1].price << "\n";
#endif

		PlaceOrder(MarketMakers[i], orders[1], 'S');
#ifdef _DEBUG
		std::cout << "[MM] " << MarketMakers[i].botname
			<< " sym=" << orders[0].symbol  // will print empty string if bug is present
			<< " bid=" << orders[0].price
			<< " ask=" << orders[1].price << "\n";
		matchingfunction(orders[0], Global::books[MarketMakers[i].Symbol]);
		matchingfunction(orders[1], Global::books[MarketMakers[i].Symbol]);
#endif

		NoiseTradingStrategy(NoiseTraders[i], matchingfunction);
	}
}

