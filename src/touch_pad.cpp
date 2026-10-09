// touch_pad.cpp — on-screen touch controller overlay (iOS).
//
// Pikmin 1's pc/src/pc_touch_pad.cpp, moved to SDL3 events and Pikmin 2's
// button set (see BUTTON SET below); everything else is unchanged. Pikmin 1
// ported it from ac-metal's pc_touch_pad.c. What carried over unchanged is the
// part that took the iterating: a FLOATING stick (the first finger landing in
// the left zone anchors a virtual stick wherever it touched, dragging
// deflects, release recenters — much better than a fixed pad you have to find
// by feel), per-finger button latching so a finger that slides off a button
// still owns it until it lifts, and hiding the whole overlay when a real
// controller is connected.
//
// What is deliberately different:
//
//  - COORDINATE SPACE. AC laid out in the game's 320x240 UI space and mapped
//    fingers through the letterboxed content rect, because it drew with the
//    game's own menu primitives. Pikmin has no such layer, so layout here is
//    in a unit space spanning the WHOLE drawable (y 0..1, x 0..aspect). That
//    also puts the controls in the letterbox bars rather than on top of the
//    game: a 4:3 scene on a ~19.5:9 iPhone leaves ~19% of the width black on
//    each side, which is almost exactly a thumb's worth of room. Nothing
//    occludes the playfield.
//
//  - BUTTON SET. Pikmin 1 needs the stick, A/B/X/Y, Start, and L. Pikmin 2's
//    gameplay also reads R and Z (camera, playCamera.cpp), and the D-pad: up
//    and down are the sprays, left and right change the held Pikmin
//    (naviState.cpp). Those are added; the C-stick (squad steering) is not,
//    as in Pikmin 1. A touch button is all-or-nothing, so L and R are full
//    trigger pulls.
//
//  - SHAPES, NOT SQUARES. The first cut drew every control as an axis-aligned
//    square because that is one triangle pair and no shader work. It read as a
//    row of blocks. Controls are now rounded boxes with an outline and a
//    stroke-font label, rasterised from a signed distance field
//    (src/touch_draw.cpp), which is what lets X and Y be the GameCube's rotated
//    kidneys instead of two more squares.
#include "p2_touch_pad.h"

#include <TargetConditionals.h> // TARGET_OS_IPHONE

#include <SDL3/SDL_events.h>
#include <dolphin/pad.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

// GC pad bits (dolphin/pad.h).
#define TP_BTN_A     PAD_BUTTON_A
#define TP_BTN_B     PAD_BUTTON_B
#define TP_BTN_X     PAD_BUTTON_X
#define TP_BTN_Y     PAD_BUTTON_Y
#define TP_BTN_START PAD_BUTTON_START
#define TP_BTN_L     PAD_TRIGGER_L
#define TP_BTN_R     PAD_TRIGGER_R
#define TP_BTN_Z     PAD_TRIGGER_Z

#define TP_MAX_FINGERS 10
#define TP_STICK_TRAVEL 0.115f // unit-space deflection for full stick
#define TP_STICK_MAG    90.0f  // GC stick value at full deflection (as Pikmin 1)
#define TP_HIT_SLOP     0.022f // touch tolerance beyond the visual edge

// Stick furniture, as multiples of the travel. The gate is drawn WIDER than
// the travel and the knob small enough that (gate - knob) still exceeds it:
// that is what lets the knob sit flush against the inside of the gate at full
// deflection instead of half-escaping it, while still moving nearly as far as
// the finger does.
#define TP_STICK_GATE 1.22f
#define TP_STICK_KNOB 0.50f

// Stick zone: a finger landing left of this x and below this y owns the stick.
#define TP_ZONE_X 0.62f
#define TP_ZONE_Y 0.30f
// Idle hint position before any finger anchors it. Centred in the left
// letterbox bar of a 19.5:9 screen (bar spans x 0..0.417), at the same height
// as the button cluster so the two read as one row.
#define TP_HINT_X 0.209f
#define TP_HINT_Y 0.678f

typedef struct {
	const char* label; // drawn with the stroke font; NULL for none
	float label_h;     // cap height, unit space
	float cx, cy;      // centre, unit space. cx is FROM THE RIGHT edge when
	                   // from_right is set (aspect is not known until runtime).
	float hw, hh;      // half extents before rotation
	float radius;      // corner radius; == min(hw,hh) makes a circle/capsule
	float rot_deg;     // counter-clockwise on screen
	int from_right;
	unsigned short mask;
	unsigned char cr, cg, cb;
} TPButton;

// The right-hand four reproduce the GameCube face cluster, which is not a
// symmetric diamond: a large A in the middle, a small round B down-left, and X
// and Y as kidneys that wrap AROUND A — X standing on the right, Y lying
// across the top. Muscle memory for this pad is spatial, so the arrangement is
// worth getting right rather than approximating with four equal circles.
//
// Centres are polar around A (angle measured counter-clockwise from screen
// right, so the numbers below match how the pad looks, not how y-down maths
// works), all at radius 0.113:
//
//     B  205 deg      X    8 deg      Y  100 deg
//
// and each kidney's long axis is tangential — perpendicular to its own radius
// — which is what makes the pair curve around A instead of pointing at it.
//
// Shoulders and Start go along the TOP edge, deliberately far from where either
// thumb rests (Pikmin 2: R and Z top right, Start beside R; the D-pad sits
// under L, above the stick zone). Start especially: the obvious spot is bottom-centre (where AC put it)
// but on a modern iPhone that is the home-indicator strip, and it also sits
// under the stick thumb — two ways to hit pause by accident mid-throw. L takes
// the top LEFT because that is the shoulder it is, and it is drawn as a
// lozenge rather than a circle for the same reason.
static const TPButton tp_buttons[] = {
	//  label  lh      cx       cy       hw      hh      radius  rot    fr mask          r    g    b
	{ "A",     0.044f, 0.2150f, 0.7120f, 0.062f, 0.062f, 0.062f,   0.0f, 1, TP_BTN_A,     110, 203, 165 },
	{ "B",     0.026f, 0.3174f, 0.7598f, 0.034f, 0.034f, 0.034f,   0.0f, 1, TP_BTN_B,     215,  91,  91 },
	{ "X",     0.030f, 0.1031f, 0.6963f, 0.058f, 0.029f, 0.029f,  98.0f, 1, TP_BTN_X,     216, 212, 206 },
	{ "Y",     0.030f, 0.2346f, 0.6007f, 0.058f, 0.029f, 0.029f,  10.0f, 1, TP_BTN_Y,     216, 212, 206 },
	{ "L",     0.030f, 0.1280f, 0.0980f, 0.076f, 0.032f, 0.030f,   0.0f, 0, TP_BTN_L,     169, 169, 206 },
	{ "R",     0.030f, 0.1280f, 0.0980f, 0.076f, 0.032f, 0.030f,   0.0f, 1, TP_BTN_R,     169, 169, 206 },
	{ "Z",     0.026f, 0.1280f, 0.1850f, 0.058f, 0.026f, 0.026f,   0.0f, 1, TP_BTN_Z,     132, 108, 210 },
	{ "START", 0.023f, 0.3000f, 0.0920f, 0.030f, 0.030f, 0.030f,   0.0f, 1, TP_BTN_START, 168, 168, 168 },
	// D-pad: four arms around an inert centre square, so the five read as one cross.
	{ NULL,    0.0f,   0.1280f, 0.2480f, 0.026f, 0.026f, 0.008f,   0.0f, 0, PAD_BUTTON_UP,    200, 198, 194 },
	{ NULL,    0.0f,   0.1280f, 0.3520f, 0.026f, 0.026f, 0.008f,   0.0f, 0, PAD_BUTTON_DOWN,  200, 198, 194 },
	{ NULL,    0.0f,   0.0760f, 0.3000f, 0.026f, 0.026f, 0.008f,   0.0f, 0, PAD_BUTTON_LEFT,  200, 198, 194 },
	{ NULL,    0.0f,   0.1800f, 0.3000f, 0.026f, 0.026f, 0.008f,   0.0f, 0, PAD_BUTTON_RIGHT, 200, 198, 194 },
	{ NULL,    0.0f,   0.1280f, 0.3000f, 0.026f, 0.026f, 0.004f,   0.0f, 0, 0,                200, 198, 194 },
};
#define TP_NUM_BUTTONS ((int)(sizeof(tp_buttons) / sizeof(tp_buttons[0])))

// START does not fit inside a 0.03-radius button, so it is set beneath it —
// which is where the real controller prints it too. Everything else is labelled
// on the button face.
#define TP_START_LABEL_DY 0.052f

typedef struct {
	SDL_FingerID id;
	int active;
	int button;   // index into tp_buttons, or -1
	int is_stick;
	float ux, uy; // current position, unit space
	float ax, ay; // stick anchor, unit space
} TPFinger;

// Idle dimming. Touch controls are always in the way of something, so the
// convention every on-screen pad settles on is: full opacity while in use, a
// dimmer resting level once the player has left it alone, and an INSTANT
// return on the next touch — a fade back in would lag the input it belongs to.
// The rest level is deliberately mild rather than near-invisible; the controls
// still have to be findable at a glance mid-game.
#define TP_IDLE_HOLD 3.0f  // seconds at full opacity after the last touch
#define TP_IDLE_FADE 0.9f  // seconds spent crossing to the resting level
#define TP_IDLE_REST 0.42f // alpha multiplier once faded

static TPFinger tp_fingers[TP_MAX_FINGERS];
static int tp_controller_present;
static float tp_aspect = 4.0f / 3.0f;
static float tp_idle; // seconds since the last touch

// Resolved centre x for a button (the table stores a right-edge offset for the
// right-hand cluster so one table serves any aspect).
static float tp_btn_x(const TPButton* b) { return b->from_right ? (tp_aspect - b->cx) : b->cx; }

static int tp_platform_enabled(void)
{
#if TARGET_OS_IPHONE
	return 1;
#else
	// P2_TOUCH_OVERLAY=1 forces it on elsewhere. Desktop has no touch input,
	// so this only exercises layout and drawing — which is still the useful
	// half to be able to look at without a device.
	static int checked, enabled;
	if (!checked) {
		checked = 1;
		enabled = getenv("P2_TOUCH_OVERLAY") != NULL;
	}
	return enabled;
#endif
}

static int tp_active(void) { return tp_platform_enabled() && !tp_controller_present; }

void p2_touch_pad_set_aspect(float aspect)
{
	if (aspect > 0.1f && aspect < 10.0f) {
		tp_aspect = aspect;
	}
}

void p2_touch_pad_set_controller(int present)
{
	if (present && !tp_controller_present) {
		memset(tp_fingers, 0, sizeof(tp_fingers)); // drop held touches
	}
	tp_controller_present = present != 0;
}

void p2_touch_pad_advance(float dt)
{
	// A finger down holds the overlay awake even if it is resting in dead
	// space: the player is on the glass, so the controls are in use.
	for (int i = 0; i < TP_MAX_FINGERS; i++) {
		if (tp_fingers[i].active) {
			tp_idle = 0.0f;
			return;
		}
	}
	// Clamp rather than reject: a long hitch (a breakpoint, a load, coming back
	// from the background) should still land the overlay at rest, not stall the
	// timer or jump it by whole seconds.
	if (!(dt > 0.0f)) {
		return;
	}
	tp_idle += dt > 0.25f ? 0.25f : dt;
}

// 1.0 in use, TP_IDLE_REST once idle. Smoothstepped: a linear ramp shows its
// corners at both ends, which reads as a flicker rather than a settle.
static float tp_fade(void)
{
	if (tp_idle <= TP_IDLE_HOLD) {
		return 1.0f;
	}
	float t = (tp_idle - TP_IDLE_HOLD) / TP_IDLE_FADE;
	if (t >= 1.0f) {
		return TP_IDLE_REST;
	}
	t = t * t * (3.0f - 2.0f * t);
	return 1.0f + (TP_IDLE_REST - 1.0f) * t;
}

static TPFinger* tp_find(SDL_FingerID id)
{
	for (int i = 0; i < TP_MAX_FINGERS; i++) {
		if (tp_fingers[i].active && tp_fingers[i].id == id) {
			return &tp_fingers[i];
		}
	}
	return NULL;
}

static TPFinger* tp_alloc(SDL_FingerID id)
{
	for (int i = 0; i < TP_MAX_FINGERS; i++) {
		if (!tp_fingers[i].active) {
			memset(&tp_fingers[i], 0, sizeof(TPFinger));
			tp_fingers[i].active = 1;
			tp_fingers[i].id     = id;
			tp_fingers[i].button = -1;
			return &tp_fingers[i];
		}
	}
	return NULL;
}

static int tp_stick_owner(void)
{
	for (int i = 0; i < TP_MAX_FINGERS; i++) {
		if (tp_fingers[i].active && tp_fingers[i].is_stick) {
			return i;
		}
	}
	return -1;
}

// Signed distance to a rounded box centred at the origin — the same function
// the shader rasterises with, so the touchable region IS the drawn region
// (plus slop) even for the rotated kidneys.
static float tp_sd_round_box(float px, float py, float hw, float hh, float r)
{
	const float qx = fabsf(px) - hw + r;
	const float qy = fabsf(py) - hh + r;
	const float ax = qx > 0.0f ? qx : 0.0f;
	const float ay = qy > 0.0f ? qy : 0.0f;
	float in      = qx > qy ? qx : qy;
	if (in > 0.0f) {
		in = 0.0f;
	}
	return sqrtf(ax * ax + ay * ay) + in - r;
}

// Distance from a point to a button, negative inside. Rotation is undone
// first: `rot` turns local +x counter-clockwise on screen, and unit y points
// DOWN, so the forward map is (x*c + y*s, -x*s + y*c) and this is its inverse.
static float tp_btn_distance(const TPButton* b, float ux, float uy)
{
	const float dx = ux - tp_btn_x(b);
	const float dy = uy - b->cy;
	const float a  = b->rot_deg * (float)(M_PI / 180.0);
	const float c = cosf(a), s = sinf(a);
	return tp_sd_round_box(dx * c - dy * s, dx * s + dy * c, b->hw, b->hh, b->radius);
}

static int tp_button_hit(float ux, float uy)
{
	int best      = -1;
	float best_d  = 0.0f;
	for (int i = 0; i < TP_NUM_BUTTONS; i++) {
		const float d = tp_btn_distance(&tp_buttons[i], ux, uy);
		if (d <= TP_HIT_SLOP && (best < 0 || d < best_d)) {
			best   = i;
			best_d = d;
		}
	}
	return best;
}

void p2_touch_pad_handle_event(const void* sdl_event)
{
	if (!tp_platform_enabled()) {
		return;
	}
	const SDL_Event* e = (const SDL_Event*)sdl_event;

	// SDL finger coords are normalised [0,1] per axis over the whole window;
	// x scales by aspect to land in unit space.
	const float ux = e->tfinger.x * tp_aspect;
	const float uy = e->tfinger.y;

	// Any touch at all wakes the overlay, including one that hits nothing —
	// reaching for a control counts as using it, and the reach comes first.
	if (e->type == SDL_EVENT_FINGER_DOWN || e->type == SDL_EVENT_FINGER_MOTION || e->type == SDL_EVENT_FINGER_UP ||
	    e->type == SDL_EVENT_FINGER_CANCELED) {
		tp_idle = 0.0f;
	}

	switch (e->type) {
	case SDL_EVENT_FINGER_DOWN: {
		TPFinger* f = tp_alloc(e->tfinger.fingerID);
		if (!f) {
			return;
		}
		f->ux = ux;
		f->uy = uy;
		// Buttons win over the stick zone where they overlap, so a button
		// placed inside the left zone (L, Start) still works.
		const int hit = tp_button_hit(ux, uy);
		if (hit >= 0) {
			f->button = hit;
		} else if (ux < TP_ZONE_X && uy > TP_ZONE_Y && tp_stick_owner() < 0) {
			f->is_stick = 1;
			f->ax       = ux;
			f->ay       = uy;
		}
		break;
	}
	case SDL_EVENT_FINGER_MOTION: {
		TPFinger* f = tp_find(e->tfinger.fingerID);
		if (!f) {
			return;
		}
		// Position updates even past the zone edge: a stick finger dragged
		// out must keep deflecting, and a button finger keeps its latch.
		f->ux = ux;
		f->uy = uy;
		break;
	}
	case SDL_EVENT_FINGER_UP:
	case SDL_EVENT_FINGER_CANCELED: {
		TPFinger* f = tp_find(e->tfinger.fingerID);
		if (f) {
			f->active = 0;
		}
		break;
	}
	default:
		break;
	}
}

int p2_touch_pad_apply(void* pad_status)
{
	if (!tp_active() || !pad_status) {
		return 0;
	}
	PADStatus* pad = (PADStatus*)pad_status;
	for (int i = 0; i < TP_MAX_FINGERS; i++) {
		const TPFinger* f = &tp_fingers[i];
		if (!f->active) {
			continue;
		}
		if (f->button >= 0) {
			const unsigned short mask = tp_buttons[f->button].mask;
			pad->button |= mask;
			// L and R drive the analog triggers too, as full pulls: a touch
			// button cannot express a partial hold.
			if (mask == TP_BTN_L) {
				pad->triggerLeft = 255;
			}
			if (mask == TP_BTN_R) {
				pad->triggerRight = 255;
			}
		}
		if (f->is_stick) {
			float sx = (f->ux - f->ax) * (TP_STICK_MAG / TP_STICK_TRAVEL);
			float sy = -(f->uy - f->ay) * (TP_STICK_MAG / TP_STICK_TRAVEL); // unit y down -> GC y up
			if (sx > TP_STICK_MAG) {
				sx = TP_STICK_MAG;
			} else if (sx < -TP_STICK_MAG) {
				sx = -TP_STICK_MAG;
			}
			if (sy > TP_STICK_MAG) {
				sy = TP_STICK_MAG;
			} else if (sy < -TP_STICK_MAG) {
				sy = -TP_STICK_MAG;
			}
			pad->stickX = (s8)sx;
			pad->stickY = (s8)sy;
		}
	}
	return 1;
}

static int tp_button_pressed(int idx)
{
	for (int i = 0; i < TP_MAX_FINGERS; i++) {
		if (tp_fingers[i].active && tp_fingers[i].button == idx) {
			return 1;
		}
	}
	return 0;
}

// ---- shape emission --------------------------------------------------------

static P2TouchShape* tp_push(P2TouchShape** p, int* n, int max)
{
	if (*n >= max) {
		return NULL;
	}
	P2TouchShape* s = (*p)++;
	(*n)++;
	memset(s, 0, sizeof(*s));
	s->glyph[0] = -1;
	return s;
}

static void tp_set_rgba(float* dst, float r, float g, float b, float a)
{
	dst[0] = r;
	dst[1] = g;
	dst[2] = b;
	dst[3] = a;
}

// Resolve a label to font indices. An unknown character is dropped rather than
// drawn wrong; the set is fixed and small enough that this can only fire on a
// typo in the table above.
static void tp_set_label(P2TouchShape* s, const char* text, float cap_h)
{
	int n = 0;
	for (const char* c = text; *c && n < P2_TOUCH_GLYPH_MAX; c++) {
		const char* at = strchr(P2_TOUCH_GLYPH_SET, *c);
		if (at) {
			s->glyph[n++] = (signed char)(at - P2_TOUCH_GLYPH_SET);
		}
	}
	if (n < P2_TOUCH_GLYPH_MAX) {
		s->glyph[n] = -1;
	}
	s->lh = n > 0 ? cap_h : 0.0f;
}

static float tp_label_half_width(const P2TouchShape* s)
{
	int n = 0;
	while (n < P2_TOUCH_GLYPH_MAX && s->glyph[n] >= 0) {
		n++;
	}
	if (n == 0) {
		return 0.0f;
	}
	return 0.5f * ((n - 1) * P2_TOUCH_GLYPH_ADV + P2_TOUCH_GLYPH_INK) * s->lh;
}

int p2_touch_pad_get_shapes(P2TouchShape* out, int max)
{
	if (!tp_active() || !out || max <= 0) {
		return 0;
	}
	int n           = 0;
	P2TouchShape* p = out;
	const int owner = tp_stick_owner();

	// Stick: gate ring at the anchor (or the idle hint), knob at the
	// deflection. The knob's CENTRE is clamped to (gate - knob), not to the
	// travel, so at full deflection it comes to rest flush inside the ring —
	// the ring then reads as the limit it actually is.
	const float gate_r = TP_STICK_TRAVEL * TP_STICK_GATE;
	const float knob_r = TP_STICK_TRAVEL * TP_STICK_KNOB;
	const float knob_max = gate_r - knob_r - 0.004f;

	float bx = TP_HINT_X, by = TP_HINT_Y, kx = TP_HINT_X, ky = TP_HINT_Y;
	float lit = 0.0f;
	if (owner >= 0) {
		const TPFinger* f = &tp_fingers[owner];
		bx = f->ax;
		by = f->ay;
		kx = f->ux;
		ky = f->uy;
		const float dx = kx - bx, dy = ky - by;
		const float d2 = dx * dx + dy * dy;
		if (d2 > knob_max * knob_max) {
			const float inv = knob_max / sqrtf(d2);
			kx              = bx + dx * inv;
			ky              = by + dy * inv;
		}
		lit = 1.0f;
	}
	if (P2TouchShape* s = tp_push(&p, &n, max)) {
		s->cx = bx;
		s->cy = by;
		s->hw = s->hh = s->radius = gate_r;
		s->edgew                  = 0.0055f;
		tp_set_rgba(s->fill, 1.0f, 1.0f, 1.0f, 0.05f + 0.06f * lit);
		tp_set_rgba(s->edge, 1.0f, 1.0f, 1.0f, 0.26f + 0.20f * lit);
	}
	if (P2TouchShape* s = tp_push(&p, &n, max)) {
		s->cx = kx;
		s->cy = ky;
		s->hw = s->hh = s->radius = knob_r;
		s->edgew                  = 0.005f;
		tp_set_rgba(s->fill, 1.0f, 1.0f, 1.0f, 0.22f + 0.32f * lit);
		tp_set_rgba(s->edge, 1.0f, 1.0f, 1.0f, 0.48f + 0.38f * lit);
	}

	// Bodies first, then every label, so no button face can overdraw a
	// neighbour's letter where the kidneys crowd A.
	for (int i = 0; i < TP_NUM_BUTTONS; i++) {
		const TPButton* b = &tp_buttons[i];
		P2TouchShape* s   = tp_push(&p, &n, max);
		if (!s) {
			break;
		}
		const float hot = tp_button_pressed(i) ? 1.0f : 0.0f;
		const float cr = b->cr / 255.0f, cg = b->cg / 255.0f, cb = b->cb / 255.0f;
		s->cx     = tp_btn_x(b);
		s->cy     = b->cy;
		s->hw     = b->hw;
		s->hh     = b->hh;
		s->radius = b->radius;
		s->rot    = b->rot_deg * (float)(M_PI / 180.0);
		s->edgew  = 0.0055f;
		tp_set_rgba(s->fill, cr, cg, cb, 0.20f + 0.42f * hot);
		// The rim is the button's own colour lightened, not white: it keeps A
		// green and B red at a glance even when the fill is nearly invisible
		// against the black letterbox bar.
		tp_set_rgba(s->edge, cr + (1.0f - cr) * 0.35f, cg + (1.0f - cg) * 0.35f, cb + (1.0f - cb) * 0.35f,
		            0.50f + 0.45f * hot);
	}

	for (int i = 0; i < TP_NUM_BUTTONS; i++) {
		const TPButton* b = &tp_buttons[i];
		if (!b->label) {
			continue;
		}
		P2TouchShape* s = tp_push(&p, &n, max);
		if (!s) {
			break;
		}
		const float hot = tp_button_pressed(i) ? 1.0f : 0.0f;
		const float cr = b->cr / 255.0f, cg = b->cg / 255.0f, cb = b->cb / 255.0f;
		tp_set_label(s, b->label, b->label_h);
		s->cx = tp_btn_x(b);
		s->cy = b->cy + (b->mask == TP_BTN_START ? TP_START_LABEL_DY : 0.0f);
		// Labels stay upright even where the button is not: the letters on the
		// real X and Y are printed level, not rolled with the kidney.
		s->rot = 0.0f;
		// The box is only there to size the quad the glyphs are drawn in — the
		// fill is left transparent. Half a cap height plus the stroke's own
		// overshoot past cap top and baseline.
		s->hw = tp_label_half_width(s);
		s->hh = s->lh * 0.575f;
		tp_set_rgba(s->lcol, cr + (1.0f - cr) * 0.72f, cg + (1.0f - cg) * 0.72f, cb + (1.0f - cb) * 0.72f,
		            0.70f + 0.30f * hot);
	}

	// Idle dim, applied once at the end so it scales every control uniformly —
	// the overlay has to fade as one object, not drift apart shape by shape.
	const float fade = tp_fade();
	if (fade < 1.0f) {
		for (int i = 0; i < n; i++) {
			out[i].fill[3] *= fade;
			out[i].edge[3] *= fade;
			out[i].lcol[3] *= fade;
		}
	}
	return n;
}
