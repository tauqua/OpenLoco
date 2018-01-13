#include "../interop/interop.hpp"
#include "../openloco.h"
#include "../windowmgr.h"

namespace openloco::ui::windows
{
    // 0x00498E9B
    void sub_498E9B(window * w)
    {
        w->enabled_widgets |= 2;
#ifdef _DISABLE_TOWN_RENAME_
        if (is_editor_mode())
        {
            w->enabled_widgets &= ~2;
        }
#endif
    }

    // 0x00446F6B
    // dx: townId
    // esi: {return}
    window * open_town_window(uint16_t townId)
    {
        registers regs;
        regs.dx = townId;
        LOCO_CALLPROC_X(0x00446F6B, regs);
        return (window *)regs.esi;
    }
}