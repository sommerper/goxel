/* Goxel 3D voxels editor
 *
 * copyright (c) 2024-present Guillaume Chereau <guillaume@noctua-software.com>
 *
 * Goxel is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.

 * Goxel is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.

 * You should have received a copy of the GNU General Public License along with
 * goxel.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "goxel.h"
#include "gui.h"

typedef struct {
    filter_t filter;
    int through;
} filter_revolve_t;

static int gui(filter_t *filter)
{
    filter_revolve_t *revolve = (void*)filter;
    static const char *AXIS_NAMES[] = {"X", "Y", "Z"};
    char buf[32];
    int axis, side[3] = {0};

    gui_text_wrapped("Spin the selection, or the current layer if nothing "
                     "is selected, around an axis through its center or "
                     "one of its sides.");

    // through: 0 for the center, then -X, +X, -Y, +Y, -Z, +Z.
    gui_group_begin("Axis through");
    gui_selectable_toggle("Center", &revolve->through, 0, NULL, -1);
    for (axis = 0; axis < 3; axis++) {
        gui_row_begin(2);
        snprintf(buf, sizeof(buf), "-%s", AXIS_NAMES[axis]);
        gui_selectable_toggle(buf, &revolve->through, 1 + 2 * axis, NULL, 0);
        snprintf(buf, sizeof(buf), "+%s", AXIS_NAMES[axis]);
        gui_selectable_toggle(buf, &revolve->through, 2 + 2 * axis, NULL, 0);
        gui_row_end();
    }
    gui_group_end();

    if (revolve->through > 0)
        side[(revolve->through - 1) / 2] = revolve->through % 2 ? -1 : 1;

    gui_group_begin(NULL);
    for (axis = 0; axis < 3; axis++) {
        snprintf(buf, sizeof(buf), "Revolve %s", AXIS_NAMES[axis]);
        if (gui_button(buf, 1.0, 0)) {
            image_revolve_selection(goxel.image, axis, side);
            image_history_push(goxel.image);
        }
    }
    gui_group_end();
    return 0;
}

FILTER_REGISTER(revolve, filter_revolve_t,
    .name = N_("Revolve"),
    .gui_fn = gui,
)
