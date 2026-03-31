#include "display.h"
#include "global.h"
#include "crisis.h"
#include "candleplot.h"
#include <iostream>
#include <algorithm>
#include <numeric>

GLFWwindow* Display::window;
int         Display::width;
int         Display::height;

void Display::Init() {
    
    std::string serverName{ "Server" };

    width = 1600;
    height = 900;

    // Initialize OpenGL
    glfwInit();

    // OpenGL version 4.6
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    const GLFWvidmode* mode = glfwGetVideoMode(glfwGetPrimaryMonitor());

    window = glfwCreateWindow(width, height, serverName.c_str(), NULL, NULL);
    int posX{ (mode->width - width) / 2 },      // calculate the x position to center the application
        posY{ (mode->height - height) / 2 };    // calculate the y position to center the application
    glfwSetWindowPos(window, posX, posY);

    glfwMakeContextCurrent(window);

    glewExperimental = GL_TRUE;
    glewInit();

    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);

    glfwSwapInterval(0);


    IMGUI_CHECKVERSION();

    ImGui::CreateContext();
    ImPlot::CreateContext();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init();

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(width, height);

    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;         // Enable Docking

    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;

    // Base Colors
    ImVec4 bgColor =            ImVec4(0.10f, 0.105f, 0.11f, 1.00f);
    ImVec4 lightBgColor =       ImVec4(0.15f, 0.16f, 0.17f, 1.00f);
    ImVec4 panelColor =         ImVec4(0.17f, 0.18f, 0.19f, 1.00f);
    ImVec4 panelHoverColor =    ImVec4(0.25f, 0.35f, 0.45f, 1.00f);
    ImVec4 panelActiveColor =   ImVec4(0.20f, 0.30f, 0.40f, 1.00f);
    ImVec4 textColor =          ImVec4(0.86f, 0.87f, 0.88f, 1.00f);
    ImVec4 textDisabledColor =  ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
    ImVec4 borderColor =        ImVec4(0.14f, 0.16f, 0.18f, 1.00f);

    // Text
    colors[ImGuiCol_Text] = textColor;
    colors[ImGuiCol_TextDisabled] = textDisabledColor;

    // Windows
    colors[ImGuiCol_WindowBg] = bgColor;
    colors[ImGuiCol_ChildBg] = bgColor;
    colors[ImGuiCol_PopupBg] = bgColor;
    colors[ImGuiCol_Border] = borderColor;
    colors[ImGuiCol_BorderShadow] = borderColor;

    // Headers
    colors[ImGuiCol_Header] = panelColor;
    colors[ImGuiCol_HeaderHovered] = panelHoverColor;
    colors[ImGuiCol_HeaderActive] = panelActiveColor;

    // Buttons
    colors[ImGuiCol_Button] = panelColor;
    colors[ImGuiCol_ButtonHovered] = panelHoverColor;
    colors[ImGuiCol_ButtonActive] = panelActiveColor;

    // Frame BG
    colors[ImGuiCol_FrameBg] = lightBgColor;
    colors[ImGuiCol_FrameBgHovered] = panelHoverColor;
    colors[ImGuiCol_FrameBgActive] = panelActiveColor;

    // Tabs
    colors[ImGuiCol_Tab] = panelColor;
    colors[ImGuiCol_TabHovered] = panelHoverColor;
    colors[ImGuiCol_TabActive] = panelActiveColor;
    colors[ImGuiCol_TabUnfocused] = panelColor;
    colors[ImGuiCol_TabUnfocusedActive] = panelHoverColor;

    // Title
    colors[ImGuiCol_TitleBg] = bgColor;
    colors[ImGuiCol_TitleBgActive] = bgColor;
    colors[ImGuiCol_TitleBgCollapsed] = bgColor;

    // Scrollbar
    colors[ImGuiCol_ScrollbarBg] = bgColor;
    colors[ImGuiCol_ScrollbarGrab] = panelColor;
    colors[ImGuiCol_ScrollbarGrabHovered] = panelHoverColor;
    colors[ImGuiCol_ScrollbarGrabActive] = panelActiveColor;

    // Checkmark
    colors[ImGuiCol_CheckMark] = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);

    // Slider
    colors[ImGuiCol_SliderGrab] = panelHoverColor;
    colors[ImGuiCol_SliderGrabActive] = panelActiveColor;

    // Resize Grip
    colors[ImGuiCol_ResizeGrip] = panelColor;
    colors[ImGuiCol_ResizeGripHovered] = panelHoverColor;
    colors[ImGuiCol_ResizeGripActive] = panelActiveColor;

    // Separator
    colors[ImGuiCol_Separator] = borderColor;
    colors[ImGuiCol_SeparatorHovered] = panelHoverColor;
    colors[ImGuiCol_SeparatorActive] = panelActiveColor;

    // Plot
    colors[ImGuiCol_PlotLines] = textColor;
    colors[ImGuiCol_PlotLinesHovered] = panelActiveColor;
    colors[ImGuiCol_PlotHistogram] = textColor;
    colors[ImGuiCol_PlotHistogramHovered] = panelActiveColor;

    // Text Selected BG
    colors[ImGuiCol_TextSelectedBg] = panelActiveColor;

    // Modal Window Dim Bg
    colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.10f, 0.105f, 0.11f, 0.5f);

    // Tables
    colors[ImGuiCol_TableHeaderBg] = panelColor;
    colors[ImGuiCol_TableBorderStrong] = borderColor;
    colors[ImGuiCol_TableBorderLight] = borderColor;
    colors[ImGuiCol_TableRowBg] = bgColor;
    colors[ImGuiCol_TableRowBgAlt] = lightBgColor;

    // Styles
    style.FrameBorderSize = 1.0f;
    style.FrameRounding = 2.0f;
    style.WindowBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.ScrollbarSize = 12.0f;
    style.ScrollbarRounding = 2.0f;
    style.GrabMinSize = 7.0f;
    style.GrabRounding = 2.0f;
    style.TabBorderSize = 1.0f;
    style.TabRounding = 2.0f;

    // Reduced Padding and Spacing
    style.WindowPadding =       ImVec2(5.0f, 5.0f);
    style.FramePadding =        ImVec2(4.0f, 3.0f);
    style.ItemSpacing =         ImVec2(6.0f, 4.0f);
    style.ItemInnerSpacing =    ImVec2(4.0f, 4.0f);

}

void Display::InitPorts() {

    // Initialize port values. Is a "blocking" call

    while ((Global::udpPort == 0 || Global::tcpPort == 0) && Global::running.load()) {
        // Starting the frame
        {
            glViewport(0, 0, 1600, 900);
            glClearDepthf(0.f);
            glClearColor(0.f, 0.f, 0.f, 0.f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

            if (glfwWindowShouldClose(window)) Global::running.exchange(false);

            ImGui_ImplGlfw_NewFrame();
            ImGui_ImplOpenGL3_NewFrame();
        }

        ImGui::NewFrame();

        bool open{ true };
        static std::string buf(1024, '\0');
        ImGui::SetNextWindowSize({ 400.f,200.f }, ImGuiCond_Once);
        ImGui::SetNextWindowPos({ width / 2 - 200.f,height / 2 - 100.f }, ImGuiCond_Once);
        if (ImGui::Begin("Enter UDP and TCP port numbers:", &open, ImGuiWindowFlags_NoCollapse)) {

            static int udpPort{ 0 };
            static int tcpPort{ 0 };

            ImGui::Text("TCP Port:      ");
            ImGui::SameLine();

            ImGui::PushID(0);
            ImGui::InputInt("", &tcpPort);
            ImGui::PopID();

            ImGui::Text("UDP Port:      ");
            ImGui::SameLine();

            ImGui::PushID(1);
            ImGui::InputInt("", &udpPort);
            ImGui::PopID();

            ImGui::Text("Storage Folder:");
            ImGui::SameLine();

            ImGui::PushID(2);
            ImGui::InputText("", buf.data(), 1024);
            ImGui::PopID();
            ImGui::Text("(optional)");

            ImGui::NewLine();
            ImGui::NewLine();

            if (ImGui::Button("   Confirm   ")) {
                
                if (tcpPort > 0 && tcpPort < 65536 &&
                    udpPort > 0 && udpPort < 65536) {
                    Global::udpPort = static_cast<uint16_t>(udpPort);
                    Global::tcpPort = static_cast<uint16_t>(tcpPort);
                    Global::persistPath = buf.c_str();
                }
            }

            if (tcpPort == 0) {
                ImGui::Text("Please key in a value for TCP");
            }
            if (udpPort == 0) {
                ImGui::Text("Please key in a value for UDP");
            }
            if (tcpPort > 65535) {
                ImGui::Text("Please key in a value less than 65536 for the TCP port");
            }
            if (udpPort > 65535) {
                ImGui::Text("Please key in a value less than 65536 for the TCP port");
            }
        }

        ImGui::End();


        // RENDERING THE FRAME
        {
            ImGui::Render();
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

            glfwSwapBuffers(window);
            glfwPollEvents();
        }
    }
}

void Display::Draw() {

    // Starting the frame
    {
        glViewport(0, 0, 1600, 900);
        glClearDepthf(0.f);
        glClearColor(0.f, 0.f, 0.f, 0.f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        if (glfwWindowShouldClose(window)) Global::running.exchange(false);

        ImGui_ImplGlfw_NewFrame();
        ImGui_ImplOpenGL3_NewFrame();
    }
    
    ImGui::NewFrame();

    if (ImGui::Begin("Dockspace", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoBringToFrontOnFocus))
    {
        ImGui::SetWindowSize(ImVec2{ static_cast<float>(width),  static_cast<float>(height) });
        // Set the position of the dockspace window just below the menu bar.
        ImGui::SetWindowPos(ImVec2(0.f, 0.f));
        ImGui::DockSpace(ImGuiDockNodeFlags_NoResize | ImGuiDockNodeFlags_NoDockingOverCentralNode | ImGuiDockNodeFlags_NoDockingSplit);
    }
    ImGui::End();

    static double time{ 0. };
    static double prevTime = glfwGetTime();
    if (ImGui::Begin("Active Symbols:")){
        static ImVec4 bullCol = ImVec4(0.000f, 1.000f, 0.441f, 1.000f);
        static ImVec4 bearCol = ImVec4(0.853f, 0.050f, 0.310f, 1.000f);

        time += glfwGetTime() - prevTime;
        prevTime = glfwGetTime();

        struct plotData {
            std::vector<double> dates;
            std::vector<double> opens;
            std::vector<double> highs;
            std::vector<double> lows;
            std::vector<double> closes;
        };

        static std::unordered_map<std::string, std::vector<TradePoint>> tradeLog;       // Map of SYMBOL to the trade history
        static std::unordered_map<std::string, std::vector<TradePoint>> newLog;         // Stores the map of the new data from the 5s window
        static std::unordered_map<std::string, plotData>                plotMap;        // Map of SYMBOL to the extracted plot data

        // Update the values once every 5s
        if (time >= 1.) {
            time = 0;

            // Making a copy of the trade logs to not "hog" the mutex.
            {
                std::lock_guard lock{ Global::exMtx };
                for (const auto& Orderbooks : Global::books) {
                    newLog[Orderbooks.first] = std::vector<TradePoint>(Orderbooks.second.tradeLog.begin() + tradeLog[Orderbooks.first].size(), Orderbooks.second.tradeLog.end());
                    tradeLog[Orderbooks.first] = Orderbooks.second.tradeLog;
                }
            }

            // Drawing out each plot
            for (const auto& log :newLog) {
                if (log.second.empty()) continue;

                double high, low, close;
                double open = -1.f;
                for (const TradePoint& tp : log.second) {
                    if (open == -1.f) {
                        if (plotMap[log.first].closes.empty()) open = tp.price;
                        else open = plotMap[log.first].closes.back();
                    }
                    high = std::max(open, tp.price);
                    low = std::min(open, tp.price);
                    close = tp.price;

                }


                plotMap[log.first].dates.push_back(plotMap[log.first].dates.size());
                plotMap[log.first].opens.push_back(open);
                plotMap[log.first].highs.push_back(high);
                plotMap[log.first].lows.push_back(low);
                plotMap[log.first].closes.push_back(close);
            }

        }

        for (const auto& log : plotMap) {
            if (log.second.dates.empty()) continue;
            if (log.first != "GOOGL") continue;

            if (ImPlot::BeginPlot(log.first.c_str())) {

                //bool tooltip{ true };

                double minTime = *std::min_element(log.second.dates.begin(), log.second.dates.end());
                double maxTime = *std::max_element(log.second.dates.begin(), log.second.dates.end());
                ImPlot::SetupAxes(nullptr, nullptr, 0, ImPlotAxisFlags_AutoFit | ImPlotAxisFlags_RangeFit);
                ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Time);
                ImPlot::SetupAxisFormat(ImAxis_Y1, "$%.0f");
                ImPlot::SetupAxisLimitsConstraints(ImAxis_X1, minTime,maxTime);
                MyImPlot::PlotCandlestick(log.second.dates.data(), log.second.opens.data(), log.second.closes.data(), 
                                          log.second.lows.data(), log.second.highs.data(), log.second.dates.size());
                ImPlot::EndPlot();

            }
        }


    }
    ImGui::End();

    if (ImGui::Begin("Crisis Panel")) {
        ImGui::Text("Current Crisis: ");

        ImGui::SameLine();
        switch (CrisisManager::getState()) {
        case CrisisManager::NONE:
            ImGui::Text("Nothing happening...");
            break;
        case CrisisManager::CRAZE:
            ImGui::Text("CRAZE");
            break;
        case CrisisManager::RANDOM:
            ImGui::Text("RANDOM");
            break;
        case CrisisManager::SHOCK:
            ImGui::Text("SHOCK");
            break;
        case CrisisManager::PANIC:
            ImGui::Text("PANIC");
            break;
        }

        ImGui::NewLine();
        ImGui::NewLine();

        if (ImGui::CollapsingHeader("ADMIN PANEL")) {
            if (ImGui::Button("  NONE ")) {
                CrisisManager::setState(CrisisManager::NONE);
            }

            ImGui::SameLine();
            if (ImGui::Button(" SHOCK ")) {
                CrisisManager::setState(CrisisManager::SHOCK);
            }

            ImGui::SameLine();
            if (ImGui::Button(" PANIC ")) {
                CrisisManager::setState(CrisisManager::PANIC);
            }
            // New line
            if (ImGui::Button(" CRAZE ")) {
                CrisisManager::setState(CrisisManager::CRAZE);
            }

            ImGui::SameLine();
            if (ImGui::Button("RANDOM")) {
                CrisisManager::setState(CrisisManager::RANDOM);
            }
        }
    }
    ImGui::End();

    if (ImGui::Begin("Server Information")) {
        ImGui::Text("Server TCP Port number: %d", Global::tcpPort);
        ImGui::Text("Server UDP Port number: %d", Global::udpPort);
        ImGui::Text("Server IP Address: %s", Global::ipAddr.c_str());
    }
    ImGui::End();

    // RENDERING THE FRAME
    {
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
        glfwPollEvents();
    }
    
}

void Display::Free() {
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();

    glfwTerminate();
}