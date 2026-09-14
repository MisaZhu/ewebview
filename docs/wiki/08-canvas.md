# Chapter 8 · Canvas 2D

> Language: **English** | [中文](08-canvas.zh.md)

Canvas 2D spans three files with an extremely strict division of responsibilities:

| Layer | File | Owns what |
| --- | --- | --- |
| Bridge (pure C) | `jsnative/natives/js_canvas.c` | **All** `CanvasRenderingContext2D` state and geometry: CTM, clip stack, path building, arc/bezier subdivision, even-odd scanline fill, polyline stroking, dashes, gradient and shadow evaluation, composite modes |
| Canvas (C++) | `ewebview/src/EWebCanvas.{h,cc}` | An offscreen ARGB surface + methods mapping "finished primitives" 1:1 onto the porting tables, **doing no geometry** |
| Glue (C++) | `ewebview/src/EWebCanvasGlue.cc` | C callback trampolines, the canvas registry, `drawImage` image resolution, compositing back to the page |

```
JS: ctx.translate(x,y); ctx.bezierCurveTo(...); ctx.fill()
   │
   ▼ js_canvas.c (mario objects + geometry state machine, platform-agnostic)
   CTM transform → curve subdivision → scanline fill → gradient/shadow evaluation
   │  emits only "a single finished primitive in device coordinates"
   ▼ js_canvas_callbacks_t (every callback OPTIONAL)
   EWebCanvasGlue.cc trampoline: cast the handle back to EWebCanvas*, forward the method
   │
   ▼ EWebCanvas methods (1:1 mapping, see 8.3)
   gfx.fill_rect / gfx.wline / gfx.stroke_bezier / font.draw_text ...
```

Why all geometry is in the bridge: the porting tables thereby stay minimal (Ch. 10's gfx table has only basic primitives like rect/line/circle/curve), and the engine does not re-implement Canvas semantics — a new platform can run Canvas without understanding Canvas.

## 8.1 Canvas Registry & Lifecycle

The engine holds `m_jsCanvases` (`std::vector<EWebCanvas*>`), keyed by the `<canvas>` element's **id**:

- **Lazy creation**: on the first `canvas.getContext('2d')` the bridge calls `canvas_create(id, w, h)` → `getOrCreateCanvas()` looks up by id and only `new EWebCanvas` if absent. The same id always returns the same canvas — `getContext` is idempotent.
- **Default size**: the constructor clamps non-positive width/height to the HTML default **300×150**.
- **Destroyed with the page**: `cleanupBuildResources()` (on navigation/page change) calls `freeCanvases()` to `delete` them uniformly; the destructor releases the bitmap and the lazy font. The bridge's per-context state is freed by `vm_close()`.
- **Anonymous bitmaps**: the backing store of `ImageData`, `CanvasPattern` sources, and the source of `drawImage(otherCanvas)` are raw `eweb_surface_t*` held by the bridge itself (requested directly from the gfx table via `bitmap_create/free/dims/data`, cleared to fully transparent at creation). During compositing, a composite key starting with `'@'` means "no corresponding element" and is skipped (see 8.5).

## 8.2 Threading Model

All Canvas callbacks **run synchronously inside one VM run on the engine thread** (a script calling `fillRect` is a single function call straight through to the porting table). The engine exclusively owns the VM, document, and surfaces, so **no locking is needed or allowed here**. The painted result waits for the next mark-dirty relayout/repaint (`jsMarkLayoutDirty`) to reach the screen with a frame — Canvas has no independent "immediate on-screen" channel.

## 8.3 Primitive Mapping Table

When a callback reaches `EWebCanvas`, coordinates are already in device space and operations are already single finished primitives (the `EWebCanvas.h` header comment is the spec):

| EWebCanvas method | Lands on the porting table |
| --- | --- |
| `fillRect` | `gfx.fill_rect` |
| `strokeRect` | 4× `gfx.wline` (`gfx.rect` is only 1px) |
| `drawLine` | `gfx.wline` (with width) |
| `fillCircle` / `strokeCircle` | `gfx.fill_circle` / `gfx.circle(rw=lw)` |
| `fillArc` / `strokeArc` | `gfx.fill_arc` / `gfx.arc(rw=lw)` |
| `fillRoundRect` / `strokeRoundRect` | `gfx.fill_round` / `gfx.round(rw=lw)` |
| `strokeQuadratic` / `strokeBezier` | `gfx.stroke_quadratic` / `gfx.stroke_bezier` |
| `setPixel` / `getPixel` | `gfx.set_pixel` / `gfx.get_pixel` |
| `blit` | `gfx.blit_fit_alpha` |
| `drawText` | `font.draw_text` |
| `strokeText` | 5× `font.draw_text` (4 offsets + center, a stroke approximation) |
| `textSize` | `font.text_size` |
| `setClip` / `clearClip` | `gfx.surface_set_clip` / `gfx.surface_unset_clip` |

**Single source of curve subdivision**: when the bridge needs to scanline-**fill** a path itself, it must first flatten the curves into device-space polylines; this de Casteljau subdivision is not duplicated in the bridge but calls back the porting table's `flatten_quadratic` / `flatten_cubic` (marked OPTIONAL in the HAL) — the subdivision implementation exists in exactly one place, the porting layer. When the hook is missing it degrades to a **straight chord** (returning only the endpoint); filling still works, the curve just becomes a polyline.

The `fill_polygon` / `stroke_polygon` callbacks are **deliberately left NULL**: the bridge's scanline-fill/polyline-stroke path already breaks them into a `fill_rect`/`draw_line` sequence, doing the same work as the porting layer; not implementing them keeps the callback table minimal.

## 8.4 Text

Canvas text is rare enough that `EWebCanvas`'s font handle is **created only when used** (`ensureFont()`); a canvas that writes no text has zero font overhead. `fillText`/`strokeText`/`measureText` all land on `font.draw_text`/`font.text_size`; text styles like `ctx.font`, `textAlign`/`textBaseline`, `letterSpacing` are parsed and evaluated by the bridge, and the engine only receives "draw this string at this place at this size".

## 8.5 drawImage & Image-Cache Reuse

The image source of `drawImage(img, ...)` (all 3/5/9-argument forms supported) is resolved via `bitmap_from_element`:

1. the element handle is the `litehtml::element*` the DOM bridge stores on the Element instance;
2. read its `src` attribute and resolve to an absolute URL using the **container's** base URL (`resolveUrl`);
3. query **the same image cache shared with litehtml layout** (`EWebContainer::getImage`, see 5.5) — an already-downloaded-and-decoded `<img>` bitmap is reused at zero cost;
4. the returned `eweb_surface_t*` is **owned by the container**; the bridge must not free it.

The container differs between construction and the live phase: while a build is in progress (a script running in `BUILD_RUN_JS`) it queries `m_buildContainer`, otherwise the visible page's `m_container`.

## 8.6 Compositing Back to the Page

Canvas bitmaps do not participate in litehtml's draw traversal; instead the engine **composites them a second time** at the end of each frame render (`compositeCanvases(cache, -m_engineScrollX, -m_engineScrollY)` in `ewebview.cc`):

```
for each EWebCanvas in m_jsCanvases:
    id empty or starting with '@' (anonymous bitmap) → skip
    el = m_doc->root()->select_one("#" + id)   ← is the canvas element still on the tree
    p  = el->get_placement()                    ← its laid-out position
    gfx.blit(canvas bitmap → frame cache, target = origin + p.x/p.y)
```

Implications and trade-offs:

- compositing happens **after** litehtml `draw()`, so canvas pixels always cover the element box's background/borders — approximating the paint order of a "replaced element";
- after a script detaches the element from the tree, `select_one` comes up empty and the canvas automatically stops displaying (the bitmap itself stays alive until page change);
- it depends on `gfx.blit`: when that callback is NULL the whole compositing is a no-op (a pure-markup page is unaffected);
- every full-frame repaint re-composites, so scrolling is naturally correct — the canvas sits in the document coordinate system and is scrolled along with the page.

## 8.7 Supported JS Surface

The bridge implements the **complete `CanvasRenderingContext2D`** per the WHATWG spec (the "Supported surface" header of `js_canvas.h`): the state stack, all transforms (including `getTransform`/`DOMMatrix`), `globalAlpha`/`globalCompositeOperation` (composite modes passed to the engine as an enum; unsupported modes may be ignored), fill/stroke styles (color | CanvasGradient | CanvasPattern), line cap/join/dash, the four shadow properties, all path methods + `Path2D` (including `addPath`), `fill/stroke/clip/isPointInPath/isPointInStroke`, `fillText/strokeText/measureText` and the text-style family, the three argument forms of `drawImage`, `createImageData/getImageData/putImageData`, linear/radial/conic gradients, `createPattern` + `setTransform`.

Known gaps: the `filter` property accepts assignment but is currently ignored; no `toDataURL` (needs an image encoder, the HAL only decodes); no WebGL.

## 8.8 Design Points Recap

- **Geometry in the bridge, rasterization in the porting layer**: porting a platform needs no understanding of Canvas semantics, only the basic primitives rect/line/circle/curve/bitmap/text;
- **All-integer output inside the bridge**: every callback reaching the engine is device coordinates + `0xAARRGGBB`;
- **Zero extra threads/locks**: Canvas painting is part of a VM run, naturally serial;
- **Reuse rather than build anew**: images go through the container cache, bitmaps through the gfx table, text through the font table — Canvas introduces no new category of platform capability.
