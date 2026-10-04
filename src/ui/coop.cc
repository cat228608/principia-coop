#include "imgui.hh"
#include "main.hh"
#include "multiplayer.hh"
#include "pkgman.hh"
#include "ui.hh"

#include <cstdio>
#include <cstring>
#include <string>

/* Co-op: "Create server" dialog */
namespace UiCoopHost {
static bool do_open = false;

static char server_name[64] = "Principia co-op";
static char player_name[32] = "Host";
static int  port = MP_DEFAULT_PORT;
static int  max_players = 4;
static int  room_width = 60;
static int  room_height = 40;

    void open() {
        if (P.username && P.username[0] && strcmp(player_name, "Host") == 0) {
            snprintf(player_name, sizeof(player_name), "%s", P.username);
        }

        do_open = true;
    }

    void layout() {
        handle_do_open(&do_open, "###coop-host");
        ImGui_CenterNextWindow();

        if (ImGui::BeginPopupModal("Create co-op server###coop-host", REF_TRUE, MODAL_FLAGS)) {
            ImGui_CloseOnEsc();

            ImGui::TextUnformatted("Server settings");
            ImGui::Separator();

            ImGui::PushItemWidth(UI(220.f));
            ImGui::InputText("Server name", server_name, sizeof(server_name));
            ImGui::InputText("Your name", player_name, sizeof(player_name));
            ImGui::InputInt("Port", &port);
            ImGui::SliderInt("Max players", &max_players, 2, MP_MAX_PLAYERS);
            ImGui::PopItemWidth();

            ImGui::Dummy(UI(0.f, 6.f));
            ImGui::TextUnformatted("Room settings");
            ImGui::Separator();

            ImGui::PushItemWidth(UI(220.f));
            ImGui::SliderInt("Room width", &room_width, 20, 400);
            ImGui::SliderInt("Room height", &room_height, 20, 400);
            ImGui::PopItemWidth();

            if (port < 1024) port = 1024;
            if (port > 65535) port = 65535;

            ImGui::Dummy(UI(0.f, 8.f));

            if (ImGui::Button("Create server", UI(140.f, 0.f))) {
                mp::host_config *cfg = new mp::host_config();

                snprintf(cfg->server_name, sizeof(cfg->server_name), "%s", server_name);
                snprintf(cfg->player_name, sizeof(cfg->player_name), "%s", player_name);
                cfg->port = port;
                cfg->max_players = max_players;
                cfg->room_width = room_width;
                cfg->room_height = room_height;

                P.add_action(ACTION_COOP_HOST, cfg);

                ImGui::CloseCurrentPopup();
            }

            ImGui::SameLine();

            if (ImGui::Button("Cancel", UI(80.f, 0.f)))
                ImGui::CloseCurrentPopup();

            ImGui::EndPopup();
        }
    }
}

/* Co-op: "Join server" dialog */
namespace UiCoopJoin {
static bool do_open = false;

static char ip[128] = "127.0.0.1";
static char player_name[32] = "Player";
static int  port = MP_DEFAULT_PORT;

    void open() {
        if (P.username && P.username[0] && strcmp(player_name, "Player") == 0) {
            snprintf(player_name, sizeof(player_name), "%s", P.username);
        }

        do_open = true;
    }

    void layout() {
        handle_do_open(&do_open, "###coop-join");
        ImGui_CenterNextWindow();

        if (ImGui::BeginPopupModal("Join co-op server###coop-join", REF_TRUE, MODAL_FLAGS)) {
            ImGui_CloseOnEsc();

            ImGui::TextUnformatted("Connect to a friend by IP address");
            ImGui::Separator();

            ImGui::PushItemWidth(UI(220.f));
            ImGui::InputText("Server IP", ip, sizeof(ip));
            ImGui::InputInt("Port", &port);
            ImGui::InputText("Your name", player_name, sizeof(player_name));
            ImGui::PopItemWidth();

            if (port < 1024) port = 1024;
            if (port > 65535) port = 65535;

            ImGui::Dummy(UI(0.f, 8.f));

            if (ImGui::Button("Connect", UI(140.f, 0.f))) {
                mp::join_config *cfg = new mp::join_config();

                snprintf(cfg->ip, sizeof(cfg->ip), "%s", ip);
                snprintf(cfg->player_name, sizeof(cfg->player_name), "%s", player_name);
                cfg->port = port;

                P.add_action(ACTION_COOP_JOIN, cfg);

                ImGui::CloseCurrentPopup();
            }

            ImGui::SameLine();

            if (ImGui::Button("Cancel", UI(80.f, 0.f)))
                ImGui::CloseCurrentPopup();

            ImGui::EndPopup();
        }
    }
}

/* Co-op: in-game chat / session info */
namespace UiCoopChat {
static bool do_open = false;
static char text[256] = "";

    void open() {
        do_open = true;
    }

    void layout() {
        handle_do_open(&do_open, "###coop-chat");
        ImGui_CenterNextWindow();

        if (ImGui::BeginPopupModal("Co-op###coop-chat", REF_TRUE, MODAL_FLAGS)) {
            ImGui_CloseOnEsc();

            if (mp::is_active())
                ImGui::TextUnformatted(mp::status_line());
            else
                ImGui::TextUnformatted("Not connected.");

            ImGui::Separator();

            ImGui::PushItemWidth(UI(280.f));
            bool enter = ImGui::InputText("Message", text, sizeof(text),
                                          ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::PopItemWidth();

            if (enter || ImGui::Button("Send", UI(90.f, 0.f))) {
                if (text[0]) {
                    mp::send_chat(text);
                    text[0] = 0;
                }
                ImGui::CloseCurrentPopup();
            }

            ImGui::SameLine();

            if (ImGui::Button("Close", UI(90.f, 0.f)))
                ImGui::CloseCurrentPopup();

            if (mp::is_active()) {
                ImGui::Dummy(UI(0.f, 6.f));
                if (ImGui::Button("Leave co-op session", UI(180.f, 0.f))) {
                    mp::shutdown("you left the session");
                    ImGui::CloseCurrentPopup();
                }
            }

            ImGui::EndPopup();
        }
    }
}
