===========================================================
Project Guide - 
===========================================================

This guide explains how to install CMake on a target machine and 
regenerate the Visual Studio 2022 solution.

-----------------------------------------------------------
1. Install CMake
-----------------------------------------------------------
• Download CMake from the official website:
  https://cmake.org/download

• Choose the appropriate installer for your platform:
  - Windows: "cmake-x.x.x-windows-x86_64.msi"

• IMPORTANT: The installed CMake version must be 3.5 or higher.
  You can check your version by running:
     cmake --version


=== Regenerating Visual Studio 2022 Solution with CMake ===

1. Open command prompt

2. Navigate to the project root folder (where CMakeLists.txt is located):
   cd path\to\the\project

3. Create a new build directory (if not already present):
   mkdir build
   cd build

4. Run the CMake configure command for Visual Studio 2022 (x64):
   cmake -G "Visual Studio 17 2022" -A x64 ..

5. Once this finishes, a Visual Studio solution (.sln) file will be created
   inside the "build" directory. Open it with Visual Studio 2022:
   build\GAM200-ENGINE.sln

=== Alternative ===
Run the provided "run.bat" script. It will automatically:
- Create the build folder,
- Run the CMake configure command,
- Generate the Visual Studio 2022 solution.

Usage:
   double-click run.bat


===Locate The Release build===
the Release build for both client and server is located at 
-build/src/Server/Release 
-build/src/Client/Release 


===========================================================
About This Project
===========================================================

**Trading Platform** is a client/server simulation of a stock exchange,
written in C++20 for Windows.  A single `Server` process runs a live
limit-order-book exchange for five symbols; any number of `Client`
processes connect over the network to log in, watch prices, and trade
against the book.

What makes the market move is the server itself: it runs ~65 simulated
trading bots (market makers, momentum, mean-reversion, trend-following,
herd-behaviour, noise traders and panic sellers) that continuously quote
and cross orders, plus a *Crisis Manager* that periodically throws the
market into a SHOCK, PANIC, CRAZE or RANDOM regime.  Prices therefore
trend, spike and crash on their own, and the human traders are just more
participants in that book.

Symbols and reference prices:

    AAPL  $180    GOOGL $170    MSFT  $420    TSLA  $250    AMZN  $185

Every new account starts with **$100,000** cash and no holdings.

-----------------------------------------------------------
Feature summary
-----------------------------------------------------------
• Price/time-priority matching engine with a real order book
  (bids highest-first, asks lowest-first, partial fills supported).
• Limit orders, market orders, order cancellation.
• Conditional orders: stop-loss and take-profit, evaluated after every
  trade in the symbol.
• Per-position P&L: average cost basis, unrealised P&L and realised P&L.
• Live candlestick charts (ImGui + ImPlot) with an SMA-10 overlay,
  built server-side from 15-second OHLC buckets.
• Bot-driven market simulation + random market crises.
• Encrypted disk persistence (AES-GCM) of accounts, trades, resting
  orders and price history, flushed every 5 seconds.
• Diffie-Hellman key exchange on login so the password is not sent in
  the clear.

-----------------------------------------------------------
Architecture
-----------------------------------------------------------
Two transports are used, as in the assignment brief:

    TCP  - all control traffic: login, orders, cancels, queries,
           trade executions and server messages.
           Frame layout:  CmdID(1) + PayloadLen(2) + Payload

    UDP  - one-way market-data broadcast from server to every
           subscribed client after each trade.
           Datagram layout: Seq(4) + CmdID(1) + PayloadLen(2) + Payload
           Clients drop datagrams whose sequence number is not newer
           than the last one seen (out-of-order / duplicate handling).

Server threads: an accept loop feeding a 24-slot thread pool (one
session per client), a UDP broadcast thread, a persistence thread, a
bot-simulation thread (ticks every 500 ms), and the main thread which
drives the crisis manager and the ImGui window.

Client threads: the main thread renders the GUI, `T_TcpRcv` reads server
responses, `T_UdpRcv` reads market-data broadcasts.

Source layout:

    src/Shared/    utils.h      protocol IDs, (de)serialisation, framing
                   crypto.h     Diffie-Hellman + AES-GCM helpers
                   candleplot.* ImPlot candlestick renderer
    src/Server/    server.cpp   matching engine, protocol handlers, main
                   Bots.cpp     bot strategies
                   crisis.cpp   market-crisis state machine
                   display.cpp  server ImGui window
                   persistent.cpp  encrypted save/load
                   global.cpp   shared exchange state
    src/Client/    client.cpp   GUI, command parser, receive threads


===========================================================
Using The Application
===========================================================

Build first (see the CMake instructions above, or just run `run.bat`).
The executables land in:

    build\src\Server\Release\Server.exe
    build\src\Client\Release\Client.exe

-----------------------------------------------------------
1. Starting the server
-----------------------------------------------------------
Run `Server.exe`.  A window opens asking for:

    TCP Port         port clients connect to        (1 - 65535)
    UDP Port         port market data is sent from  (1 - 65535)
    Storage Folder   where the .dat save files go   (optional)

Enter both ports and press **Confirm**.  Any free high port works, e.g.
TCP 9000 / UDP 9001.

Tip: give the Storage Folder an explicit path (e.g. `C:\exchange_data`).
If left blank the save files are written to the root of the current
drive, which is rarely what you want.  The folder is created if it does
not exist, and existing accounts, orders, trades and price history are
loaded from it on startup.

Once confirmed, the server console prints its IP address and ports —
this is what you type into the client — and the server window shows:

  • **Active Symbols**  – live candlestick chart, one tab per symbol.
  • **Crisis Panel**    – current market regime, and an ADMIN PANEL
                          section whose buttons force NONE / SHOCK /
                          PANIC / CRAZE / RANDOM immediately.
  • **Server Information** – IP address and port numbers.

The panels are dockable; drag their title bars to rearrange.
Closing the server window shuts the exchange down cleanly and writes a
final snapshot to disk.

-----------------------------------------------------------
2. Starting a client
-----------------------------------------------------------
Run `Client.exe`.  It first asks four questions in the console:

    Server IP Address:        e.g. 127.0.0.1  (or the IP the server printed)
    Server TCP Port Number:   the server's TCP port
    Server UDP Port Number:   the server's UDP port
    Local UDP Port Number:    a free port on THIS machine for receiving
                              broadcasts - give each client on the same
                              machine a different one (e.g. 9100, 9101)

The trading window then opens.  Its five panels are:

  • **Chart**        – candlesticks + SMA-10 for the selected symbol.
                       Click a tab to switch symbol; scroll the mouse
                       wheel to zoom between 10 and 120 candles.
  • **Market Data**  – bid / ask / last / volume for all five symbols,
                       updated live from UDP broadcasts.
  • **Account**      – cash, holdings with average cost and P&L,
                       portfolio value, and your open orders.
  • **Log**          – server messages, order acknowledgements, fills
                       and errors.
  • **Command**      – the text box where you type commands.

Panels are dockable and the layout is remembered in `imgui.ini`.

-----------------------------------------------------------
3. Logging in
-----------------------------------------------------------
Type into the Command box:

    /login <username> <password>

There is no separate registration step: the first time a username is
seen the account is created with that password and $100,000 cash, and
afterwards the same password is required.  On the first login the client
performs a Diffie-Hellman handshake and sends the password encrypted
with the resulting session key; the log will show "Secure channel
established".

A username can only be logged in from one client at a time, and the
reserved account name `EXCHANGE` cannot be used.

-----------------------------------------------------------
4. Command reference
-----------------------------------------------------------
Session
    /login <user> <pass>              log in or create an account
    /logout                           log out (stay connected)
    /help                             print the command list
    /q  |  /quit  |  /exit            quit the client

Trading
    /buy  <SYM> <qty> <price>         limit buy
    /sell <SYM> <qty> <price>         limit sell
    /mbuy  <SYM> <qty>                market buy  (takes the best asks)
    /msell <SYM> <qty>                market sell (hits the best bids)
    /cancel <orderID>                 cancel one of your resting orders

Conditional orders
    /stoploss   <SYM> <qty> <price>   sell if price falls to <price>
    /takeprofit <SYM> <qty> <price>   sell if price rises to <price>
    /stops                            list your conditional orders
    /cancelstop <id>                  cancel one of them

Information
    /market <SYM>                     one-off quote for a symbol
    /account                          refresh cash and holdings
    /portfolio                        positions table with P&L
    /orders                           refresh your open orders
    /trades                           your last 50 fills
    /graph [SYM]                      reload the chart (optionally
                                      switching symbol)

Note: `/crisis` exists in the client but the server side is currently
commented out, so it just replies "Unknown command."  Use the server
window's ADMIN PANEL to force a crisis instead.

-----------------------------------------------------------
5. How trading works
-----------------------------------------------------------
• **Reservation.** When you place a buy, `price × qty` is deducted from
  your cash immediately and held while the order rests; if it fills
  cheaper than your limit, the difference is refunded.  When you place a
  sell, the shares are removed from your holdings up front.  Cancelling
  refunds whatever is left.

• **Matching.** Your order is matched against the opposite side of the
  book by price then time.  Anything unfilled rests in the book as a
  live order and keeps working until it fills or you cancel it.

• **Market orders.** `/mbuy` and `/msell` sweep the book at the best
  available prices.  They are rejected if that side of the book is
  empty.

• **Stops.** A stop-loss trigger must be *below* the current price and a
  take-profit trigger *above* it, otherwise the order is rejected.  Both
  are checked after every trade in that symbol and fire as market sells;
  you get a server message when one triggers.

• **P&L.** Average cost is a weighted average across your buys.
  Unrealised P&L is `(last price - avg cost) × qty`; realised P&L is
  booked per symbol when you sell.

-----------------------------------------------------------
6. Quick walkthrough (single machine)
-----------------------------------------------------------
1. Start `Server.exe`, enter TCP 9000 / UDP 9001 and a storage folder,
   press Confirm.
2. Start `Client.exe`, answer: `127.0.0.1`, `9000`, `9001`, `9100`.
3. In the Command box: `/login alice hunter2`
4. Watch the Market Data panel until prices start moving (the bots quote
   within a second or two).
5. `/mbuy AAPL 10` — buys 10 AAPL at the best ask.
6. `/portfolio` — see your position, cost basis and P&L.
7. `/takeprofit AAPL 10 185` — auto-sell if AAPL reaches $185.
8. Press SHOCK or PANIC in the server's Crisis Panel and watch the
   chart and your P&L react.
9. `/q` to quit.  Your account is on disk; log in again later with the
   same username and password to pick up where you left off.

-----------------------------------------------------------
7. Troubleshooting
-----------------------------------------------------------
"connect failed."
    The server is not running, the IP/TCP port is wrong, or Windows
    Firewall is blocking the port.  Allow both executables through the
    firewall when prompted.

"UDP bind failed."
    Another program (often a second client) already holds that Local UDP
    Port.  Pick a different one.

Market Data shows "--" for every symbol.
    The UDP subscription did not get through.  Check the Local UDP Port
    is free and not blocked; `/market AAPL` fetches a quote over TCP as
    a fallback check.

"User already logged in from another session."
    That username is open in another client.  Use `/logout` there, or
    log in as a different user.

Chart says "No data yet."
    No trades have printed for that symbol yet.  Give the bots a few
    seconds, or trade the symbol yourself.

Nothing is restored after a restart.
    The Storage Folder differs from the one used last time, or was left
    blank.  Use the same explicit path every run.

-----------------------------------------------------------
Note on security
-----------------------------------------------------------
This is a coursework simulation.  The Diffie-Hellman parameters are
64-bit, the login password hash is a salted djb2, and the disk
encryption key is a hard-coded passphrase — enough to demonstrate the
concepts, not suitable for anything real.
