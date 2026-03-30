#pragma once
#include <array>
#include "types.h"



constexpr int TotalBots = 50;

enum Strategy
{
    MarketMaker = 0,
    Mean_Reversion,
    Momentum,
    Trend_Following,
    HerdBehavior,
    PanicSelling,
    NoiseTrader,

    StrategyCount

};

class Bot
{
public:
    void InitBot( std::string botName, Strategy strategy = StrategyCount);
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

    void InitBots();
    void InitMarketMaker();

    std::array<Order,2> MarketMakerStrategy(Bot& bot, double const& bestbid, double const& bestask);
    void MomentumStrategy(Bot& bot, std::function<void(Order&, OrderBook&)> matchingfunction);
    void TrendFollowingStrategy(Bot& bot,
        std::function<void(Order&, OrderBook&)> matchingfunction);
    void MeanReversionStrategy(Bot& bot, std::function<void(Order&, OrderBook&)> matchingfunction);
    void NoiseTradingStrategy(Bot& bot, std::function<void(Order&, OrderBook&)> matchingfunction);
    void HerdBehaviorStrategy(Bot& bot,
        std::function<void(Order&, OrderBook&)> matchingfunction);
    void PanicSellingStrategy(
        Bot& bot,
        std::function<void(Order&, OrderBook&)> matchingfunction);
    void CancelOrder(Bot& bot);
    void PlaceOrder(Bot& bot,Order ord, char side);
    
    void ProcessStrategies(std::function<void(Order& ord, OrderBook& book)>);
    void ProcessMarketMaker(std::function<void(Order& ord, OrderBook& book)>);

    static std::array<Bot, TotalBots> bots; // momentum, mean-reversion
    static std::array<Bot, 5> MarketMakers; // momentum, mean-reversion
    static std::array<Bot, 5> NoiseTraders; // momentum, mean-reversion
    static std::array<Bot, 5> PanicSellers; // momentum, mean-reversion

private:
};