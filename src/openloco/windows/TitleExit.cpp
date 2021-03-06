#include "../game_commands.h"
#include "../graphics/colours.h"
#include "../graphics/gfx.h"
#include "../interop/interop.hpp"
#include "../intro.h"
#include "../localisation/string_ids.h"
#include "../openloco.h"
#include "../ui.h"
#include "../ui/WindowManager.h"

using namespace openloco::interop;

namespace openloco::ui::windows
{
    static const gfx::ui_size_t window_size = { 40, 28 };

    namespace widx
    {
        enum
        {
            exit_button
        };
    }

    static widget_t _widgets[] = {
        make_widget({ 0, 0 }, window_size, widget_type::wt_9, 1, -1, string_ids::title_menu_exit_from_game),
        widget_end(),
    };

    static window_event_list _events;

    static void onMouseUp(window* window, widget_index widgetIndex);
    static void prepareDraw(ui::window* self);
    static void draw(ui::window* window, gfx::drawpixelinfo_t* dpi);

    window* openTitleExit()
    {
        _events.on_mouse_up = onMouseUp;
        _events.prepare_draw = prepareDraw;
        _events.draw = draw;

        auto window = openloco::ui::WindowManager::createWindow(
            WindowType::titleExit,
            gfx::point_t(ui::width() - window_size.width, ui::height() - window_size.height),
            window_size,
            window_flags::stick_to_front | window_flags::transparent | window_flags::no_background | window_flags::flag_6,
            &_events);

        window->widgets = _widgets;
        window->enabled_widgets = (1 << widx::exit_button);

        window->init_scroll_widgets();

        window->colours[0] = colour::translucent(colour::saturated_green);
        window->colours[1] = colour::translucent(colour::saturated_green);

        return window;
    }

    static void prepareDraw(ui::window* self)
    {
        auto exitString = stringmgr::get_string(string_ids::title_exit_game);
        self->width = gfx::get_string_width_new_lined(exitString) + 10;
        self->x = ui::width() - self->width;
        self->widgets[widx::exit_button].right = self->width;
    }

    // 0x00439236
    static void draw(ui::window* window, gfx::drawpixelinfo_t* dpi)
    {
        // Draw widgets.
        window->draw(dpi);

        int16_t x = window->x + window->width / 2;
        int16_t y = window->y + window->widgets[widx::exit_button].top + 8;
        gfx::point_t origin = { x, y };
        gfx::draw_string_centred_wrapped(dpi, &origin, window->width, colour::black, string_ids::title_exit_game);
    }

    // 0x00439268
    static void onMouseUp(window* window, widget_index widgetIndex)
    {
        if (intro::is_active())
        {
            return;
        }

        switch (widgetIndex)
        {
            case widx::exit_button:
                // Exit to desktop
                game_commands::do_21(0, 2);
                break;
        }
    }
}
