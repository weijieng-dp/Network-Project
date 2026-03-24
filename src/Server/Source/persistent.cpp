#include "persistence.h"
#include <string>
#include <iostream>
#include <mutex>
#include <fstream>

#include "global.h"
#include "utils.h"

/*--------------------------------------------------------------------------
 * Persistence
 *--------------------------------------------------------------------------*/

void persistData() {
    std::string logMsg;
    {
        std::lock_guard<std::mutex> lk(Global::exMtx);
        // --- accounts.dat: cash + holdings ---
        std::ofstream fa(Global::persistPath + "\\accounts.dat", std::ios::trunc);
        for (auto& [u, acc] : Global::accounts) {
            fa << "A " << u << " " << std::fixed << std::setprecision(6) << acc.cash << " " << acc.passwordHash << "\n";
            for (auto& [sym, qty] : acc.holdings) if (qty > 0) fa << "H " << sym << " " << qty << "\n";
        }
        // --- trades.dat: trade log ---
        std::ofstream ft(Global::persistPath + "\\trades.dat", std::ios::trunc);
        for (auto& tr : Global::allTrades)
            ft << tr.tradeId << " " << tr.symbol << " " << tr.qty << " "
            << std::fixed << std::setprecision(6) << tr.price << " "
            << tr.buyUser << " " << tr.sellUser << " " << tr.datetime << "\n";
        // --- orders.dat: all resting orders in the book ---
        std::ofstream fo(Global::persistPath + "\\orders.dat", std::ios::trunc);
        for (auto& [oid, ord] : Global::liveOrders) {
            fo << "O " << oid << " " << ord.username << " " << ord.side << " "
                << ord.symbol << " " << ord.qty << " " << ord.origQty << " "
                << std::fixed << std::setprecision(6) << ord.price << "\n";
        }
        // --- history.dat: price history for charting ---
        std::ofstream fh(Global::persistPath + "\\history.dat", std::ios::trunc);
        for (auto& [sym, book] : Global::books) {
            for (auto& tp : book.tradeLog)
                fh << sym << " " << std::fixed << std::setprecision(6) << tp.price << " " << tp.qty << " " << tp.datetime << "\n";
        }
        logMsg = "[" + nowString() + "] Persisted " + std::to_string(Global::accounts.size()) + " accounts, "
            + std::to_string(Global::allTrades.size()) + " trades, "
            + std::to_string(Global::liveOrders.size()) + " orders.";
    }
    // Print AFTER releasing Global::exMtx to avoid double-lock with Global::printMtx
    std::lock_guard<std::mutex> plk(Global::printMtx);
    std::cout << logMsg << "\n";
}

void loadData() {
    // --- Load accounts ---
    std::ifstream fa(Global::persistPath + "\\accounts.dat");
    if (fa.is_open()) {
        std::string line, curUser;
        while (std::getline(fa, line)) {
            if (line.empty()) continue;
            std::istringstream ss(line); char tag; ss >> tag;
            if (tag == 'A') { std::string u, ph; double c; ss >> u >> c >> ph; Global::accounts[u].username = u; Global::accounts[u].cash = c; Global::accounts[u].passwordHash = ph; curUser = u; }
            else if (tag == 'H' && !curUser.empty()) { std::string sym; uint32_t qty; ss >> sym >> qty; Global::accounts[curUser].holdings[sym] = qty; }
        }
        std::cout << "Loaded " << Global::accounts.size() << " accounts.\n";
    }
    // --- Load trades ---
    std::ifstream ft(Global::persistPath + "\\trades.dat");
    if (ft.is_open()) {
        Trade tr;
        while (ft >> tr.tradeId >> tr.symbol >> tr.qty >> tr.price >> tr.buyUser >> tr.sellUser >> tr.datetime) {
            Global::allTrades.push_back(tr);
            Global::accounts[tr.buyUser].trades.push_back(tr);
            Global::accounts[tr.sellUser].trades.push_back(tr);
            if (tr.tradeId >= Global::nextTradeId.load()) Global::nextTradeId.store(tr.tradeId + 1);
        }
        std::cout << "Loaded " << Global::allTrades.size() << " trades.\n";
    }
    // --- Load resting orders (rebuild order book) ---
    std::ifstream fo(Global::persistPath + "\\orders.dat");
    if (fo.is_open()) {
        std::string line;
        uint64_t maxOid = Global::nextOrderId.load();
        while (std::getline(fo, line)) {
            if (line.empty()) continue;
            std::istringstream ss(line); char tag; ss >> tag;
            if (tag != 'O') continue;
            uint64_t oid = 0; std::string user; char side; std::string sym;
            uint32_t qty = 0, origQty = 0; double price = 0;
            ss >> oid >> user >> side >> sym >> qty >> origQty >> price;
            if (oid == 0 || user.empty() || sym.empty() || qty == 0) continue;

            Order ord;
            ord.orderId = oid; ord.username = user; ord.side = side;
            ord.symbol = sym; ord.qty = qty; ord.origQty = origQty; ord.price = price;
            ord.ts = std::chrono::steady_clock::now();

            // Insert into the order book
            if (side == 'B') Global::books[sym].bids[price][oid] = ord;
            else          Global::books[sym].asks[price][oid] = ord;
            Global::liveOrders[oid] = ord;
            Global::accounts[user].openOrders[oid] = ord;

            if (oid >= maxOid) maxOid = oid + 1;
        }
        Global::nextOrderId.store(maxOid);
        std::cout << "Loaded " << Global::liveOrders.size() << " resting orders.\n";
    }
    // --- Load price history for charting ---
    std::ifstream fh(Global::persistPath + "\\history.dat");
    if (fh.is_open()) {
        std::string sym, dt; double price; uint32_t qty;
        size_t hcount = 0;
        while (fh >> sym >> price >> qty >> dt) {
            Global::books[sym].tradeLog.push_back({ price,qty,dt });
            ++hcount;
        }
        std::cout << "Loaded " << hcount << " price history points.\n";
    }
    for (auto& sym : Global::SYMBOLS) Global::books[sym];
}