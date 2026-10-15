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

static int export_as_png_slices(const file_format_t *format,
                                const image_t *image, const char *path)
{
    volume_t *volume, *tmp;
    const layer_t *layer;
    int x, y, z, w, h, d, pos[3], bbox[2][3];
    uint8_t c[4];
    uint8_t *img;
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
    free(img);
    volume_delete(volume);
    return 0;
}

FILE_FORMAT_REGISTER(png_slices,
    .name = "png slices",
    .exts = {"*.png"},
    .exts_desc = "png",
    .export_func = export_as_png_slices,
)
