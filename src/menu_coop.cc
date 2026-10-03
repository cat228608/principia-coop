#include "menu_coop.hh"

#include "font.hh"
#include "game.hh"
#include "gui.hh"
#include "main.hh"
#include "misc.hh"
#include "multiplayer.hh"
#include "text.hh"
#include "ui.hh"
#include "widget_manager.hh"

bool menu_coop::widget_clicked(principia_wdg *w, uint8_t button_id, int pid) {
    if (menu_base::widget_clicked(w, button_id, pid)) {
        return true;
    }

    switch (button_id) {
        case BTN_BACK:
            P.add_action(ACTION_GOTO_MAINMENU, 0x1);
            break;

        case BTN_COOP_HOST:
            ui::open_dialog(DIALOG_COOP_HOST);
            break;

        case BTN_COOP_JOIN:
            ui::open_dialog(DIALOG_COOP_JOIN);
            break;

        case BTN_COOP_DISCONNECT:
            mp::shutdown("you closed the session");
            ui::message("Co-op session closed.");
            this->refresh_widgets();
            break;

        default: return false;
    }

    return true;
}

menu_coop::menu_coop() : menu_base(false) {
    this->wdg_back = this->wm->create_widget(
            this->get_surface(), TMS_WDG_BUTTON,
            BTN_BACK, AREA_MENU_TOP_LEFT,
            gui_spritesheet::get_sprite(S_LEFT), 0,
            0.7f);
    this->wdg_back->priority = 500;
    this->wdg_back->add();

    this->wdg_host = this->wm->create_widget(
            this->get_surface(), TMS_WDG_LABEL,
            BTN_COOP_HOST, AREA_MENU_TOP_CENTER);
    this->wdg_host->set_label("Create server", font::large);
    this->wdg_host->priority = 1000;
    this->wdg_host->render_background = true;
    this->wdg_host->add();

    this->wdg_join = this->wm->create_widget(
            this->get_surface(), TMS_WDG_LABEL,
            BTN_COOP_JOIN, AREA_MENU_TOP_CENTER);
    this->wdg_join->set_label("Join server", font::large);
    this->wdg_join->priority = 900;
    this->wdg_join->render_background = true;
    this->wdg_join->add();

    this->wdg_disconnect = this->wm->create_widget(
            this->get_surface(), TMS_WDG_LABEL,
            BTN_COOP_DISCONNECT, AREA_MENU_TOP_CENTER);
    this->wdg_disconnect->set_label("Leave session", font::large);
    this->wdg_disconnect->priority = 800;
    this->wdg_disconnect->render_background = true;

    this->wdg_host->label->set_scale(0.9);
    this->wdg_join->label->set_scale(0.9);
    this->wdg_disconnect->label->set_scale(0.9);

    {
        principia_wdg *row[] = { this->wdg_host, this->wdg_join, this->wdg_disconnect };
        wdg_equalize_width(row, 3);
    }

    this->wdg_info = this->wm->create_widget(
            this->get_surface(), TMS_WDG_LABEL,
            BTN_IGNORE, AREA_MENU_BOTTOM_CENTER);
    this->wdg_info->set_label("Co-op: build together in a shared sandbox room", font::medium);
    this->wdg_info->priority = 100;
    this->wdg_info->add();

    this->refresh_widgets();
}

int menu_coop::resume() {
    menu_base::resume();
    this->refresh_widgets();

    return T_OK;
}

int menu_coop::pause() {
    return T_OK;
}

int menu_coop::render() {
    menu_base::render();

    return T_OK;
}

int menu_coop::step(double dt) {
    menu_base::step(dt);

    this->wm->step();

    return T_OK;
}

int menu_coop::handle_input(tms::event *ev, int action) {
    if (pscreen::handle_input(ev, action) == EVENT_DONE) {
        return EVENT_DONE;
    }

    if (ev->type == TMS_EV_KEY_PRESS) {
        switch (ev->data.key.keycode) {
            case TMS_KEY_1:
                this->wdg_host->click();
                return T_OK;

            case TMS_KEY_2:
                this->wdg_join->click();
                return T_OK;

            case TMS_KEY_ESC:
                P.add_action(ACTION_GOTO_MAINMENU, 0x1);
                return T_OK;
        }
    }

    return T_OK;
}

void menu_coop::refresh_widgets() {
    menu_base::refresh_widgets();

    if (mp::is_active()) {
        this->wdg_disconnect->add();
        this->wdg_info->set_label(mp::status_line(), font::medium);
    } else {
        this->wdg_disconnect->remove();
        this->wdg_info->set_label("Co-op: build together in a shared sandbox room", font::medium);
    }

    this->wm->rearrange();
}
