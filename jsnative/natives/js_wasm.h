/*
 * js_wasm.h - WebAssembly natives for the mario JavaScript VM.
 *
 * Fifth bridge in the set (js_dom.h = Document/Element, js_event.h =
 * Event/EventTarget, js_web.h = BOM, js_canvas.h = Canvas 2D, this one =
 * the WebAssembly JS API). Backed by the embedded easm runtime
 * (easm/ in the repo root), which provides decode / validate / interpreter /
 * aarch64 baseline JIT / WASI for the guest modules.
 *
 * Pure C; knows nothing about WidgetWebview, litehtml or EwokOS. The bridge
 * needs no embedder callbacks: it owns a per-VM EaStore (freed with the VM)
 * and resolves imports from the JS import object passed to
 * `new WebAssembly.Instance(module, imports)`. WASI imports
 * (module name "wasi_snapshot_preview1") resolve to the built-in host
 * instance automatically.
 *
 * Registration order:
 *   vm_init() -> js_register_dom_natives() -> js_register_canvas_natives()
 *             -> js_register_event_natives() -> js_register_web_natives()
 *             -> js_register_wasm_natives()
 * (js_register_wasm_natives only needs vm->root; the DOM/window bridges may
 * or may not have run - the namespace is mirrored onto `window` when one
 * exists.)
 *
 * Supported surface:
 *   WebAssembly.validate(bytes) -> bool
 *   WebAssembly.compile(bytes) -> Promise<Module>   (synchronous resolve)
 *   WebAssembly.instantiate(moduleOrBytes, imports) -> Promise<{module,instance}>
 *   WebAssembly.instantiateStreaming / compileStreaming -> rejected Promise
 *       (streaming compilation is not supported; use the bytes overloads)
 *   new WebAssembly.Module(bytes)
 *       module.exports / module.imports (descriptor arrays)
 *       WebAssembly.Module.customSections(module, name)
 *   new WebAssembly.Instance(module, imports)
 *       instance.exports (functions / Memory / Table / Global wrappers)
 *   new WebAssembly.Memory({initial, maximum})
 *       memory.buffer (live ArrayBuffer view), memory.grow(delta)
 *   new WebAssembly.Table({element, initial, maximum})
 *       table.length / get / set / grow
 *   new WebAssembly.Global({value, mutable}, value)
 *       global.value get/set
 *   WebAssembly.CompileError / LinkError / RuntimeError (Error subclasses)
 *
 * Known limitations (deliberate, not oversights):
 *   - Module compilation and instantiation are synchronous on the caller's
 *     thread (no worker thread); `compile`/`instantiate` return already
 *     resolved Promises.
 *   - Streaming compilation is not implemented; the *Streaming entry points
 *     return a rejected Promise.
 *   - SharedArrayBuffer memory (descriptor `shared: true`) is accepted but
 *     the sharing is not exposed to JS threads (the engine is single-threaded
 *     per page anyway).
 *   - externref table/global values are stored by JS object pointer and are
 *     NOT rooted by the runtime: the embedder must keep the JS object alive
 *     while the wasm side holds the reference.
 *   - Only one guest instance can use WASI at a time (the WASI context is
 *     bound to the most recently instantiated instance), mirroring the easm
 *     CLI.
 */

#ifndef MARIO_JS_WASM_H
#define MARIO_JS_WASM_H

#include "mario.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Install the WebAssembly namespace, classes and natives on vm->root (and on
 * `window` when the DOM bridge already created one). Safe to call after
 * js_register_dom_natives(); call it once per VM. Returns false on failure. */
bool js_register_wasm_natives(vm_t* vm);

#ifdef __cplusplus
}
#endif

#endif /* MARIO_JS_WASM_H */
