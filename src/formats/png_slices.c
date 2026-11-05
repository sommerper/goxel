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
#include "file_format.h"

// A png text chunk, since the sheet alone doesn't tell where a slice ends.
#define SLICES_KEY "goxel:slices"

static int export_as_png_slices(const file_format_t *format,
                                const image_t *image, const char *path)
{
    volume_t *volume, *tmp;
    const layer_t *layer;
    int x, y, z, w, h, d, pos[3], bbox[2][3];
    uint8_t c[4];
    uint8_t *img;
    char text[16];
    volume_iterator_t iter = {0};

    // Merge the visible layers ourselves so that the material opacity
    // ends up in the voxels alpha.
    image_update((image_t*)image);
    volume = volume_new();
    DL_FOREACH(image->layers, layer) {
        if (!layer->visible || !layer->volume) continue;
        tmp = volume_copy(layer->volume);
        if (layer->material && layer->material->base_color[3] < 1) {
            iter = volume_get_iterator(tmp, VOLUME_ITER_VOXELS);
            while (volume_iter(&iter, pos)) {
                volume_get_at(tmp, &iter, pos, c);
                c[3] = c[3] * layer->material->base_color[3];
                volume_set_at(tmp, NULL, pos, c);
            }
        }
        volume_merge(volume, tmp, MODE_OVER, NULL);
        volume_delete(tmp);
    }
    iter = (volume_iterator_t){0};
    // Crop to the voxels, ignoring the image box, so we don't export
    // empty slices.
    if (!volume_get_bbox(volume, bbox, true)) {
        LOG_E("Nothing to export");
        volume_delete(volume);
        return -1;
    }
    w = bbox[1][0] - bbox[0][0];
    h = bbox[1][1] - bbox[0][1];
    d = bbox[1][2] - bbox[0][2];
    img = calloc(w * h * d, 4);
    for (z = 0; z < d; z++)
    for (y = 0; y < h; y++)
    for (x = 0; x < w; x++) {
        pos[0] = x + bbox[0][0];
        pos[1] = y + bbox[0][1];
        pos[2] = z + bbox[0][2];
        volume_get_at(volume, &iter, pos, c);
        img[(y * w * d + z * w + x) * 4 + 0] = c[0];
        img[(y * w * d + z * w + x) * 4 + 1] = c[1];
        img[(y * w * d + z * w + x) * 4 + 2] = c[2];
        img[(y * w * d + z * w + x) * 4 + 3] = c[3];
    }
    img_write(img, w * d, h, 4, path);
    snprintf(text, sizeof(text), "%d", d);
    img_png_add_text(path, SLICES_KEY, text);
    free(img);
    volume_delete(volume);
    return 0;
}

// How much of the sheet stays the same when shifted by some pixels.
static float sheet_similarity(const uint8_t *img, int w, int h, int shift)
{
    int x, y, same = 0, total = 0;
    const uint8_t *a, *b;

    for (y = 0; y < h; y++)
    for (x = 0; x < w - shift; x++) {
        a = img + (y * w + x) * 4;
        b = a + shift * 4;
        if (!a[3] && !b[3]) continue;
        total++;
        if (a[3] && b[3] && abs(a[0] - b[0]) <= 8 &&
            abs(a[1] - b[1]) <= 8 && abs(a[2] - b[2]) <= 8) same++;
    }
    return total ? (float)same / total : 0;
}

// Neighbouring slices of a model look alike, so the sheet repeats at the slice
// width: a shift by just that much matches better than one pixel off.
// ponytail: a heuristic, so it only fills in the preview for the user to check.
static int guess_slices(const uint8_t *img, int w, int h)
{
    int s, ret = 0;
    float before, after, best = 0;
    float *peaks = calloc(w / 2 + 1, sizeof(*peaks));

    for (s = 2; s <= w / 2; s++) {
        if (w % s != 0) continue;
        before = sheet_similarity(img, w, h, s - 1);
        after = sheet_similarity(img, w, h, s + 1);
        peaks[s] = sheet_similarity(img, w, h, s) - max(before, after);
        best = max(best, peaks[s]);
    }
    // Multiples of the slice width peak too: take the smallest good one.
    for (s = 2; s <= w / 2 && best > 0; s++) {
        if (peaks[s] >= best / 2) {
            ret = w / s;
            break;
        }
    }
    free(peaks);
    return ret;
}

// The count to start the preview with: the saved one, else a guess, else 0.
int png_slices_suggest(const char *data, int size,
                       const uint8_t *img, int w, int h)
{
    char text[16];
    int d = 0;

    if (img_png_get_text(data, size, SLICES_KEY, text, sizeof(text)))
        d = atoi(text);
    // Ignore a saved count that doesn't fit: the png got edited since.
    if (d >= 1 && w % d == 0) return d;
    return guess_slices(img, w, h);
}

// The count picked in the preview, for one import.
int g_png_slices = 0;

static int g_slices_input = 1;
static char g_slices_path[1024];
static texture_t *g_slices_preview = NULL;

static int slices_popup(void *data)
{
    int ret = 0, w = g_slices_preview->w, h = g_slices_preview->h;
    int d;
    bool valid;

    gui_slices_preview(g_slices_preview, g_slices_input);
    // Unbounded: a slider over the whole range can't pick an exact count.
    gui_input_int(_("Slices"), &g_slices_input, 0, 0);
    d = g_slices_input;
    valid = d >= 1 && w % d == 0;
    if (valid)
        gui_text(_("Each slice is %d x %d"), w / d, h);
    else
        gui_text(_("The width (%d) can't be split into %d slices"), w, d);
    gui_row_begin(0);
    gui_enabled_begin(valid);
    if (gui_button(_("OK"), 0, 0)) ret = 1;
    gui_enabled_end();
    if (gui_button(_("Cancel"), 0, 0)) ret = 2;
    gui_row_end();
    return ret;
}

// Once the popup is closed, so that the import can open its own alert.
static void on_slices_popup_closed(int r)
{
    if (r != 1) return;
    g_png_slices = g_slices_input;
    goxel_import_file(g_slices_path, "png slices");
}

// The inverse of export_as_png_slices.
static int import_png_slices(const file_format_t *format, image_t *image,
                             const char *path)
{
    layer_t *layer = image->active_layer;
    int x, y, z, w, h, size, sheet_w, d = g_png_slices, bpp = 4;
    const uint8_t *c;
    uint8_t *img;
    char *data;
    volume_accessor_t acc;

    g_png_slices = 0;
    data = read_file(path, &size);
    img = data ? img_read_from_mem(data, size, &sheet_w, &h, &bpp) : NULL;
    if (!img) {
        free(data);
        gui_alert(_("Import"), _("Cannot read this image"));
        return -1;
    }

    if (d < 1 || sheet_w % d != 0) {
        // goxel_import_file gets called again once the preview is closed.  It
        // stays until the next one, as the closing frame still draws it.
        g_slices_input = png_slices_suggest(data, size, img, sheet_w, h)
                         ?: g_slices_input;
        snprintf(g_slices_path, sizeof(g_slices_path), "%s", path);
        if (g_slices_preview) texture_delete(g_slices_preview);
        g_slices_preview = texture_new_from_buf(img, sheet_w, h, 4, TF_NEAREST);
        gui_open_popup(_("Import png slices"), 0, NULL, slices_popup);
        gui_on_popup_closed(on_slices_popup_closed);
        free(data);
        free(img);
        return -1;
    }
    free(data);
    w = sheet_w / d;

    // Never merge into existing voxels, so that a wrong slice count is easy
    // to get rid of.
    if (!image_layer_can_edit(image, layer) || !volume_is_empty(layer->volume))
        layer = image_add_layer(image, NULL);

    acc = volume_get_accessor(layer->volume);
    for (z = 0; z < d; z++)
    for (y = 0; y < h; y++)
    for (x = 0; x < w; x++) {
        c = img + (y * sheet_w + z * w + x) * 4;
        if (!c[3]) continue;
        // Centered like the vox import.
        volume_set_at(layer->volume, &acc,
                      (int[]){x - w / 2, y - h / 2, z - d / 2}, c);
    }
    free(img);
    return 0;
}

FILE_FORMAT_REGISTER(png_slices,
    .name = "png slices",
    .exts = {"*.png"},
    .exts_desc = "png",
    .export_func = export_as_png_slices,
    .import_func = import_png_slices,
    .name_only = true,
)
