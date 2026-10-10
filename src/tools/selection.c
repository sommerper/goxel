/* Goxel 3D voxels editor
 *
 * copyright (c) 2017 Guillaume Chereau <guillaume@noctua-software.com>
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

typedef struct {
    tool_t  tool;

    int     snap_face;
    float   start_rect[4][4];

    volume_t *start_mask; // The mask when we started a new selection.
    int     mode; // MODE_REPLACE | MODE_OVER | MODE_SUB
    int     current_mode; // The mode for the current box only.

    // Ctrl + gizmo arrow drag: move the selected voxels with the box.
    volume_t *start_volume;  // Layer volume without the selected voxels.
    volume_t *start_content; // The selected voxels.
    float   start_box[4][4];

} tool_selection_t;

static void get_rect(const float pos[3], const float normal[3],
                     float out[4][4])
{
    plane_from_normal(out, pos, normal);
    mat4_iscale(out, 0.5, 0.5, 0);
}

// Recompute the selection mask after we moved the selection rect.
static void update_mask(tool_selection_t *tool)
{
    volume_t *tmp;
    painter_t painter;
    image_t *img = goxel.image;

    if (box_is_null(img->selection_box)) return;
    // Compute interesection of volume and selection rect.
    painter = (painter_t) {
        .shape = &shape_cube,
        .mode = MODE_INTERSECT_FILL,
        .color = {255, 255, 255, 255},
    };
    tmp = volume_copy(goxel.image->active_layer->volume);
    volume_op(tmp, &painter, img->selection_box);

    // Apply the new volume.
    if (img->selection_mask == NULL) img->selection_mask = volume_new();
    volume_set(img->selection_mask, tool->start_mask);
    volume_merge(img->selection_mask, tmp, tool->current_mode, painter.color);
    volume_delete(tmp);
    image_history_push(goxel.image);
}

// Until the mask is updated, show the voxels in the box tinted 50% green.
static void preview_mask(bool on)
{
    image_t *img = goxel.image;
    const painter_t painter = {
        .shape = &shape_cube,
        .mode = MODE_PAINT,
        .color = {0, 255, 0, 128},
    };

    if (!on || box_is_null(img->selection_box)) {
        volume_delete(goxel.tool_volume);
        goxel.tool_volume = NULL;
        return;
    }
    if (!goxel.tool_volume) goxel.tool_volume = volume_new();
    volume_set(goxel.tool_volume, img->active_layer->volume);
    volume_op(goxel.tool_volume, &painter, img->selection_box);
}

// Same as the move tool: put the selected voxels back at the box offset.
// To stretch, the box only grew one face: repeat the voxels into it.
static void move_content(tool_selection_t *tool, int state, bool stretch)
{
    image_t *img = goxel.image;
    volume_t *volume = img->active_layer->volume;
    volume_t *tmp;
    float transf[4][4] = MAT4_IDENTITY;
    float a[3], b[3];
    int dir[3] = {0}, n = 0, i;

    if (!tool->start_volume) {
        tool->start_volume = volume_copy(volume);
        volume_merge(tool->start_volume, img->selection_mask, MODE_SUB, NULL);
        tool->start_content = volume_copy(volume);
        volume_merge(tool->start_content, img->selection_mask,
                     MODE_INTERSECT, NULL);
    }

    vec3_sub(img->selection_box[3], tool->start_box[3], transf[3]);
    // Stretch along the axis that grew, toward the side the box moved.
    // Rounded, since the box picks up float errors while dragging.
    box_get_size(tool->start_box, a);
    box_get_size(img->selection_box, b);
    for (i = 0; stretch && i < 3; i++) {
        if (round(2 * (b[i] - a[i])) <= 0) continue;
        n = round(2 * (b[i] - a[i]));
        dir[i] = transf[3][i] > 0 ? 1 : -1;
    }
    tmp = volume_copy(tool->start_content);
    if (stretch)
        volume_stretch(tmp, dir, n);
    else
        volume_move(tmp, transf);
    volume_set(volume, tool->start_volume);
    volume_merge(volume, tmp, MODE_OVER, NULL);
    volume_delete(tmp);

    if (state == GESTURE3D_STATE_END) {
        if (stretch)
            volume_stretch(img->selection_mask, dir, n);
        else
            volume_move(img->selection_mask, transf);
        volume_delete(tool->start_volume);
        volume_delete(tool->start_content);
        tool->start_volume = NULL;
        tool->start_content = NULL;
        image_history_push(img);
    }
}

static int on_hover(gesture3d_t *gest)
{
    float rect[4][4];
    uint8_t rect_color[4] = {255, 255, 0, 255};

    if (gest->snaped & (SNAP_SELECTION_OUT | SNAP_SELECTION_IN)) {
        return -1;
    }
    get_rect(gest->pos, gest->normal, rect);
    render_box(&goxel.rend, rect, rect_color, EFFECT_WIREFRAME);
    return 0;
}

static int on_click(gesture3d_t *gest)
{
    // A short Shift/Ctrl drag ends up as a click: keep the selection.
    if (gest->flags & (GESTURE3D_FLAG_SHIFT | GESTURE3D_FLAG_CTRL)) return 0;
    action_exec2(ACTION_reset_selection);
    return 0;
}

static const char *hint_for_mode(int mode)
{
    switch (mode) {
    case MODE_REPLACE: return _("Create Selection");
    case MODE_OVER: return _("Add to Selection");
    case MODE_SUB: return _("Remove from Selection");
    default: return "";
    }
}

static int on_drag(gesture3d_t *gest)
{
    tool_selection_t *tool = gest->user;
    float rect[4][4];
    float p[3];
    int dir;
    int mode = tool->mode;
    image_t *img = goxel.image;

    if (gest->flags & GESTURE3D_FLAG_SHIFT)
        mode = MODE_OVER;
    else if (gest->flags & GESTURE3D_FLAG_CTRL)
        mode = MODE_SUB;

    goxel_add_hint(HINT_LARGE, GLYPH_MOUSE_LMB, hint_for_mode(mode));
    if (gest->state < GESTURE3D_STATE_BEGIN) return 0;

    get_rect(gest->pos, gest->normal, rect);
    if (gest->state == GESTURE3D_STATE_BEGIN) {
        get_rect(gest->start_pos, gest->start_normal, tool->start_rect);

        if (tool->start_mask == NULL) tool->start_mask = volume_new();
        if (img->selection_mask == NULL) img->selection_mask = volume_new();
        volume_set(tool->start_mask, img->selection_mask);
        tool->current_mode = mode;
    }

    box_union(tool->start_rect, rect, img->selection_box);
    // If the selection is flat, we grow it one voxel.
    if (box_get_volume(img->selection_box) == 0) {
        dir = gest->snaped == SNAP_VOLUME ? -1 : 1;
        vec3_addk(gest->pos, gest->normal, dir, p);
        bbox_extends_from_points(img->selection_box, 1, &p,
                                 img->selection_box);
    }

    preview_mask(gest->state != GESTURE3D_STATE_END);
    if (gest->state == GESTURE3D_STATE_END) {
        update_mask(tool);
    }
    return 0;
}

static void init(tool_t *tool_)
{
    tool_selection_t *tool = (void*)tool_;
    tool->mode = MODE_REPLACE;
    tool->current_mode = MODE_REPLACE;
}

// XXX: this is very close to tool_shape_iter.
static int iter(tool_t *tool, const painter_t *painter,
                const float viewport[4])
{
    float transf[4][4];
    tool_selection_t *selection = (tool_selection_t*)tool;
    int snap_mask = goxel.snap_mask;
    int box_edit_state;
    image_t *img = goxel.image;

    // To cleanup.
    snap_mask |= SNAP_ROUNDED;
    snap_mask &= ~(SNAP_SELECTION_IN | SNAP_SELECTION_OUT);

    box_edit_state = box_edit(
            img->selection_box,
            GIZMO_TRANSLATION | GIZMO_GROW | GIZMO_MOVE_CONTENT, transf);
    if (box_edit_state) {
        if (box_edit_state == GESTURE3D_STATE_BEGIN)
            mat4_copy(img->selection_box, selection->start_box);
        mat4_mul(transf, img->selection_box, img->selection_box);
        // Only the arrows accept Ctrl, so this is a Ctrl + arrow drag, with
        // Shift a stretch.  Ctrl or Shift may come down mid-drag (a home-row
        // mod only sends it after its hold timeout): the content then jumps
        // to follow the box.
        if (    selection->start_volume || (
                box_edit_state != GESTURE3D_STATE_END &&
                img->selection_mask &&
                image_layer_can_edit(img, img->active_layer) &&
                (gui_is_key_down(KEY_LEFT_CONTROL) ||
                 gui_is_key_down(KEY_RIGHT_CONTROL)))) {
            move_content(selection, box_edit_state,
                         gui_is_key_down(KEY_LEFT_SHIFT) ||
                         gui_is_key_down(KEY_RIGHT_SHIFT));
        } else {
            preview_mask(box_edit_state != GESTURE3D_STATE_END);
            if (box_edit_state == GESTURE3D_STATE_END)
                update_mask(selection);
        }
        return 0;
    }
    if (box_edit_is_active()) return 0;

    goxel_gesture3d(&(gesture3d_t) {
        .type = GESTURE3D_TYPE_HOVER,
        .snap_mask = snap_mask | SNAP_SELECTION_OUT,
        .callback = on_hover,
        .user = selection,
        .name = "Selection Hover",
    });

    goxel_gesture3d(&(gesture3d_t) {
        .type = GESTURE3D_TYPE_CLICK,
        .snap_mask = snap_mask | SNAP_SELECTION_OUT,
        .callback = on_click,
        .user = selection,
        .name = "Selection Click",
    });

    goxel_gesture3d(&(gesture3d_t) {
        .type = GESTURE3D_TYPE_DRAG,
        .snap_mask = snap_mask & ~(SNAP_SELECTION_IN | SNAP_SELECTION_OUT),
        .callback = on_drag,
        .flags = GESTURE3D_FLAG_DRAG_DELAY | GESTURE3D_FLAG_ALWAYS_CALL,
        .user = selection,
        .name = "Selection Drag",
    });

    return tool->state;
}

static float get_magnitude(float box[4][4], int axis_index)
{
    return box[0][axis_index] + box[1][axis_index] + box[2][axis_index];
}

static int gui(tool_t *tool_)
{
    tool_selection_t *tool = (void*)tool_;
    float x_mag, y_mag, z_mag;
    int x, y, z, w, h, d;
    float (*box)[4][4] = &goxel.image->selection_box;
    bool deactivated = false;

    tool_gui_mask_mode(&tool->mode);

    if (box_is_null(*box)) return 0;

    // XXX: why not using gui_bbox here?
    x_mag = fabs(get_magnitude(*box, 0));
    y_mag = fabs(get_magnitude(*box, 1));
    z_mag = fabs(get_magnitude(*box, 2));
    w = round(x_mag * 2);
    h = round(y_mag * 2);
    d = round(z_mag * 2);
    x = round((*box)[3][0] - x_mag);
    y = round((*box)[3][1] - y_mag);
    z = round((*box)[3][2] - z_mag);

    gui_group_begin(_("Origin"));
    gui_input_int("x", &x, 0, 0);
    deactivated |= gui_is_item_deactivated();
    gui_input_int("y", &y, 0, 0);
    deactivated |= gui_is_item_deactivated();
    gui_input_int("z", &z, 0, 0);
    deactivated |= gui_is_item_deactivated();
    gui_group_end();

    gui_group_begin(_("Size"));
    gui_input_int("w", &w, 0, 0);
    deactivated |= gui_is_item_deactivated();
    gui_input_int("h", &h, 0, 0);
    deactivated |= gui_is_item_deactivated();
    gui_input_int("d", &d, 0, 0);
    deactivated |= gui_is_item_deactivated();
    w = max(1, w);
    h = max(1, h);
    d = max(1, d);
    gui_group_end();

    bbox_from_extents(*box,
            VEC(x + w / 2., y + h / 2., z + d / 2.),
            w / 2., h / 2., d / 2.);

    if (deactivated) {
        update_mask(tool);
    }
    return 0;
}

TOOL_REGISTER(TOOL_SELECTION,
              selection,
              tool_selection_t,
              .name             = N_("Selection"),
              .init_fn          = init,
              .iter_fn          = iter,
              .gui_fn           = gui,
              .default_shortcut = "Shift R",
              .flags            = TOOL_SHOW_MASK | TOOL_SHOW_SELECTION_BOX |
                                  TOOL_REQUIRE_VISIBLE, )
