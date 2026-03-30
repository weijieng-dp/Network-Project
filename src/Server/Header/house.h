#pragma once
#include "types.h"
#include "global.h"
#include "utils.h"

bool postHouseOrder(const std::string& sym, char side, double price, uint32_t qty);
void seedMarketMaker();
void requoteMarketMaker(const std::string& sym);