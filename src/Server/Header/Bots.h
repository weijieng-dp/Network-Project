#pragma once
#include <array>
#include "types.h"



int const& TotalBots = 4;

enum Strategy
{
    Mean_Reversion = 0,
    Trend_Following,
    Momentum,
    MarketMaker,
    HerdBehavior,
    PanicSelling,

    StrategyCount

};

class Bot
{
public:
    void InitBot(double currentPriceMarket, std::string botName);
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

    std::array<Order,2> MarketMakerStrategy(Bot& bot, double const& bestbid, double const& bestask);
    void MomentumStrategy();
    void TrendFollowingStrategy();
    void NoiseTraderStrategy();
    void HerdBehaviorStrategy();
    void PanicSellingStrategy();
    void CancelOrder(Bot& bot);
    void PlaceOrder(Bot& bot,Order ord, char side);
    
    void ProcessStrategies(std::function<void(Order& ord, OrderBook& book)>);

    std::array<Bot, TotalBots> bots; // momentum, mean-reversion

private:
};