#include "persistence.h"
#include <string>
#include <iostream>
#include <mutex>
#include <fstream>

#include "global.h"
#include "utils.h"

#include "crypto.h"

/*--------------------------------------------------------------------------
 * Persistence
 *--------------------------------------------------------------------------*/

void writePersistentData() {
    std::vector<uint8_t> salt(16);
    std::random_device rd;
    for (auto& b : salt) b = static_cast<uint8_t>(rd());

    std::string logMsg;
    {
        std::lock_guard<std::mutex> lk(Global::exMtx);

        AESGCMCipher cipher;
        cipher.setKey(Global::password, salt);

        // --- accounts.dat: cash + holdings ---
        std::vector<uint8_t> accountsAad{ 'a', 'c', 'c', 'o', 'u', 'n', 't', 's', '.', 'd', 'a', 't' };
        std::string accountsPlain;
        for (auto& [u, acc] : Global::accounts) {
            accountsPlain += "A " + u + " " + std::to_string(acc.cash) + " " + acc.passwordHash + "\n";
            for (auto& [sym, qty] : acc.holdings) if (qty > 0) accountsPlain += "H " + sym + " " + std::to_string(qty) + "\n";
        }
        std::vector<uint8_t> encryptedAcc{ cipher.encrypt(std::vector<uint8_t>(accountsPlain.begin(), accountsPlain.end()), accountsAad) };
        std::ofstream fa(Global::persistPath + "\\accounts.dat", std::ios::binary | std::ios::trunc);
        fa.write(reinterpret_cast<char*>(salt.data()), salt.size());
        fa.write(reinterpret_cast<char*>(encryptedAcc.data()), encryptedAcc.size());

        // --- trades.dat: trade log ---
        std::vector<uint8_t> tradesAad{ 't', 'r', 'a', 'd', 'e', 's', '.', 'd', 'a', 't' };
        std::string tradesPlain;
        for (auto& tr : Global::allTrades)
            tradesPlain += std::to_string(tr.tradeId) + " " + tr.symbol + " " + std::to_string(tr.qty) + " "
                + std::to_string(tr.price) + " " + tr.buyUser + " " + tr.sellUser + " " + tr.datetime + "\n";
        std::vector<uint8_t> encryptedTrades{ cipher.encrypt(std::vector<uint8_t>(tradesPlain.begin(), tradesPlain.end()), tradesAad) };
        std::ofstream ft(Global::persistPath + "\\trades.dat", std::ios::binary | std::ios::trunc);
        ft.write(reinterpret_cast<char*>(salt.data()), salt.size());
        ft.write(reinterpret_cast<char*>(encryptedTrades.data()), encryptedTrades.size());

        // --- orders.dat: all resting orders in the book ---
        std::vector<uint8_t> ordersAad{ 'o', 'r', 'd', 'e', 'r', 's', '.', 'd', 'a', 't' };
        std::string ordersPlain;
        for (auto& [oid, ord] : Global::liveOrders) {
            ordersPlain += "O " + std::to_string(oid) + " " + ord.username + " " + ord.side + " "
                + ord.symbol + " " + std::to_string(ord.qty) + " " + std::to_string(ord.origQty) + " "
                + std::to_string(ord.price) + "\n";
        }
        std::vector<uint8_t> encryptedOrders{ cipher.encrypt(std::vector<uint8_t>(ordersPlain.begin(), ordersPlain.end()), ordersAad) };
        std::ofstream fo(Global::persistPath + "\\orders.dat", std::ios::binary | std::ios::trunc);
        fo.write(reinterpret_cast<char*>(salt.data()), salt.size());
        fo.write(reinterpret_cast<char*>(encryptedOrders.data()), encryptedOrders.size());

        // --- history.dat: price history for charting ---
        std::vector<uint8_t> historyAad{ 'h', 'i', 's', 't', 'o', 'r', 'y', '.', 'd', 'a', 't' };
        std::string historyPlain;
        for (auto& [sym, book] : Global::books) {
            for (auto& tp : book.tradeLog)
                historyPlain += sym + " " + std::to_string(tp.price) + " " + std::to_string(tp.qty) + 
                " " + tp.datetime + "\n";
        }
        std::vector<uint8_t> encryptedHistory{ cipher.encrypt(std::vector<uint8_t>(historyPlain.begin(), historyPlain.end()), historyAad) };
        std::ofstream fh(Global::persistPath + "\\history.dat", std::ios::binary | std::ios::trunc);
        fh.write(reinterpret_cast<char*>(salt.data()), salt.size());
        fh.write(reinterpret_cast<char*>(encryptedHistory.data()), encryptedHistory.size());


        logMsg = "[" + nowString() + "] Persisted " + std::to_string(Global::accounts.size()) + " accounts, "
            + std::to_string(Global::allTrades.size()) + " trades, "
            + std::to_string(Global::liveOrders.size()) + " orders.";
    }
    // Print AFTER releasing Global::exMtx to avoid double-lock with Global::printMtx
    std::lock_guard<std::mutex> plk(Global::printMtx);
    std::cout << logMsg << "\n";
}

void loadPersistentData() {
    AESGCMCipher cipher;

    // --- Load accounts ---
    std::ifstream fa(Global::persistPath + "\\accounts.dat", std::ios::binary);

    if (fa.is_open()) {
        std::vector<uint8_t> accountsAad{ 'a', 'c', 'c', 'o', 'u', 'n', 't', 's', '.', 'd', 'a', 't' };
        std::vector<uint8_t> salt(16);
        fa.read(reinterpret_cast<char*>(salt.data()), salt.size());

        cipher.setKey(Global::password, salt);

        std::vector<uint8_t> encrypted((std::istreambuf_iterator<char>(fa)), std::istreambuf_iterator<char>());
        std::vector<uint8_t> decrypted{ cipher.decrypt(encrypted, accountsAad) };

        std::string accountsPlain(decrypted.begin(), decrypted.end());
        std::istringstream ss(accountsPlain);
        std::string line, curUser;
        while (std::getline(ss, line)) {
            if (line.empty()) continue;
            char tag;
            std::istringstream ls(line); ls >> tag;

            if (tag == 'A') { 
                std::string u, ph; 
                double c; 

                ls >> u >> c >> ph; 
                
                Global::accounts[u].username = u; 
                Global::accounts[u].cash = c; 
                Global::accounts[u].passwordHash = ph; 
                curUser = u; 
            }
            else if (tag == 'H' && !curUser.empty()) { 
                std::string sym; 
                uint32_t qty; 
                
                ls >> sym >> qty; 
                
                Global::accounts[curUser].holdings[sym] = qty; 
            }
        }
        std::cout << "Loaded " << Global::accounts.size() << " accounts.\n";
    }


    // --- Load trades ---
    std::ifstream ft(Global::persistPath + "\\trades.dat", std::ios::binary);
    if (ft.is_open()) {
        std::vector<uint8_t> tradesAad{ 't', 'r', 'a', 'd', 'e', 's', '.', 'd', 'a', 't' };
        std::vector<uint8_t> salt(16);
        ft.read(reinterpret_cast<char*>(salt.data()), salt.size());
        cipher.setKey(Global::password, salt);
        std::vector<uint8_t> encrypted((std::istreambuf_iterator<char>(ft)), std::istreambuf_iterator<char>());
        std::vector<uint8_t> decrypted{ cipher.decrypt(encrypted, tradesAad) };

        std::string tradesPlain(decrypted.begin(), decrypted.end());
        std::istringstream ss(tradesPlain);

        Trade tr;
        while (ss >> tr.tradeId >> tr.symbol >> tr.qty >> tr.price >> tr.buyUser >> tr.sellUser >> tr.datetime) {

            Global::allTrades.push_back(tr);
            Global::accounts[tr.buyUser].trades.push_back(tr);
            Global::accounts[tr.sellUser].trades.push_back(tr);

            if (tr.tradeId >= Global::nextTradeId.load()) Global::nextTradeId.store(tr.tradeId + 1);
        }

        std::cout << "Loaded " << Global::allTrades.size() << " trades.\n";
    }


    // --- Load resting orders (rebuild order book) ---
    std::ifstream fo(Global::persistPath + "\\orders.dat", std::ios::binary);
    if (fo.is_open()) {
        std::vector<uint8_t> ordersAad{ 'o', 'r', 'd', 'e', 'r', 's', '.', 'd', 'a', 't' };
        std::vector<uint8_t> salt(16);
        fo.read(reinterpret_cast<char*>(salt.data()), salt.size());
        cipher.setKey(Global::password, salt);
        std::vector<uint8_t> encrypted((std::istreambuf_iterator<char>(fo)), std::istreambuf_iterator<char>());
        std::vector<uint8_t> decrypted{ cipher.decrypt(encrypted, ordersAad) };

        std::string ordersPlain(decrypted.begin(), decrypted.end());
        std::istringstream ss(ordersPlain);

        std::string line;
        uint64_t maxOid = Global::nextOrderId.load();

        while (std::getline(ss, line)) {
            if (line.empty()) continue;
            char tag;
            std::istringstream ls(line); ls >> tag;

            if (tag != 'O') continue;

            uint64_t oid = 0; 
            std::string user; 
            char side; 
            std::string sym;
            uint32_t qty = 0, origQty = 0; 
            double price = 0;

            ls >> oid >> user >> side >> sym >> qty >> origQty >> price;

            if (oid == 0 || user.empty() || sym.empty() || qty == 0) continue;

            Order ord;
            ord.orderId = oid; 
            ord.username = user; 
            ord.side = side;
            ord.symbol = sym; 
            ord.qty = qty; 
            ord.origQty = origQty;
            ord.price = price;
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
    std::ifstream fh(Global::persistPath + "\\history.dat", std::ios::binary);

    if (fh.is_open()) {
        std::vector<uint8_t> historyAad{ 'h', 'i', 's', 't', 'o', 'r', 'y', '.', 'd', 'a', 't' };
        std::vector<uint8_t> salt(16);
        fh.read(reinterpret_cast<char*>(salt.data()), salt.size());
        cipher.setKey(Global::password, salt);
        std::vector<uint8_t> encrypted((std::istreambuf_iterator<char>(fh)), std::istreambuf_iterator<char>());
        std::vector<uint8_t> decrypted{ cipher.decrypt(encrypted, historyAad) };

        std::string historyPlain(decrypted.begin(), decrypted.end());
        std::istringstream ss(historyPlain);

        std::string sym, dt; 
        double price; 
        uint32_t qty;
        size_t hcount = 0;

        while (ss >> sym >> price >> qty >> dt) {
            Global::books[sym].tradeLog.push_back({ price,qty,dt });
            ++hcount;
        }
        std::cout << "Loaded " << hcount << " price history points.\n";
    }


    for (auto& sym : Global::SYMBOLS) Global::books[sym];
}