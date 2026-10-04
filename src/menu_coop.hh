#pragma once

#include "main.hh"
#include "menu-base.hh"

class principia_wdg;
class widget_manager;
class p_text;

/* "Co-op game" menu: host a server or join one by IP. */
class menu_coop : public menu_base
{
  public:
    principia_wdg *wdg_host;
    principia_wdg *wdg_join;
    principia_wdg *wdg_disconnect;
    principia_wdg *wdg_info;

  public:
    bool widget_clicked(principia_wdg *w, uint8_t button_id, int pid);

    menu_coop();

    int resume();
    int pause();
    int render();
    int step(double dt);
    int handle_input(tms::event *ev, int action);

    void refresh_widgets();
};
