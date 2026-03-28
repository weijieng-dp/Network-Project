#pragma once
#include <array>
#include "types.h"



constexpr int TotalBots = 4;

enum Strategy
{
    MarketMaker = 0,
    Trend_Following,
    Momentum,
    Mean_Reversion,
    HerdBehavior,
    PanicSelling,

    StrategyCount

};

class Bot
{
public:
    void InitBot(double currentPriceMarket, std::string botName, bool isMarketMaker = false);
    Strategy bot;
    std::string botname;
    std::string Symbol;
    double riskTolerance;
    double reactionSpeed;
    double bias;
    double lastPriceSeen;
    double averageEntryPrice;
    double cooldown;
    double momentumWeight;
    double meanReversionWeight;
    double herdWeight;
};

class BotManager
{
public:
    static BotManager Instance()
    {
        static BotManager instance;
        return instance;
    }

    void InitBots(double currentPriceMarket);
    void InitMarketMaker(double currentPriceMarket);

    std::array<Order,2> MarketMakerStrategy(Bot& bot, double const& bestbid, double const& bestask);
    void MomentumStrategy();
    void TrendFollowingStrategy();
    void MeanReversionStrategy(Bot& bot, std::function<void(Order&, OrderBook&)> matchingfunction);
    void HerdBehaviorStrategy();
    void PanicSellingStrategy();
    void CancelOrder(Bot& bot);
    void PlaceOrder(Bot& bot,Order ord, char side);
    
    void ProcessStrategies(std::function<void(Order& ord, OrderBook& book)>);
    void ProcessMarketMaker(std::function<void(Order& ord, OrderBook& book)>);

    static std::array<Bot, TotalBots> bots; // momentum, mean-reversion
    static std::array<Bot, 1> MarketMakers; // momentum, mean-reversion

private:
};