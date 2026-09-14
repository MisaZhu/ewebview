/*
 * svg.c - thin one-shot rasteriser over plutosvg/plutovg.
 *
 * Vendored from EwokOS system/gui/libs/libsvg with the EwokOS-only
 * openlibm fenv round-mode dance dropped (it papered over an openlibm
 * x86_64 quirks; host toolchains round to nearest already).
 *
 * svg_load_from_memory() parses an SVG document, rasterises it at its
 * intrinsic size into a plutovg surface (anti-aliased FT smooth raster)
 * and hands back a copy of the ARGB32 pixel buffer. The pixels come out
 * PREMULTIPLIED (plutovg's internal format); callers that feed them to a
 * non-premultiplied blender must unpremultiply first.
 */

#include "svg.h"
#include "plutosvg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

svg_image_t* svg_load_from_memory(const uint8_t* data, uint32_t size) {
    svg_image_t* img;
    plutosvg_document_t* doc;
    plutovg_surface_t* surface;
    const unsigned char* src_pixels;
    size_t pixel_bytes;
    int width, height, surf_width, surf_height;

    if (data == NULL || size == 0)
        return NULL;

    doc = plutosvg_document_load_from_data((const char*)data, (int)size, -1.0f, -1.0f, NULL, NULL);
    if (doc == NULL)
        return NULL;

    width = (int)plutosvg_document_get_width(doc);
    height = (int)plutosvg_document_get_height(doc);
    if (width <= 0 || height <= 0) {
        plutosvg_document_destroy(doc);
        return NULL;
    }

    surface = plutosvg_document_render_to_surface(doc, NULL, width, height, NULL, NULL, NULL);
    plutosvg_document_destroy(doc);
    if (surface == NULL)
        return NULL;

    surf_width = plutovg_surface_get_width(surface);
    surf_height = plutovg_surface_get_height(surface);

    img = (svg_image_t*)malloc(sizeof(svg_image_t));
    if (img == NULL) {
        plutovg_surface_destroy(surface);
        return NULL;
    }

    img->width = (uint32_t)surf_width;
    img->height = (uint32_t)surf_height;

    pixel_bytes = (size_t)surf_width * surf_height * sizeof(uint32_t);
    img->pixels = (uint32_t*)malloc(pixel_bytes);
    if (img->pixels == NULL) {
        free(img);
        plutovg_surface_destroy(surface);
        return NULL;
    }

    src_pixels = plutovg_surface_get_data(surface);
    memcpy(img->pixels, src_pixels, pixel_bytes);

    plutovg_surface_destroy(surface);
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
