/*
 * main_headless.c - headless (non-SDL) ewebview driver.
 *
 * Purpose-built for the AddressSanitizer teardown-corruption investigation:
 * SDL builds cannot run under ASan on this machine (the libSDL2 dyld load
 * deadlocks against the ASan runtime), so this binary drives the engine
 * directly through the headless port with no SDL anywhere in the process.
 *
 * Lifecycle: create -> set listener/viewport/js -> load(url) -> tick at 60Hz
 * for the requested number of seconds (hydration) -> destroy. The destroy
 * path runs the engine teardown (resetJsVm -> vm_close -> full GC sweep),
 * which is where the corruption was observed; under ASan the *write* that
 * corrupts the var is reported at hydration time instead.
 *
 * Usage:
 *   hlbrowser <url> [seconds] [--js off] [--no-load]
 *
 * Env knobs:
 *   HL_TICK_MS   per-tick sleep in ms (default 16, i.e. ~60Hz)
 *   HL_LOG_STAT  if set, print a one-line status every 5s
 */

#include <ewebview.h>
#include <ewebview_port.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void eweb_port_headless(eweb_port_t* port, void* ud);

typedef struct {
	ewebview_t* view;
	uint32_t    frames;
	uint32_t    status_lines;
} hl_state_t;

static void hl_on_frame(void* ud, eweb_surface_t* frame,
		int frame_scroll_x, int frame_scroll_y, int doc_w, int doc_h) {
	hl_state_t* st = (hl_state_t*)ud;
	st->frames++;
	if(frame != NULL)
		ewebview_release_frame(st->view, frame);
}

static void hl_on_status(void* ud, const char* text, int progress) {
	hl_state_t* st = (hl_state_t*)ud;
	st->status_lines++;
	if(getenv("HL_LOG_STAT") != NULL && text != NULL)
		fprintf(stderr, "[stat] %s\n", text);
	(void)progress;
}

static void hl_on_title(void* ud, const char* title) {
	fprintf(stderr, "[title] %s\n", (title != NULL) ? title : "");
	(void)ud;
}

int main(int argc, char** argv) {
	const char* url = NULL;
	uint32_t seconds = 60;
	bool js_enabled = true;
	bool do_load = true;

	for(int i = 1; i < argc; i++) {
		if(strcmp(argv[i], "--js") == 0 && i + 1 < argc) {
			js_enabled = (strcmp(argv[++i], "off") != 0);
		} else if(strcmp(argv[i], "--no-load") == 0) {
			do_load = false;
		} else if(argv[i][0] != '-' && url == NULL) {
			url = argv[i];
		} else if(argv[i][0] != '-') {
			seconds = (uint32_t)atoi(argv[i]);
			if(seconds == 0) seconds = 60;
		}
	}

	if(url == NULL && do_load) {
		fprintf(stderr, "usage: hlbrowser <url> [seconds] [--js off] [--no-load]\n");
		return 2;
	}

	uint32_t tick_ms = 16;
	const char* tm = getenv("HL_TICK_MS");
	if(tm != NULL && atoi(tm) > 0)
		tick_ms = (uint32_t)atoi(tm);

	eweb_port_t port;
	memset(&port, 0, sizeof(port));
	eweb_port_headless(&port, NULL);

	hl_state_t st;
	memset(&st, 0, sizeof(st));

	ewebview_t* view = ewebview_create(&port);
	if(view == NULL) {
		fprintf(stderr, "hlbrowser: ewebview_create failed\n");
		return 1;
	}
	st.view = view;

	eweb_listener_t listener;
	eweb_listener_init(&listener);
	listener.ud = &st;
	listener.on_frame = hl_on_frame;
	listener.on_status = hl_on_status;
	listener.on_title = hl_on_title;
	ewebview_set_listener(view, &listener);

	ewebview_set_viewport(view, 1280, 900);
	ewebview_set_js_enabled(view, js_enabled);

	if(do_load)
		ewebview_load(view, url);

	uint64_t start = port.clock.tic_ms(port.clock.ud);
	uint64_t deadline = start + (uint64_t)seconds * 1000ull;
	uint64_t last_log = start;

	while(port.clock.tic_ms(port.clock.ud) < deadline) {
		ewebview_tick(view);
		if(getenv("HL_LOG_STAT") != NULL) {
			uint64_t now = port.clock.tic_ms(port.clock.ud);
			if(now - last_log >= 5000) {
				last_log = now;
				fprintf(stderr, "[hl] t=%llus frames=%u idle=%d\n",
					(unsigned long long)((now - start) / 1000ull),
					st.frames, (int)ewebview_is_idle(view));
			}
		}
		if(port.clock.sleep_ms != NULL)
			port.clock.sleep_ms(port.clock.ud, tick_ms);
	}

	fprintf(stderr, "[hl] destroying after %us (frames=%u, idle=%d)\n",
		seconds, st.frames, (int)ewebview_is_idle(view));
	ewebview_destroy(view);
	fprintf(stderr, "[hl] destroyed cleanly\n");
	return 0;
}
