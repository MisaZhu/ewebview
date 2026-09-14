/*
 * svg.c - thin rasteriser over plutosvg/plutovg.
 *
 * Vendored from EwokOS system/gui/libs/libsvg with the EwokOS-only
 * openlibm fenv round-mode dance dropped (it papered over an openlibm
 * x86_64 quirks; host toolchains round to nearest already).
 *
 * Two entry styles over the same parser: svg_load_from_memory() is the
 * one-shot raster at the intrinsic size, while svg_doc_load() keeps the
 * parsed document so svg_doc_render() can rasterise it at ANY target
 * size - SVG is vector data, so scaling stays anti-aliased at the exact
 * destination resolution (the FT smooth raster runs per render) instead
 * of resampling one fixed bitmap. All returned pixels are STRAIGHT-alpha
 * ARGB8888 (plutovg's premultiplied output is unpremultiplied here), the
 * layout SDL-style non-premultiplied blenders expect.
 */

#include "svg.h"
#include "plutosvg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct svg_doc {
    plutosvg_document_t* doc;
    /* plutosvg is zero-copy: attribute values (path 'd', points, ...) stay as
     * views into the source text and are only parsed to geometry at RENDER
     * time, so the buffer must outlive the document. Keep a private copy so a
     * doc handed back to a caller (e.g. re-rasterised later on another thread)
     * never dangles once the caller's network/decode buffer is freed. */
    char* data;
};

/* plutovg composites premultiplied; divide the colour back out so callers
 * can feed the pixels to straight-alpha blenders (SDL_BLENDMODE_BLEND). */
static void svg_unpremultiply(uint32_t* pixels, size_t count) {
    size_t i;
    for (i = 0; i < count; i++) {
        uint32_t p = pixels[i];
        uint32_t a = (p >> 24) & 0xFF;
        uint32_t r, g, b;
        if (a == 0) { pixels[i] = 0; continue; }
        if (a == 255) continue;
        r = ((p >> 16) & 0xFF) * 255 / a; if (r > 255) r = 255;
        g = ((p >>  8) & 0xFF) * 255 / a; if (g > 255) g = 255;
        b = ( p        & 0xFF) * 255 / a; if (b > 255) b = 255;
        pixels[i] = (a << 24) | (r << 16) | (g << 8) | b;
    }
}

static svg_image_t* svg_image_from_surface(plutovg_surface_t* surface) {
    svg_image_t* img;
    const unsigned char* src_pixels;
    int width, height;

    width = plutovg_surface_get_width(surface);
    height = plutovg_surface_get_height(surface);

    img = (svg_image_t*)malloc(sizeof(svg_image_t));
    if (img == NULL)
        return NULL;

    img->width = (uint32_t)width;
    img->height = (uint32_t)height;

    img->pixels = (uint32_t*)malloc((size_t)width * height * sizeof(uint32_t));
    if (img->pixels == NULL) {
        free(img);
        return NULL;
    }

    src_pixels = plutovg_surface_get_data(surface);
    if (plutovg_surface_get_stride(surface) == width * 4) {
        memcpy(img->pixels, src_pixels, (size_t)width * height * sizeof(uint32_t));
    } else {
        int y;
        for (y = 0; y < height; y++)
            memcpy(img->pixels + (size_t)y * width,
                   src_pixels + (size_t)y * plutovg_surface_get_stride(surface),
                   (size_t)width * sizeof(uint32_t));
    }
    svg_unpremultiply(img->pixels, (size_t)width * height);
    return img;
}

svg_doc_t* svg_doc_load(const uint8_t* data, uint32_t size) {
    svg_doc_t* doc;
    if (data == NULL || size == 0)
        return NULL;
    doc = (svg_doc_t*)malloc(sizeof(svg_doc_t));
    if (doc == NULL)
        return NULL;
    doc->data = (char*)malloc((size_t)size + 1);
    if (doc->data == NULL) {
        free(doc);
        return NULL;
    }
    memcpy(doc->data, data, size);
    doc->data[size] = '\0';
    doc->doc = plutosvg_document_load_from_data(doc->data, (int)size, -1.0f, -1.0f, NULL, NULL);
    if (doc->doc == NULL) {
        free(doc->data);
        free(doc);
        return NULL;
    }
    return doc;
}

void svg_doc_get_size(const svg_doc_t* doc, int* width, int* height) {
    if (width) *width = doc ? (int)plutosvg_document_get_width(doc->doc) : 0;
    if (height) *height = doc ? (int)plutosvg_document_get_height(doc->doc) : 0;
}

svg_image_t* svg_doc_render(const svg_doc_t* doc, int width, int height) {
    plutovg_surface_t* surface;
    svg_image_t* img;
    if (doc == NULL || doc->doc == NULL)
        return NULL;
    surface = plutosvg_document_render_to_surface(doc->doc, NULL, width, height, NULL, NULL, NULL);
    if (surface == NULL)
        return NULL;
    img = svg_image_from_surface(surface);
    plutovg_surface_destroy(surface);
    return img;
}

void svg_doc_free(svg_doc_t* doc) {
    if (doc) {
        plutosvg_document_destroy(doc->doc);
        free(doc->data);
        free(doc);
    }
}

svg_image_t* svg_load_from_memory(const uint8_t* data, uint32_t size) {
    svg_doc_t* doc;
    svg_image_t* img;
    int width, height;

    doc = svg_doc_load(data, size);
    if (doc == NULL)
        return NULL;

    svg_doc_get_size(doc, &width, &height);
    if (width <= 0 || height <= 0) {
        svg_doc_free(doc);
        return NULL;
    }

    img = svg_doc_render(doc, width, height);
    svg_doc_free(doc);
    return img;
}

svg_image_t* svg_load(const char* filename) {
    FILE* fp;
    long file_size;
    uint8_t* data;
    svg_image_t* img;

    fp = fopen(filename, "rb");
    if (fp == NULL)
        return NULL;

    fseek(fp, 0, SEEK_END);
    file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (file_size <= 0) {
        fclose(fp);
        return NULL;
    }

    data = (uint8_t*)malloc((size_t)file_size);
    if (data == NULL) {
        fclose(fp);
        return NULL;
    }

    if (fread(data, 1, (size_t)file_size, fp) != (size_t)file_size) {
        free(data);
        fclose(fp);
        return NULL;
    }
    fclose(fp);

    img = svg_load_from_memory(data, (uint32_t)file_size);
    free(data);
    return img;
}

void svg_free(svg_image_t* img) {
    if (img) {
        free(img->pixels);
        free(img);
    }
}
