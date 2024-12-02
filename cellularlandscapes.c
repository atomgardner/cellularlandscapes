#define _POSIX_SOURCE 200112L
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/input-event-codes.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client-protocol.h>

#include "xdg-shell-protocol.h"

#define fail_wl_init        "couldn't init all necessary wayland components.\n"
#define fail_landscape_init "couldn't init the desired landscape.\n"

#define STEP_TIME_MSEC 150

struct landscape {
	// geometry
	size_t width, height; // [cells]
	void (*quotient)(struct landscape *landscape,
			int x, int y, size_t *X, size_t *Y);

	// dynamics
	uint32_t rule;
	void (*automata)(struct landscape *, int, int, uint32_t);

	// flip and flop hold the state of the landscape
	uint8_t *show, *flip, *flop;

	// aesthetics
	size_t cell_width, cell_height, cell_wall;
};

struct buffer {
	struct wl_buffer *wl_buffer;
	uint32_t *pixels;
	uint8_t busy:1;
};

// The first crack kept everything local and nice. But I'm in a global kind of
// mood. And this way the wl stays singleton (not that it really matters.)
struct {
	struct wl_display    *display;
	struct wl_compositor *compositor;
	struct wl_shm        *shm;
	struct wl_surface    *surf;
	struct xdg_wm_base   *wm;
	struct xdg_surface   *xdg_surf;
	struct xdg_toplevel  *xdg_top;
	struct wl_seat       *seat;
	struct wl_pointer    *pointer;
	struct wl_keyboard   *keyboard;
	struct wl_callback   *frame_cb;

	struct buffer buffers[2];

	int32_t pointer_cell;
	uint32_t pointer_held; // bool

	uint32_t running:1,
		paused:1,
		wait_for_config:1,
		redraw:1;

	void (*brush)(struct landscape *landscape, int x, int y);
} wl;

enum { cell_off, cell_on, cell_cursor };
uint32_t state_colours[] = {
	[cell_off]	= 0x80000000,
	[cell_on]	= 0x80ffffff,
	[cell_cursor]	= 0x80ffff00,
};

void frame_listener_done(void *data, struct wl_callback *callback, uint32_t time);

void render(void *data);
void landscape_draw(struct landscape *landscape, struct buffer *buffer);
uint8_t landscape_get(struct landscape *b, int x, int y);
void landscape_set(struct landscape *b, int x, int y, uint8_t val);
size_t landscape_count_neighbours(struct landscape *b, int x, int y);

void brush_default(struct landscape *landscape, int x, int y);
void brush_conway_glider(struct landscape *landscape, int x, int y);

void quotient_torus(struct landscape *l, int x, int y, size_t *X, size_t *Y);
// TODO:
// void quotient_mobius(struct landscape *l, int x, int y, size_t *X, size_t *Y);
// void quotient_klein(struct landscape *l, int x, int y, size_t *X, size_t *Y);
// void quotient_schwartzschild(struct landscape *l, int x, int y, size_t *X, size_t *Y); lol????

#define birth_bit(x) ((uint32_t)1 << (9 + (x)))
#define survive_bit(x) ((uint32_t)1 << (x))
void print_lifelike_rule(uint32_t rule)
{
	fprintf(stdout, "B");
	for (int i = 0; i < 9; i++)
		if (!!(rule & birth_bit(i)))
			fprintf(stdout, "%d", i);
	fprintf(stdout, "/S");
	for (int i = 0; i < 9; i++)
		if (!!(rule & (1 << i)))
			fprintf(stdout, "%d", i);
	fprintf(stdout, "\n");
}

uint32_t conway = birth_bit(3) | survive_bit(2) | survive_bit(3);

// The space of 2d nearest-neighbour automata is huge: 2**(2**9) distinct
// members.
//
// To make exploring the space a bit easier, we restrict to the update rules
// that depend only on neighbour counts, the life-like automata.
//
// A way to specify life-like automata is `Bx/Sy`, where x and y are each an
// array of 8 bits.
//
// The bits in x say when a dead cell is born. If the live neighbour count of
// the cell is an element of x the cell switches to living. y has the same
// meaning for living cells and their survival.
//
// Conway's Game of Life is the automata B3/S23. It encodes as:
// 	[b8, b7, ... b0, s8, ... s0] = 18'b000001000 000001100
void twod_life_like(struct landscape *landscape, int x, int y, uint32_t rule)
{
	size_t n = landscape_count_neighbours(landscape, x, y);

	if (landscape_get(landscape, x, y))
		landscape_set(landscape, x, y, !!(survive_bit(n) & rule));
	else
		landscape_set(landscape, x, y, !!(birth_bit(n) & rule));
}

// The `rule` parameter is an 8 bit array that uses the states of the cell at
// (x,y) and its neighbours, (x-1,y) and (x+1,y), to determine the state of the
// cell at (x, y+1).
// This is easier to see with an illustration. The following diagram defines the
// rule, `a << 7 | b << 6 | ... | h`.
//
//			xxx xx- x-x x-- -xx -x- --x ---
//			 a   b   c   d   e   f   g   h
//
// . XXX: it might be interesting to explore the dynamics when the a..h are
// Bernoulli probabilities instead; i.e., take a = 1, b = 0.01, ..., h = 0.5
// 	Maybe covered in [arXiv:1010.3133]
void oned(struct landscape *landscape, int x, int y, uint32_t rule)
{
	if (y == 0 || x == 0 || x == landscape->width - 1) {
		int v = landscape_get(landscape, x, y);
		landscape_set(landscape, x, y, v);
		return;
	}

	uint8_t parent_pattern =  landscape_get(landscape,  x - 1, y-1) << 2
				| landscape_get(landscape,  x,     y-1) << 1
				| landscape_get(landscape,  x + 1, y-1);

	uint8_t update = !!((rule & 0xff) & (1 << parent_pattern));
	landscape_set(landscape, x, y, update);
}

static void buffer_release(void *data, struct wl_buffer *buf)
{
	struct buffer *buffer = data;
	buffer->busy = 0;
}

static const struct wl_buffer_listener buffer_listener = {
	.release = buffer_release,
};

int shm_create_buffer(struct landscape *landscape, struct buffer *buffer)
{
	int fd, ret;
	void *data;
	char tmpname[] = "/tmp/cellular-XXXXXX";
	struct wl_shm *shm = wl.shm;
	uint32_t width = landscape->width * landscape->cell_width;
	uint32_t height = landscape->height * landscape->cell_height;
	uint32_t size = 4 * width * height; // 4 bytes per pixel

	// fd = memfd_create("cellularlandscapes-shared", MFD_CLOEXEC | MFD_ALLOW_SEALING);
	fd = mkostemp(tmpname, O_CLOEXEC);

	if (fd > -1)
		unlink(tmpname);
	else
		return -1;

	do {
		ret = posix_fallocate(fd, 0, size);
	} while (ret < 0 && errno == EINTR);
	if (ret < 0) {
		close(fd);
		return -1;
	}

	data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		close(fd);
		return -1;
	}
	buffer->pixels = data;

	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	buffer->wl_buffer = wl_shm_pool_create_buffer(pool,
			0, width, height, 4*width, WL_SHM_FORMAT_ARGB8888);

	wl_buffer_add_listener(buffer->wl_buffer, &buffer_listener, buffer);
	wl_shm_pool_destroy(pool);
	close(fd);

	return 0;
}

// XXX: The double buffering is like never used
struct buffer *landscape_next_buffer(struct landscape *landscape)
{
	struct buffer *buf;

	if (!wl.buffers[0].busy)
		buf = &wl.buffers[0];
	else if (!wl.buffers[1].busy)
		buf = &wl.buffers[1];
	else
		return NULL;

	if (!buf->wl_buffer)
		if (shm_create_buffer(landscape, buf) < 0)
			return NULL;

	return buf;
}

void clamped(struct landscape *l, int x, int y, size_t *X, size_t *Y) {
	*X = x;
	*Y = y;

	if (x < 0)
		*X = 0;
	if (y < 0)
		*Y = 0;
	if (x >= l->width)
		*X = l->width-1;
	if (y >= l->height)
		*Y = l->height -1;
}

void quotient_torus(struct landscape *l, int x, int y, size_t *X, size_t *Y)
{
	if (y < 0)
		*Y = l->height - 1;
	else if (y > (int)l->height - 1)
		*Y = 0;
	else
		*Y = y;

	if (x > (int)l->width - 1)
		*X = 0;
	else if (x < 0)
		*X = l->width - 1;
	else
		*X = x;
}

// Sample from the data of the currently displayed buffer
uint8_t landscape_get(struct landscape *ls, int x, int y)
{
	size_t X, Y;

	ls->quotient(ls, x, y, &X, &Y);
	return ls->show[Y*ls->width + X];
}

// Set the next state
void landscape_set(struct landscape *ls, int x, int y, uint8_t val)
{
	size_t X, Y;

	ls->quotient(ls, x, y, &X, &Y);
	if (ls->show == ls->flip)
		ls->flop[Y*ls->width + X] = val;
	else
		ls->flip[Y*ls->width + X] = val;
}

void landscape_set_front(struct landscape *ls, int x, int y, uint8_t val)
{
	size_t X, Y;

	ls->quotient(ls, x, y, &X, &Y);
	ls->show[Y*ls->width + X] = val;
}


size_t landscape_count_neighbours(struct landscape *b, int x, int y)
{
	return	+ landscape_get(b, x,     y + 1)	// N
		+ landscape_get(b, x + 1, y + 1)	// NE
		+ landscape_get(b, x + 1, y)		// E
		+ landscape_get(b, x + 1, y - 1)	// SE
		+ landscape_get(b, x,     y - 1)	// S
		+ landscape_get(b, x - 1, y + 1)	// SW
		+ landscape_get(b, x - 1, y)		// W
		+ landscape_get(b, x - 1, y - 1);	// NW
}

void landscape_step(struct landscape *l)
{
	for (int x = 0; x < l->width; x++)
		for (int y = 0; y < l->height; y++)
			l->automata(l, x, y, l->rule);

	l->show = l->show == l->flip ? l->flop : l->flip;

	wl.redraw = 1;
}

void highlight_cell(struct landscape *landscape,
		uint32_t *pixels, int x, int y)
{
	size_t X, Y;
	landscape->quotient(landscape, x, y, &X, &Y);

	size_t tl = landscape->cell_width;
	size_t w = landscape->width * tl; // pixels per row
	for (size_t xx = x*tl; xx < (X + 1)*tl; xx++)
		for (size_t yy = Y * tl * w; yy < (Y + 1) * tl * w; yy += w)
			pixels[yy + xx] ^= 0x00000000;
}

void highlight_neighbourhood(struct landscape *ls, uint32_t *pixels, int x, int y)
{
	highlight_cell(ls, pixels, x - 1, y - 1);
	highlight_cell(ls, pixels, x    , y - 1);
	highlight_cell(ls, pixels, x + 1, y - 1);
	highlight_cell(ls, pixels, x - 1, y);
	highlight_cell(ls, pixels, x    , y);
	highlight_cell(ls, pixels, x + 1, y);
	highlight_cell(ls, pixels, x - 1, y + 1);
	highlight_cell(ls, pixels, x    , y + 1);
	highlight_cell(ls, pixels, x + 1, y + 1);
}

// TODO: make this easier to read and understand
void fill_cell(struct landscape *landscape,
		uint32_t *pixels, size_t x, size_t y, uint8_t state)
{
	size_t tl = landscape->cell_width;
	size_t w = landscape->width * tl; // pixels per row

	for (size_t xx = x*tl; xx < (x + 1)*tl; xx++)
		for (size_t yy = y * tl * w; yy < (y + 1) * tl * w; yy += w)
			pixels[yy + xx] = state_colours[state];
}

void landscape_draw(struct landscape *landscape, struct buffer *buffer)
{
	uint32_t *pixels = buffer->pixels;
	uint8_t *state = landscape->show;

	// TODO: only clear if state has changed
	memset(pixels, 0x00, 4*landscape->width*landscape->height);
	for (size_t y = 0; y < landscape->height; y++)
		for (size_t x = 0; x < landscape->width; x++)
			fill_cell(landscape, pixels, x, y,
				state[y * landscape->width + x]);

	if (wl.pointer_cell >= 0)
		fill_cell(landscape, pixels,
			wl.pointer_cell % landscape->width,
			wl.pointer_cell/landscape->width, cell_cursor);
}

void bind_globals(void *data, struct wl_registry *r, uint32_t name,
		const char *interface, uint32_t version)
{
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		wl.compositor = wl_registry_bind(r, name,
				&wl_compositor_interface, 4);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		wl.shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
		wl.wm = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
	} else if (strcmp(interface, wl_seat_interface.name) == 0) {
		wl.seat = wl_registry_bind(r, name, &wl_seat_interface, 4);
	}
}

void registry_remove(void *d, struct wl_registry *r, uint32_t n) { }

static const struct wl_registry_listener registry_listener = {
	.global = bind_globals,
	.global_remove = registry_remove,
};

static void xdg_wm_base_ping(void *data, struct xdg_wm_base *wm, uint32_t serial)
{
	xdg_wm_base_pong(wm, serial);
}

struct wl_callback_listener frame_listener = {
	.done = frame_listener_done,
};

static void xdg_surf_config(void *data, struct xdg_surface *surf, uint32_t serial)
{
	xdg_surface_ack_configure(wl.xdg_surf, serial);

	if (wl.wait_for_config) {
		wl.wait_for_config = 0;
		render(data);
	}
}

static const struct xdg_surface_listener xdg_surf_listener = {
	.configure = xdg_surf_config,
};

static const struct xdg_wm_base_listener wm_base_listener = {
	.ping = xdg_wm_base_ping,
};

void pointer_enter(void *data, struct wl_pointer *p, uint32_t serial, struct wl_surface *s,
		wl_fixed_t x, wl_fixed_t y)
{
	struct landscape *b = data;


	int32_t tmp = wl_fixed_to_int(y)/30 * b->width + wl_fixed_to_int(x)/30;
	if (tmp < 0 || tmp > b->width * b->height)
		return;
	wl.pointer_cell = tmp;

	wl.redraw = 1;
}

void pointer_motion(void *data, struct wl_pointer *p, uint32_t time,
		wl_fixed_t x, wl_fixed_t y)
{
	struct landscape *l;
	int32_t tmp;

	l = data;
	tmp = wl_fixed_to_int(y)/l->cell_width
		* l->width + wl_fixed_to_int(x)/l->cell_width;

	if (tmp < 0 || tmp >= l->width * l->height)
		return;
	wl.pointer_cell = tmp;

	// XXX: This is so racy.
	if (wl.pointer_held == BTN_LEFT)
		l->show[tmp] = cell_on;
	else if (wl.pointer_held == BTN_RIGHT)
		l->show[tmp] = cell_off;

	wl.redraw = 1;
}

void pointer_leave(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s)
{
}

void pointer_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t time,
		uint32_t button, uint32_t state)
{
	struct landscape *l = d;

	if (state == WL_POINTER_BUTTON_STATE_PRESSED)
		wl.pointer_held = button;
	else if (state == WL_POINTER_BUTTON_STATE_RELEASED)
		wl.pointer_held = 0;


	if (wl.pointer_cell >= 0 && wl.pointer_held == 0)
		// draw on release
		wl.brush(l, wl.pointer_cell % l->width, wl.pointer_cell/l->width);

	wl.redraw = 1;
}

void pointer_axis(void *d, struct wl_pointer *p, uint32_t time, uint32_t axis,
		wl_fixed_t value)
{
}

void pointer_frame(void *d, struct wl_pointer *p)
{
}

void pointer_axis_source(void *d, struct wl_pointer *p, uint32_t a)
{
}

void pointer_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a)
{
}

void pointer_axis_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t discrete)
{
}

struct wl_pointer_listener wl_pointer_listener = {
	.enter          = pointer_enter,
	.leave          = pointer_leave,
	.motion         = pointer_motion,
	.button         = pointer_button,
	.axis           = pointer_axis,
	.frame          = pointer_frame,
	.axis_source    = pointer_axis_source,
	.axis_stop      = pointer_axis_stop,
	.axis_discrete  = pointer_axis_discrete,
};

void seat_name(void *data, struct wl_seat *seat, const char *name)
{
}

static void keyboard_handle_keymap(void *data, struct wl_keyboard *keyboard, uint32_t format, int fd, uint32_t size)
{
	close(fd);
}

static void keyboard_handle_enter(void *data, struct wl_keyboard *keyboard, uint32_t serial, struct wl_surface *surface, struct wl_array *keys)
{
}

static void keyboard_handle_leave(void *data, struct wl_keyboard *keyboard, uint32_t serial, struct wl_surface *surface)
{
}


static void keyboard_handle_modifiers(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t mods_depressed, uint32_t mods_latched, uint32_t mods_locked, uint32_t group)
{
}

void landscape_debug(struct landscape *l, int32_t cell)
{
	int x = wl.pointer_cell % l->width;
	int y = wl.pointer_cell/l->width;

	fprintf(stderr, "%d %d %d\n%d %d %d\n%d %d %d\n\n",
			landscape_get(l, x - 1, y - 1),
			landscape_get(l, x,     y - 1),
			landscape_get(l, x + 1, y - 1),

			landscape_get(l, x - 1, y),
			landscape_get(l, x,     y),
			landscape_get(l, x + 1, y),

			landscape_get(l, x - 1, y + 1),
			landscape_get(l, x,     y + 1),
			landscape_get(l, x + 1, y + 1));
}

static void keyboard_handle_key(void *data, struct wl_keyboard *keyboard,
		    uint32_t serial, uint32_t time, uint32_t key,
		    uint32_t state)
{
	struct landscape *ls = data;

	if (key == KEY_R && state) {
		memset(ls->flip, 0, ls->width * ls->height);
		memset(ls->flop, 0, ls->width * ls->height);
		if (ls->automata == oned)
			ls->show[ls->width/2] = 1;
		wl.paused = 1;
	}

	if (key == KEY_P && state) {
		// getrandom(ls->cur, ls->width * ls->height, GRND_RANDOM);
		ls->show[wl.pointer_cell] = !ls->show[wl.pointer_cell];
	}

	if (key == KEY_G && state) {
		if (wl.brush == brush_default)
			wl.brush = brush_conway_glider;
		else
			wl.brush = brush_default;
	}

	if (key == KEY_J && state) {
		wl.pointer_cell += ls->width;
		wl.pointer_cell %= ls->width * ls->height;
	}

	if (key == KEY_K && state) {
		wl.pointer_cell -= ls->width - ls->width * ls->height;
		wl.pointer_cell %= ls->width * ls->height;
	}

	if (key == KEY_H && state) {
		wl.pointer_cell--;
	}


	if (key == KEY_L && state) {
		wl.pointer_cell++;
	}

	if (key == KEY_SPACE && state)
		wl.paused = !wl.paused;

	if (key == KEY_D && state)
		landscape_debug(ls, wl.pointer_cell);

	// TODO: step
	if (key == KEY_N && state) {
		if (!wl.paused)
			return
		landscape_step(ls);
	}

	if (key == KEY_ESC && state)
		wl.running = 0;

	if (key == KEY_C && state)
		ls->rule = conway;

	if (key == KEY_2 && state)
		ls->automata = twod_life_like;

	if (key == KEY_1 && state) {
		wl.paused = 1;
		ls->rule = 110;
		ls->automata = oned;
		ls->quotient = clamped;
	}

	if (key == KEY_EQUAL && state) {
		ls->rule++;
		if (ls->automata == twod_life_like)
			print_lifelike_rule(ls->rule);
		else
			fprintf(stderr, "wolfram number: %d\n", ls->rule & 0xff);

	}
	if (key == KEY_MINUS && state) {
		ls->rule--;
		if (ls->automata == twod_life_like)
			print_lifelike_rule(ls->rule);
		else
			fprintf(stderr, "wolfram number: %d\n", ls->rule & 0xff);
	}
}

static void keyboard_handle_repeat(void *data, struct wl_keyboard *k,
		int32_t rate, int32_t delay)
{
	// TODO: who wants to press +/- 256 times to see all the new kinds of
	// science!!
}

static const struct wl_keyboard_listener keyboard_listener = {
	.keymap = keyboard_handle_keymap,
	.enter = keyboard_handle_enter,
	.leave = keyboard_handle_leave,
	.key = keyboard_handle_key,
	.modifiers = keyboard_handle_modifiers,
	.repeat_info = keyboard_handle_repeat,
};

void seat_handle_caps(void *data, struct wl_seat *seat, uint32_t caps)
{
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !wl.pointer) {
		wl.pointer = wl_seat_get_pointer(wl.seat);
		wl_pointer_add_listener(wl.pointer, &wl_pointer_listener, data);
	} else if ((caps & WL_SEAT_CAPABILITY_POINTER) && wl.pointer) {
		wl_pointer_destroy(wl.pointer);
		wl.pointer = NULL;
	}

	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !wl.keyboard) {
		wl.keyboard = wl_seat_get_keyboard(seat);
		wl_keyboard_add_listener(wl.keyboard, &keyboard_listener, data);
	} else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && wl.keyboard) {
		wl_keyboard_destroy(wl.keyboard);
		wl.keyboard = NULL;
	}
}

static const struct wl_seat_listener wl_seat_listener = {
	.capabilities = seat_handle_caps,
	.name = seat_name,
};

void render(void *data) {
	struct landscape *landscape = data;
	struct buffer *buf = landscape_next_buffer(landscape);
	if (!buf)
		return;

	landscape_draw(landscape, buf);
	wl_surface_attach(wl.surf, buf->wl_buffer, 0, 0);
	wl_surface_damage_buffer(wl.surf, 0, 0, INT32_MAX, INT32_MAX);
	wl.frame_cb = wl_surface_frame(wl.surf);
	wl_callback_add_listener(wl.frame_cb, &frame_listener, landscape);

	wl_surface_commit(wl.surf);

	wl.redraw = 0;
	buf->busy = 1;
}

void frame_listener_done(void *data, struct wl_callback *callback, uint32_t time) {
	wl_callback_destroy(callback);
	if (!wl.paused && wl.redraw) {
		render(data);
	}
}


int check_registry(void)
{
	int ret = 0;

	if (!wl.compositor)
		fprintf(stderr, "missing compositor\n"), ret++;
	if (!wl.shm)
		fprintf(stderr, "missing shm\n"), ret++;
	if (!wl.wm)
		fprintf(stderr, "missing xdg_wm_base\n"), ret++;
	if (!wl.seat)
		fprintf(stderr, "missing seat\n"), ret++;

	return ret;
}

void brush_default(struct landscape *landscape, int x, int y)
{
	size_t X, Y;

	landscape->quotient(landscape, x, y, &X, &Y);
	landscape_set_front(landscape, X, Y, 1);
}

void brush_conway_glider(struct landscape *landscape, int x, int y)
{
	size_t X, Y;

	landscape->quotient(landscape, x, y, &X, &Y);

	landscape_set_front(landscape, X-1, Y+1, 1);
	landscape_set_front(landscape, X,   Y-1, 1);
	landscape_set_front(landscape, X,   Y+1, 1);
	landscape_set_front(landscape, X+1, Y  , 1);
	landscape_set_front(landscape, X+1, Y+1, 1);
}

int landscape_init_memory(struct landscape *landscape)
{
	size_t area = landscape->width * landscape->height;

	landscape->flip = calloc(area, 1);
	landscape->flop = calloc(area, 1);
	if (!(landscape->flip && landscape->flop)) {
		fprintf(stderr, "no mem\n");
		return -ENOMEM;
	}
	landscape->show = landscape->flip;

	return 0;
}

int wl_init(struct landscape *landscape)
{
	wl.display = wl_display_connect(NULL);
	if (!wl.display) {
		fprintf(stderr, "couldn't connect to display\n");
		return -1;
	}

	struct wl_registry *registry = wl_display_get_registry(wl.display);
	if (!registry) {
		fprintf(stderr, "couldn't get registry\n");
		return -1;
	}
	wl_registry_add_listener(registry, &registry_listener, NULL);

	wl_display_roundtrip(wl.display);
	if (check_registry())
		return -1;

	xdg_wm_base_add_listener(wl.wm, &wm_base_listener, NULL);
	wl_seat_add_listener(wl.seat, &wl_seat_listener, landscape);
	wl_display_roundtrip(wl.display);
	wl.surf = wl_compositor_create_surface(wl.compositor);
	wl.xdg_surf = xdg_wm_base_get_xdg_surface(wl.wm, wl.surf);
	xdg_surface_add_listener(wl.xdg_surf, &xdg_surf_listener, landscape);
	wl.xdg_top = xdg_surface_get_toplevel(wl.xdg_surf);
	xdg_toplevel_set_title(wl.xdg_top, "cellularlandscapes");
	wl_surface_commit(wl.surf);


	wl.running = 1;
	wl.paused = 1;
	wl.wait_for_config = 1;
	wl.redraw = 0;
	wl.pointer_held = 0;
	wl.pointer_cell = -1;
	wl.brush = brush_default;

	return 0;
}

int handle_options(struct landscape *l, int argc, char **argv) {
	char c;
	opterr = 0;

	while ((c = getopt(argc, argv, "1:2:w:h:")) != -1) {
		switch (c) {
			case '1':
				l->automata = oned;

				l->quotient = clamped;
				l->rule = strtoul(optarg, NULL, 10);
				break;

			case '2':
				l->rule = strtoul(optarg, NULL, 10);
				break;

			case 'w':
				l->width = strtoul(optarg, NULL, 10);
				break;

			case 'h':
				l->height = strtoul(optarg, NULL, 10);
				break;

			default:
				return -1;
		}
	}

	return 0;
}

int main(int argc, char *argv[])
{
	int ret;
	struct landscape landscape = {
		.width = 160,
		.height = 90,
		.cell_width = 10,
		.cell_height = 10,
		.rule = conway,
		.automata = twod_life_like,
		.quotient = quotient_torus,
	};

	if (handle_options(&landscape, argc, argv))
		return EXIT_FAILURE;

	if ((ret = landscape_init_memory(&landscape)) < 0) {
		fprintf(stderr, fail_landscape_init);
		return ret;
	}

	if ((ret = wl_init(&landscape)) < 0) {
		fprintf(stderr, fail_wl_init);
		return ret;
	}

	struct timespec now = {0}, then = {0}, timeout = {0, 1E6};
	fd_set rfds;

	int wlfd = wl_display_get_fd(wl.display);

	while (wl.wait_for_config)
		ret = wl_display_dispatch(wl.display);

	if (ret < 0)
		return EXIT_FAILURE;

	while (wl.running) {
		FD_ZERO(&rfds);
		FD_SET(wlfd, &rfds);
		if (pselect(wlfd+1, &rfds, NULL, NULL, &timeout, NULL) < 0) {
			if (errno == EINTR)
				continue;
			return EXIT_FAILURE;
		}

                if (FD_ISSET(wlfd, &rfds) && wl_display_dispatch(wl.display) == -1)  {
			return EXIT_FAILURE;
		}

		clock_gettime(CLOCK_MONOTONIC, &now);
		long dt_ms = (now.tv_sec - then.tv_sec)*1000
				+ (now.tv_nsec - then.tv_nsec)/1E6;
		if (!wl.paused && dt_ms >= STEP_TIME_MSEC) {
			landscape_step(&landscape);
			then = now;
		}

                if (!wl.frame_cb || wl.redraw) {
			render(&landscape);
		}

		if (wl_display_dispatch_pending(wl.display) < 0)
			return EXIT_FAILURE;
		if (wl_display_flush(wl.display) < 0)
			return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}
