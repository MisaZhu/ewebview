#ifndef SVG_H
#define SVG_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t* pixels;
} svg_image_t;

/* Parsed SVG document kept alive for repeated rasterisation at arbitrary
 * target sizes: SVG is vector data, so every render is anti-aliased at the
 * exact destination resolution instead of resampling one fixed raster. */
typedef struct svg_doc svg_doc_t;

svg_image_t* svg_load(const char* filename);

svg_image_t* svg_load_from_memory(const uint8_t* data, uint32_t size);

void svg_free(svg_image_t* img);

/* Parses and KEEPS the document. The handle owns a private copy of the source
 * text (plutosvg resolves path/point data from it lazily at render time), so
 * the caller may free `data` as soon as this returns. */
svg_doc_t* svg_doc_load(const uint8_t* data, uint32_t size);

/* Intrinsic size in CSS px (absolute width/height, else the viewBox). */
void svg_doc_get_size(const svg_doc_t* doc, int* width, int* height);

/* Rasterise the whole document at exactly width x height pixels (<=0 falls
 * back to the intrinsic size). Returns straight-alpha ARGB8888 pixels. */
svg_image_t* svg_doc_render(const svg_doc_t* doc, int width, int height);

void svg_doc_free(svg_doc_t* doc);

#ifdef __cplusplus
}
#endif

#endif