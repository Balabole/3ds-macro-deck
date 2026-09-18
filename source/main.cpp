/*
 * 3DS Macro Deck Ultra v4.0 - Native C++ Edition
 * Target : Nintendo 3DS / New 3DS XL (devkitARM / libctru / citro2d)
 */

#include <3ds.h>
#include <citro2d.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <vector>
#include <string>

#define SERVER_IP       "192.168.1.143"
#define TCP_PORT        12345
#define UDP_PORT        12346
#define SOC_BUFFER_SIZE 0x100000

#define SCREEN_TOP_W    400
#define SCREEN_TOP_H    240
#define SCREEN_BOT_W    320
#define SCREEN_BOT_H    240

#define TAB_BAR_H       32
#define GRID_COLS       3
#define BTN_HEIGHT      54
#define PAD_X           3
#define PAD_Y           3
#define GAP             3

#define CLR_BG_DARK     C2D_Color32(10, 12, 18, 255)
#define CLR_PANEL       C2D_Color32(23, 28, 38, 255)
#define CLR_PANEL_LITE  C2D_Color32(38, 46, 61, 255)
#define CLR_BORDER      C2D_Color32(46, 56, 77, 255)
#define CLR_CYAN        C2D_Color32(0, 229, 255, 255)
#define CLR_GREEN       C2D_Color32(0, 255, 102, 255)
#define CLR_RED         C2D_Color32(255, 68, 68, 255)
#define CLR_TEXT_MAIN   C2D_Color32(235, 240, 250, 255)
#define CLR_TEXT_MUTED  C2D_Color32(120, 133, 153, 255)

struct MacroButton {
    std::string label;
    std::string sub;
    std::string action;
    std::string payload;
};

struct MacroTab {
    std::string name;
    u32 accentColor;
    std::vector<MacroButton> buttons;
};

static u32* socBuffer = NULL;
static int tcp_sock = -1;
static int udp_sock = -1;
static struct sockaddr_in udp_addr;

static bool connected = false;
static int current_tab = 0;
static int active_btn_idx = -1;
static float btn_press_timer = 0.0f;

static float scroll_y = 0.0f;
static float max_scroll_y = 0.0f;
static touchPosition prev_touch = {0, 0};
static bool is_touching = false;

static int server_cpu = 0;
static int server_ram = 0;
static char log_msg[128] = "System ready";

static C2D_TextBuf g_textBuf;
static std::vector<MacroTab> g_tabs;

bool initNetworkServices() {
    socBuffer = (u32*)memalign(0x1000, SOC_BUFFER_SIZE);
    if (!socBuffer) return false;

    Result res = socInit(socBuffer, SOC_BUFFER_SIZE);
    if (R_FAILED(res)) return false;

    udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_sock >= 0) {
        memset(&udp_addr, 0, sizeof(udp_addr));
        udp_addr.sin_family = AF_INET;
        udp_addr.sin_port = htons(UDP_PORT);
        inet_pton(AF_INET, SERVER_IP, &udp_addr.sin_addr);
        fcntl(udp_sock, F_SETFL, O_NONBLOCK);
    }
    return true;
}

void closeNetworkServices() {
    if (tcp_sock >= 0) close(tcp_sock);
    if (udp_sock >= 0) close(udp_sock);
    socExit();
    if (socBuffer) free(socBuffer);
}

void connectToServer() {
    if (tcp_sock >= 0) close(tcp_sock);

    tcp_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_sock < 0) return;

    struct sockaddr_in srv;
    memset(&srv, 0, sizeof(srv));
    srv.sin_family = AF_INET;
    srv.sin_port = htons(TCP_PORT);
    inet_pton(AF_INET, SERVER_IP, &srv.sin_addr);

    // Неблокирующий режим для избежания зависания цикла
    fcntl(tcp_sock, F_SETFL, O_NONBLOCK);

    int res = connect(tcp_sock, (struct sockaddr*)&srv, sizeof(srv));
    if (res == 0 || errno == EINPROGRESS || errno == EWOULDBLOCK) {
        connected = true;
        snprintf(log_msg, sizeof(log_msg), "Connected to %s", SERVER_IP);
    } else {
        connected = false;
        close(tcp_sock);
        tcp_sock = -1;
    }
}

void sendTcpCommand(const std::string& action, const std::string& payload) {
    if (!connected || tcp_sock < 0) return;
    std::string packet;
    if (action == "hotkey") {
        packet = "{\"type\":\"deck_event\",\"data\":{\"action\":\"hotkey\",\"keys\":" + payload + "}}\n";
    } else if (action == "media_key") {
        packet = "{\"type\":\"deck_event\",\"data\":{\"action\":\"media_key\",\"key\":\"" + payload + "\"}}\n";
    } else if (action == "launch") {
        packet = "{\"type\":\"deck_event\",\"data\":{\"action\":\"launch\",\"target\":\"" + payload + "\"}}\n";
    } else {
        packet = "{\"type\":\"deck_event\",\"data\":{\"action\":\"" + action + "\"}}\n";
    }
    send(tcp_sock, packet.c_str(), packet.length(), 0);
}

void sendUdpTrackpad(float dx, float dy, int scroll) {
    if (udp_sock < 0) return;
    char buf[64];
    snprintf(buf, sizeof(buf), "PAD:%.2f:%.2f:%d", dx, dy, scroll);
    sendto(udp_sock, buf, strlen(buf), 0, (struct sockaddr*)&udp_addr, sizeof(udp_addr));
}

void updateNetwork() {
    static u64 last_conn_try = 0;
    u64 now = osGetTime();

    if (!connected) {
        if (now - last_conn_try > 3000) {
            last_conn_try = now;
            connectToServer();
        }
        return;
    }

    char rx_buf[256];
    ssize_t len = recv(tcp_sock, rx_buf, sizeof(rx_buf) - 1, 0);
    if (len > 0) {
        rx_buf[len] = '\0';
        char* cpu_ptr = strstr(rx_buf, "\"cpu\":");
        if (cpu_ptr) server_cpu = atoi(cpu_ptr + 6);
        char* ram_ptr = strstr(rx_buf, "\"ram\":");
        if (ram_ptr) server_ram = atoi(ram_ptr + 6);
    } else if (len == 0 || (len < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
        connected = false;
        close(tcp_sock);
        tcp_sock = -1;
        snprintf(log_msg, sizeof(log_msg), "Connection lost");
    }
}

void initMacroProfiles() {
    MacroTab trackpadTab = {"Trackpad", CLR_CYAN, {}};
    
    MacroTab mediaTab = {"Media", C2D_Color32(180, 50, 220, 255), {
        {"PLAY/PAUSE", "System", "media_key", "play_pause"},
        {"NEXT >>", "Skip Track", "media_key", "next"},
        {"<< PREV", "Back Track", "media_key", "prev"},
        {"VOL +", "Step Up", "media_key", "vol_up"},
        {"VOL -", "Step Down", "media_key", "vol_down"},
        {"MUTE", "Sound Cut", "media_key", "mute"}
    }};

    MacroTab sysTab = {"System", C2D_Color32(38, 140, 255, 255), {
        {"SCREENSHOT", "Cmd+Sh+4", "hotkey", "[\"cmd\",\"shift\",\"4\"]"},
        {"CLOSE WIN", "Cmd + W", "hotkey", "[\"cmd\",\"w\"]"},
        {"FORCE QUIT", "Cmd+Opt+Esc", "hotkey", "[\"cmd\",\"alt\",\"escape\"]"}
    }};

    g_tabs.push_back(trackpadTab);
    g_tabs.push_back(mediaTab);
    g_tabs.push_back(sysTab);
}

void drawText(float x, float y, float scale, u32 clr, const char* str) {
    C2D_Text c2dText;
    C2D_TextParse(&c2dText, g_textBuf, str);
    C2D_DrawText(&c2dText, C2D_WithColor, x, y, 0.5f, scale, scale, clr);
}

void renderTopScreen(C3D_RenderTarget* target) {
    C2D_SceneBegin(target);
    C2D_TargetClear(target, CLR_BG_DARK);

    C2D_DrawRectSolid(6, 6, 0.2f, SCREEN_TOP_W - 12, 42, CLR_PANEL);
    drawText(16, 12, 0.55f, CLR_TEXT_MAIN, "3DS MACRO DECK ULTRA v4.0");

    if (connected) {
        C2D_DrawCircleSolid(20, 36, 0.5f, 4, CLR_GREEN);
        drawText(30, 30, 0.45f, CLR_GREEN, "ONLINE (MAC LINKED)");
    } else {
        C2D_DrawCircleSolid(20, 36, 0.5f, 4, CLR_RED);
        drawText(30, 30, 0.45f, CLR_RED, "OFFLINE (CONNECTING...)");
    }

    float panel_y = 54;
    C2D_DrawRectSolid(6, panel_y, 0.2f, 190, 100, CLR_PANEL);
    drawText(14, panel_y + 8, 0.48f, CLR_CYAN, "HARDWARE METRICS");

    char cpu_str[32], ram_str[32];
    snprintf(cpu_str, sizeof(cpu_str), "CPU: %d%%", server_cpu);
    snprintf(ram_str, sizeof(ram_str), "RAM: %d%%", server_ram);
    drawText(14, panel_y + 30, 0.42f, CLR_TEXT_MUTED, cpu_str);
    drawText(14, panel_y + 62, 0.42f, CLR_TEXT_MUTED, ram_str);

    C2D_DrawRectSolid(6, 160, 0.2f, SCREEN_TOP_W - 12, 74, CLR_PANEL);
    drawText(14, 166, 0.42f, CLR_TEXT_MUTED, "SYSTEM LOG:");
    drawText(14, 185, 0.45f, CLR_CYAN, log_msg);
}

void renderBottomScreen(C3D_RenderTarget* target) {
    C2D_SceneBegin(target);
    C2D_TargetClear(target, CLR_BG_DARK);

    if (current_tab == 0) {
        C2D_DrawRectSolid(PAD_X, PAD_Y, 0.2f, SCREEN_BOT_W - (PAD_X * 2), 162, CLR_PANEL);
        drawText(40, 60, 0.50f, CLR_TEXT_MUTED, "UDP TRACKPAD MODE");

        C2D_DrawRectSolid(PAD_X, 170, 0.2f, 154, 34, CLR_PANEL);
        C2D_DrawRectSolid(163, 170, 0.2f, 154, 34, CLR_PANEL);
        drawText(45, 178, 0.45f, CLR_TEXT_MAIN, "LEFT CLICK");
        drawText(200, 178, 0.45f, CLR_TEXT_MAIN, "RIGHT CLICK");
    } else {
        MacroTab& tab = g_tabs[current_tab];
        float bw = (SCREEN_BOT_W - (PAD_X * 2) - (GAP * (GRID_COLS - 1))) / (float)GRID_COLS;
        int count = tab.buttons.size();

        for (int i = 0; i < count; i++) {
            int col = i % GRID_COLS;
            int row = i / GRID_COLS;
            float bx = PAD_X + col * (bw + GAP);
            float by = PAD_Y + row * (BTN_HEIGHT + GAP) + scroll_y;

            u32 bg_col = (active_btn_idx == i) ? CLR_PANEL_LITE : CLR_PANEL;
            C2D_DrawRectSolid(bx, by, 0.2f, bw, BTN_HEIGHT, bg_col);
            C2D_DrawRectSolid(bx, by, 0.25f, bw, 2, tab.accentColor);

            drawText(bx + 8, by + 12, 0.45f, CLR_TEXT_MAIN, tab.buttons[i].label.c_str());
            drawText(bx + 8, by + 30, 0.35f, CLR_TEXT_MUTED, tab.buttons[i].sub.c_str());
        }
    }

    float ty = SCREEN_BOT_H - TAB_BAR_H;
    C2D_DrawRectSolid(0, ty, 0.3f, SCREEN_BOT_W, TAB_BAR_H, CLR_PANEL);

    float tw = SCREEN_BOT_W / (float)g_tabs.size();
    for (size_t i = 0; i < g_tabs.size(); i++) {
        float tx = i * tw;
        if ((int)i == current_tab) {
            C2D_DrawRectSolid(tx + 2, ty + 2, 0.4f, tw - 4, TAB_BAR_H - 4, CLR_PANEL_LITE);
            drawText(tx + 8, ty + 9, 0.42f, CLR_TEXT_MAIN, g_tabs[i].name.c_str());
        } else {
            drawText(tx + 8, ty + 9, 0.40f, CLR_TEXT_MUTED, g_tabs[i].name.c_str());
        }
    }
}

void handleInput() {
    hidScanInput();
    u32 kDown = hidKeysDown();
    u32 kHeld = hidKeysHeld();

    if (kDown & KEY_START) sendTcpCommand("mission_control", "");

    touchPosition touch;
    hidTouchRead(&touch);

    if (kDown & KEY_TOUCH) {
        is_touching = true;
        prev_touch = touch;

        if (touch.py >= SCREEN_BOT_H - TAB_BAR_H) {
            float tw = SCREEN_BOT_W / (float)g_tabs.size();
            current_tab = touch.px / tw;
            scroll_y = 0.0f;
            return;
        }

        if (current_tab == 0 && touch.py >= 170) {
            if (touch.px < SCREEN_BOT_W / 2) {
                sendTcpCommand("mouse_click", "left");
            } else {
                sendTcpCommand("mouse_click", "right");
            }
        }
    } else if (kHeld & KEY_TOUCH) {
        float dx = touch.px - prev_touch.px;
        float dy = touch.py - prev_touch.py;

        if (current_tab == 0 && touch.py < 165) {
            sendUdpTrackpad(dx * 2.2f, dy * 2.2f, 0);
        }
        prev_touch = touch;
    } else if (hidKeysUp() & KEY_TOUCH) {
        is_touching = false;

        if (current_tab > 0 && touch.py < SCREEN_BOT_H - TAB_BAR_H) {
            MacroTab& tab = g_tabs[current_tab];
            float bw = (SCREEN_BOT_W - (PAD_X * 2) - (GAP * (GRID_COLS - 1))) / (float)GRID_COLS;

            for (size_t i = 0; i < tab.buttons.size(); i++) {
                int col = i % GRID_COLS;
                int row = i / GRID_COLS;
                float bx = PAD_X + col * (bw + GAP);
                float by = PAD_Y + row * (BTN_HEIGHT + GAP) + scroll_y;

                if (touch.px >= bx && touch.px <= bx + bw && touch.py >= by && touch.py <= by + BTN_HEIGHT) {
                    active_btn_idx = i;
                    btn_press_timer = 0.2f;
                    sendTcpCommand(tab.buttons[i].action, tab.buttons[i].payload);
                    break;
                }
            }
        }
    }

    if (btn_press_timer > 0.0f) {
        btn_press_timer -= 0.03f;
        if (btn_press_timer <= 0.0f) active_btn_idx = -1;
    }
}

int main(int argc, char* argv[]) {
    osSetSpeedupEnable(true);

    gfxInitDefault();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();

    C3D_RenderTarget* topTarget = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    C3D_RenderTarget* botTarget = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);

    g_textBuf = C2D_TextBufNew(4096);

    initNetworkServices();
    initMacroProfiles();

    while (aptMainLoop()) {
        updateNetwork();
        handleInput();

        C2D_TextBufClear(g_textBuf);

        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        renderTopScreen(topTarget);
        renderBottomScreen(botTarget);
        C3D_FrameEnd(0);
    }

    C2D_TextBufDelete(g_textBuf);
    C2D_Fini();
    C3D_Fini();
    gfxExit();
    closeNetworkServices();
    return 0;
}
