/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Native luma rasterizers must never sample outside a bounded source map. */
#define _GNU_SOURCE
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

#include "../../modules/core/transition_luma.c"

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return 1; \
        } \
    } while (0)

static mlt_frame image_frame(int width, int height, mlt_image_format format, int source)
{
    const int bytes = format == mlt_image_yuv422 ? 2 : format == mlt_image_rgba ? 4 : 8;
    uint8_t *image = malloc((size_t) width * height * bytes);
    if (!image) return NULL;
    for (int pixel = 0; pixel < width * height; ++pixel) {
        if (format == mlt_image_yuv422) {
            image[pixel * 2] = source ? 41 : 81;
            image[pixel * 2 + 1] = source ? 180 : 90;
        } else if (format == mlt_image_rgba) {
            image[pixel * 4] = source ? 0 : 220;
            image[pixel * 4 + 1] = 0;
            image[pixel * 4 + 2] = source ? 220 : 0;
            image[pixel * 4 + 3] = 255;
        } else {
            uint16_t *wide = (uint16_t *) image;
            wide[pixel * 4] = source ? 0 : 56000;
            wide[pixel * 4 + 1] = 0;
            wide[pixel * 4 + 2] = source ? 56000 : 0;
            wide[pixel * 4 + 3] = 65535;
        }
    }
    mlt_frame frame = mlt_frame_init(NULL);
    if (!frame) { free(image); return NULL; }
    mlt_frame_set_image(frame, image, width * height * bytes, free);
    mlt_properties properties = MLT_FRAME_PROPERTIES(frame);
    mlt_properties_set_int(properties, "format", format);
    mlt_properties_set_int(properties, "width", width);
    mlt_properties_set_int(properties, "height", height);
    return frame;
}

static int bounded_case(int image_height, int map_height, int field_order, mlt_image_format format)
{
    enum { width = 16, map_width = 8 };
    const size_t page = (size_t) sysconf(_SC_PAGESIZE);
    const size_t map_bytes = map_width * map_height * sizeof(uint16_t);
    CHECK(map_bytes <= page);
    uint8_t *memory = mmap(NULL, page * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(memory != MAP_FAILED);
    CHECK(mprotect(memory + page, page, PROT_NONE) == 0);
    // Place the logical end directly against an unreadable page; a pool's
    // spare allocation bytes must not hide a last-row overread.
    uint16_t *map = (uint16_t *) (memory + page - map_bytes);
    memset(map, 0, map_bytes);
    mlt_frame a = image_frame(width, image_height, format, 0);
    mlt_frame b = image_frame(width, image_height, format, 1);
    CHECK(a && b);
    if (format == mlt_image_yuv422) {
        int w = width, h = image_height;
        luma_composite_yuv422(a, b, map_width, map_height, map, .5, .01, .1, field_order, &w, &h, 0, 0, 1);
    } else if (format == mlt_image_rgba) {
        CHECK(luma_composite_rgba32(a, b, map_width, map_height, map, .5, .01, .1, field_order, width, image_height, 0, 1) == 0);
    } else {
        CHECK(luma_composite_rgba64(a, b, map_width, map_height, map, .5, .01, .1, field_order, width, image_height, 0, 1) == 0);
    }
    uint8_t *actual = NULL, *expected = NULL;
    int w = width, h = image_height;
    mlt_image_format actual_format = format, expected_format = format;
    CHECK(mlt_frame_get_image(a, &actual, &actual_format, &w, &h, 0) == 0);
    CHECK(mlt_frame_get_image(b, &expected, &expected_format, &w, &h, 0) == 0);
    CHECK(actual && expected && actual_format == format && expected_format == format);
    const int bytes = format == mlt_image_yuv422 ? 2 : format == mlt_image_rgba ? 4 : 8;
    CHECK(memcmp(actual, expected, (size_t) width * image_height * bytes) == 0);
    mlt_frame_close(a);
    mlt_frame_close(b);
    CHECK(munmap(memory, page * 2) == 0);
    return 0;
}

int main(void)
{
    const int heights[] = {1, 2, 3, 7, 24};
    const mlt_image_format formats[] = {mlt_image_yuv422, mlt_image_rgba, mlt_image_rgba64};
    int count = 0;
    for (int image = 0; image < 5; ++image)
        for (int map = 0; map < 5; ++map)
            for (int field = -1; field <= 1; ++field)
                for (int format = 0; format < 3; ++format) {
                    CHECK(bounded_case(heights[image], heights[map], field, formats[format]) == 0);
                    ++count;
                }
    printf("Passed %d guarded luma cases across YUV422, RGBA and RGBA64.\n", count);
    return 0;
}
