/*
 * hex -- a very simple hex viewer
 *
 * Copyright (c) 2016, Přemysl Janouch <p.janouch@gmail.com>
 * All rights reserved.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include "config.h"

// We "need" to have an enum for attributes before including liberty.
// Avoiding colours in the defaults here in order to support dumb terminals.
#define ATTRIBUTE_TABLE(XX)                             \
	XX( HEADER,     "header",     -1, -1, 0           ) \
	XX( HIGHLIGHT,  "highlight",  -1, -1, A_BOLD      ) \
	/* Bar                                           */ \
	XX( BAR,        "bar",        -1, -1, A_REVERSE   ) \
	XX( BAR_ACTIVE, "bar_active", -1, -1, A_UNDERLINE ) \
	/* Listview                                      */ \
	XX( EVEN,       "even",       -1, -1, 0           ) \
	XX( ODD,        "odd",        -1, -1, 0           ) \
	XX( SELECTION,  "selection",  -1, -1, A_REVERSE   ) \
	/* These are for debugging only                  */ \
	XX( WARNING,    "warning",     3, -1, 0           ) \
	XX( ERROR,      "error",       1, -1, 0           )

enum
{
#define XX(name, config, fg_, bg_, attrs_) ATTRIBUTE_ ## name,
	ATTRIBUTE_TABLE (XX)
#undef XX
	ATTRIBUTE_COUNT
};

// My battle-tested C framework acting as a GLib replacement.  Its one big
// disadvantage is missing support for i18n but that can eventually be added
// as an optional feature.  Localised applications look super awkward, though.

// User data for logger functions to enable formatted logging
#define print_fatal_data    ((void *) ATTRIBUTE_ERROR)
#define print_error_data    ((void *) ATTRIBUTE_ERROR)
#define print_warning_data  ((void *) ATTRIBUTE_WARNING)

#define LIBERTY_WANT_POLLER
#define LIBERTY_WANT_ASYNC
#define LIBERTY_WANT_PROTO_HTTP
#include "liberty/liberty.c"

#include <locale.h>
#include <termios.h>
#ifndef TIOCGWINSZ
#include <sys/ioctl.h>
#endif  // ! TIOCGWINSZ
#include <ncurses.h>

// ncurses is notoriously retarded for input handling, we need something
// different if only to receive mouse events reliably.

#include "termo.h"

// It is surprisingly hard to find a good library to handle Unicode shenanigans,
// and there's enough of those for it to be impractical to reimplement them.
//
//                         GLib          ICU     libunistring    utf8proc
// Decently sized            .            .            x            x
// Grapheme breaks           .            x            .            x
// Character width           x            .            x            x
// Locale handling           .            .            x            .
// Liberal license           .            x            .            x
//
// Also note that the ICU API is icky and uses UTF-16 for its primary encoding.
//
// Currently we're chugging along with libunistring but utf8proc seems viable.
// Non-Unicode locales can mostly be handled with simple iconv like in sdtui.
// Similarly grapheme breaks can be guessed at using character width (a basic
// test here is Zalgo text).
//
// None of this is ever going to work too reliably anyway because terminals
// and Unicode don't go awfully well together.  In particular, character cell
// devices have some problems with double-wide characters.

#include <unistr.h>
#include <uniwidth.h>
#include <uniconv.h>
#include <unicase.h>

#define APP_TITLE  PROGRAM_NAME         ///< Left top corner

// --- Utilities ---------------------------------------------------------------

// The standard endwin/refresh sequence makes the terminal flicker
static void
update_curses_terminal_size (void)
{
#if defined (HAVE_RESIZETERM) && defined (TIOCGWINSZ)
	struct winsize size;
	if (!ioctl (STDOUT_FILENO, TIOCGWINSZ, (char *) &size))
	{
		char *row = getenv ("LINES");
		char *col = getenv ("COLUMNS");
		unsigned long tmp;
		resizeterm (
			(row && xstrtoul (&tmp, row, 10)) ? tmp : size.ws_row,
			(col && xstrtoul (&tmp, col, 10)) ? tmp : size.ws_col);
	}
#else  // HAVE_RESIZETERM && TIOCGWINSZ
	endwin ();
	refresh ();
#endif  // HAVE_RESIZETERM && TIOCGWINSZ
}

static char *
latin1_to_utf8 (const char *latin1)
{
	struct str converted;
	str_init (&converted);
	while (*latin1)
	{
		uint8_t c = *latin1++;
		if (c < 0x80)
			str_append_c (&converted, c);
		else
		{
			str_append_c (&converted, 0xC0 | (c >> 6));
			str_append_c (&converted, 0x80 | (c & 0x3F));
		}
	}
	return str_steal (&converted);
}

// --- Application -------------------------------------------------------------

// Function names are prefixed mostly because of curses which clutters the
// global namespace and makes it harder to distinguish what functions relate to.

struct attrs
{
	short fg;                           ///< Foreground colour index
	short bg;                           ///< Background colour index
	chtype attrs;                       ///< Other attributes
};

// Basically a container for most of the globals; no big sense in handing
// around a pointer to this, hence it is a simple global variable as well.
// There is enough global state as it is.

static struct app_context
{
	// Event loop:

	struct poller poller;               ///< Poller
	bool quitting;                      ///< Quit signal for the event loop
	bool polling;                       ///< The event loop is running

	struct poller_fd tty_event;         ///< Terminal input event
	struct poller_fd signal_event;      ///< Signal FD event

	// Data:

	struct config config;               ///< Program configuration
	char *filename;                     ///< Target filename

	uint8_t *data;                      ///< Target data
	uint64_t data_len;                  ///< Length of the data
	uint64_t data_offset;               ///< Offset of the data within the file
	uint64_t data_cursor;               ///< Current position within the data

	// TODO: get rid of this as it can be computed from "data*"
	size_t item_count;                  ///< Total item count
	int item_top;                       ///< Index of the topmost item
	int item_selected;                  ///< Index of the selected item

	// Emulated widgets:

	// TODO: make this the footer;
	//   remove this, we know how high the footer is
	int header_height;                  ///< Height of the header

	struct poller_idle refresh_event;   ///< Refresh the screen

	// Terminal:

	termo_t *tk;                        ///< termo handle
	struct poller_timer tk_timer;       ///< termo timeout timer
	bool locale_is_utf8;                ///< The locale is Unicode

	struct attrs attrs[ATTRIBUTE_COUNT];
}
g_ctx;

/// Shortcut to retrieve named terminal attributes
#define APP_ATTR(name) g_ctx.attrs[ATTRIBUTE_ ## name].attrs

// --- Configuration -----------------------------------------------------------

static struct config_schema g_config_colors[] =
{
#define XX(name_, config, fg_, bg_, attrs_) \
	{ .name = config, .type = CONFIG_ITEM_STRING },
	ATTRIBUTE_TABLE (XX)
#undef XX
	{}
};

static const char *
get_config_string (struct config_item *root, const char *key)
{
	struct config_item *item = config_item_get (root, key, NULL);
	hard_assert (item);
	if (item->type == CONFIG_ITEM_NULL)
		return NULL;
	hard_assert (config_item_type_is_string (item->type));
	return item->value.string.str;
}

/// Load configuration for a color using a subset of git config colors
static void
app_load_color (struct config_item *subtree, const char *name, int id)
{
	const char *value = get_config_string (subtree, name);
	if (!value)
		return;

	struct str_vector v;
	str_vector_init (&v);
	cstr_split (value, " ", true, &v);

	int colors = 0;
	struct attrs attrs = { -1, -1, 0 };
	for (char **it = v.vector; *it; it++)
	{
		char *end = NULL;
		long n = strtol (*it, &end, 10);
		if (*it != end && !*end && n >= SHRT_MIN && n <= SHRT_MAX)
		{
			if (colors == 0) attrs.fg = n;
			if (colors == 1) attrs.bg = n;
			colors++;
		}
		else if (!strcmp (*it, "bold"))    attrs.attrs |= A_BOLD;
		else if (!strcmp (*it, "dim"))     attrs.attrs |= A_DIM;
		else if (!strcmp (*it, "ul"))      attrs.attrs |= A_UNDERLINE;
		else if (!strcmp (*it, "blink"))   attrs.attrs |= A_BLINK;
		else if (!strcmp (*it, "reverse")) attrs.attrs |= A_REVERSE;
#ifdef A_ITALIC
		else if (!strcmp (*it, "italic"))  attrs.attrs |= A_ITALIC;
#endif  // A_ITALIC
	}
	str_vector_free (&v);
	g_ctx.attrs[id] = attrs;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
load_config_colors (struct config_item *subtree, void *user_data)
{
	config_schema_apply_to_object (g_config_colors,   subtree, user_data);

	// The attributes cannot be changed dynamically right now, so it doesn't
	// make much sense to make use of "on_change" callbacks either.
	// For simplicity, we should reload the entire table on each change anyway.
#define XX(name, config, fg_, bg_, attrs_) \
	app_load_color (subtree, config, ATTRIBUTE_ ## name);
	ATTRIBUTE_TABLE (XX)
#undef XX
}

static void
app_load_configuration (void)
{
	struct config *config = &g_ctx.config;
	config_register_module (config, "colors", load_config_colors, NULL);

	// Bootstrap configuration, so that we can access schema items at all
	config_load (config, config_item_object ());

	char *filename = resolve_filename
		(PROGRAM_NAME ".conf", resolve_relative_config_filename);
	if (!filename)
		return;

	struct error *e = NULL;
	struct config_item *root = config_read_from_file (filename, &e);
	free (filename);

	if (e)
	{
		print_error ("error loading configuration: %s", e->message);
		error_free (e);
		exit (EXIT_FAILURE);
	}
	if (root)
	{
		config_load (&g_ctx.config, root);
		config_schema_call_changed (g_ctx.config.root);
	}
}

// --- Application -------------------------------------------------------------

static void
app_init_attributes (void)
{
#define XX(name, config, fg_, bg_, attrs_)          \
	g_ctx.attrs[ATTRIBUTE_ ## name].fg    = fg_;    \
	g_ctx.attrs[ATTRIBUTE_ ## name].bg    = bg_;    \
	g_ctx.attrs[ATTRIBUTE_ ## name].attrs = attrs_;
	ATTRIBUTE_TABLE (XX)
#undef XX
}

static void
app_init_context (void)
{
	poller_init (&g_ctx.poller);
	config_init (&g_ctx.config);

	// This is also approximately what libunistring does internally,
	// since the locale name is canonicalized by locale_charset().
	// Note that non-Unicode locales are handled pretty inefficiently.
	g_ctx.locale_is_utf8 = !strcasecmp_ascii (locale_charset (), "UTF-8");

	app_init_attributes ();
}

static void
app_init_terminal (void)
{
	TERMO_CHECK_VERSION;
	if (!(g_ctx.tk = termo_new (STDIN_FILENO, NULL, 0)))
		abort ();
	if (!initscr () || nonl () == ERR)
		abort ();

	// Disable cursor, we're not going to use it most of the time
	curs_set (0);

	// By default we don't use any colors so they're not required...
	if (start_color () == ERR
	 || use_default_colors () == ERR
	 || COLOR_PAIRS <= ATTRIBUTE_COUNT)
		return;

	for (int a = 0; a < ATTRIBUTE_COUNT; a++)
	{
		// ...thus we can reset back to defaults even after initializing some
		if (g_ctx.attrs[a].fg >= COLORS || g_ctx.attrs[a].fg < -1
		 || g_ctx.attrs[a].bg >= COLORS || g_ctx.attrs[a].bg < -1)
		{
			app_init_attributes ();
			return;
		}

		init_pair (a + 1, g_ctx.attrs[a].fg, g_ctx.attrs[a].bg);
		g_ctx.attrs[a].attrs |= COLOR_PAIR (a + 1);
	}
}

static void
app_free_context (void)
{
	config_free (&g_ctx.config);
	poller_free (&g_ctx.poller);

	free (g_ctx.filename);
	free (g_ctx.data);

	if (g_ctx.tk)
		termo_destroy (g_ctx.tk);
}

static void
app_quit (void)
{
	g_ctx.quitting = true;
	g_ctx.polling = false;
}

static bool
app_is_character_in_locale (ucs4_t ch)
{
	// Avoid the overhead joined with calling iconv() for all characters.
	if (g_ctx.locale_is_utf8)
		return true;

	// The library really creates a new conversion object every single time
	// and doesn't provide any smarter APIs.  Luckily, most users use UTF-8.
	size_t len;
	char *tmp = u32_conv_to_encoding (locale_charset (), iconveh_error,
		&ch, 1, NULL, NULL, &len);
	if (!tmp)
		return false;
	free (tmp);
	return true;
}

// --- Terminal output ---------------------------------------------------------

// Necessary abstraction to simplify aligned, formatted character output

struct row_char
{
	ucs4_t c;                           ///< Unicode codepoint
	chtype attrs;                       ///< Special attributes
	int width;                          ///< How many cells this takes
};

struct row_buffer
{
	struct row_char *chars;             ///< Characters
	size_t chars_len;                   ///< Character count
	size_t chars_alloc;                 ///< Characters allocated
	int total_width;                    ///< Total width of all characters
};

static void
row_buffer_init (struct row_buffer *self)
{
	memset (self, 0, sizeof *self);
	self->chars = xcalloc (sizeof *self->chars, (self->chars_alloc = 256));
}

static void
row_buffer_free (struct row_buffer *self)
{
	free (self->chars);
}

/// Replace invalid chars and push all codepoints to the array w/ attributes.
static void
row_buffer_append (struct row_buffer *self, const char *str, chtype attrs)
{
	// The encoding is only really used internally for some corner cases
	const char *encoding = locale_charset ();

	// Note that this function is a hotspot, try to keep it decently fast
	struct row_char current = { .attrs = attrs };
	struct row_char invalid = { .attrs = attrs, .c = '?', .width = 1 };
	const uint8_t *next = (const uint8_t *) str;
	while ((next = u8_next (&current.c, next)))
	{
		if (self->chars_len >= self->chars_alloc)
			self->chars = xreallocarray (self->chars,
				sizeof *self->chars, (self->chars_alloc <<= 1));

		current.width = uc_width (current.c, encoding);
		if (current.width < 0 || !app_is_character_in_locale (current.c))
			current = invalid;

		self->chars[self->chars_len++] = current;
		self->total_width += current.width;
	}
}

static void
row_buffer_addv (struct row_buffer *self, const char *s, ...)
	ATTRIBUTE_SENTINEL;

static void
row_buffer_addv (struct row_buffer *self, const char *s, ...)
{
	va_list ap;
	va_start (ap, s);

	while (s)
	{
		row_buffer_append (self, s, va_arg (ap, chtype));
		s = va_arg (ap, const char *);
	}
	va_end (ap);
}

/// Pop as many codepoints as needed to free up "space" character cells.
/// Given the suffix nature of combining marks, this should work pretty fine.
static int
row_buffer_pop_cells (struct row_buffer *self, int space)
{
	int made = 0;
	while (self->chars_len && made < space)
		made += self->chars[--self->chars_len].width;
	self->total_width -= made;
	return made;
}

static void
row_buffer_space (struct row_buffer *self, int width, chtype attrs)
{
	if (width < 0)
		return;

	while (self->chars_len + width >= self->chars_alloc)
		self->chars = xreallocarray (self->chars,
			sizeof *self->chars, (self->chars_alloc <<= 1));

	struct row_char space = { .attrs = attrs, .c = ' ', .width = 1 };
	self->total_width += width;
	while (width-- > 0)
		self->chars[self->chars_len++] = space;
}

static void
row_buffer_ellipsis (struct row_buffer *self, int target)
{
	if (self->total_width <= target
	 || !row_buffer_pop_cells (self, self->total_width - target))
		return;

	// We use attributes from the last character we've removed,
	// assuming that we don't shrink the array (and there's no real need)
	ucs4_t ellipsis = L'…';
	if (app_is_character_in_locale (ellipsis))
	{
		if (self->total_width >= target)
			row_buffer_pop_cells (self, 1);
		if (self->total_width + 1 <= target)
			row_buffer_append (self, "…",   self->chars[self->chars_len].attrs);
	}
	else if (target >= 3)
	{
		if (self->total_width >= target)
			row_buffer_pop_cells (self, 3);
		if (self->total_width + 3 <= target)
			row_buffer_append (self, "...", self->chars[self->chars_len].attrs);
	}
}

static void
row_buffer_align (struct row_buffer *self, int target, chtype attrs)
{
	row_buffer_ellipsis (self, target);
	row_buffer_space (self, target - self->total_width, attrs);
}

static void
row_buffer_print (uint32_t *ucs4, chtype attrs)
{
	// This assumes that we can reset the attribute set without consequences
	char *str = u32_strconv_to_locale (ucs4);
	if (str)
	{
		attrset (attrs);
		addstr (str);
		attrset (0);
		free (str);
	}
}

static void
row_buffer_flush (struct row_buffer *self)
{
	if (!self->chars_len)
		return;

	// We only NUL-terminate the chunks because of the libunistring API
	uint32_t chunk[self->chars_len + 1], *insertion_point = chunk;
	for (size_t i = 0; i < self->chars_len; i++)
	{
		struct row_char *iter = self->chars + i;
		if (i && iter[0].attrs != iter[-1].attrs)
		{
			row_buffer_print (chunk, iter[-1].attrs);
			insertion_point = chunk;
		}
		*insertion_point++ = iter->c;
		*insertion_point = 0;
	}
	row_buffer_print (chunk, self->chars[self->chars_len - 1].attrs);
}

// --- Rendering ---------------------------------------------------------------

static void
app_invalidate (void)
{
	poller_idle_set (&g_ctx.refresh_event);
}

static void
app_flush_buffer (struct row_buffer *buf, int width, chtype attrs)
{
	row_buffer_align (buf, width, attrs);
	row_buffer_flush (buf);
	row_buffer_free (buf);
}

/// Write the given UTF-8 string padded with spaces.
/// @param[in] attrs  Text attributes for the text, including padding.
static void
app_write_line (const char *str, chtype attrs)
{
	struct row_buffer buf;
	row_buffer_init (&buf);
	row_buffer_append (&buf, str, attrs);
	app_flush_buffer (&buf, COLS, attrs);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
app_flush_header (struct row_buffer *buf, chtype attrs)
{
	move (g_ctx.header_height++, 0);
	app_flush_buffer (buf, COLS, attrs);
}

static void
app_draw_status (void)
{
	// XXX: can we get rid of this and still make it look acceptable?
	chtype a_normal    = APP_ATTR (HEADER);
	chtype a_highlight = APP_ATTR (HIGHLIGHT);

	struct row_buffer buf;
	row_buffer_init (&buf);
	// ...
	app_flush_header (&buf, a_normal);
}

static void
app_draw_header (void)
{
	// TODO: call app_fix_view_range() if it changes from the previous value
	g_ctx.header_height = 0;

	if (true)
		app_draw_status ();
	else
	{
		move (g_ctx.header_height++, 0);
		app_write_line ("Connecting to MPD...", APP_ATTR (HEADER));
	}

	// XXX: can we get rid of this and still make it look acceptable?
	chtype a_normal = APP_ATTR (BAR);
	chtype a_active = APP_ATTR (BAR_ACTIVE);

	struct row_buffer buf;
	row_buffer_init (&buf);

	// TODO: print the filename here instead
	row_buffer_append (&buf, APP_TITLE, a_normal);
	row_buffer_append (&buf, " ", a_normal);

	// TODO: endian indication, position indication
	app_flush_header (&buf, a_normal);
}

static int
app_visible_items (void)
{
	// This may eventually include a header bar and/or a status bar
	return MAX (0, LINES - g_ctx.header_height);
}

static void
app_draw_view (void)
{
	move (g_ctx.header_height, 0);
	clrtobot ();

	int view_width = COLS;

	int to_show = MIN (LINES - g_ctx.header_height,
		(int) g_ctx.item_count - g_ctx.item_top);
	for (int row = 0; row < to_show; row++)
	{
		int item_index = g_ctx.item_top + row;
		int row_attrs = (item_index & 1) ? APP_ATTR (ODD) : APP_ATTR (EVEN);
		if (item_index == g_ctx.item_selected)
			row_attrs = APP_ATTR (SELECTION);

		struct row_buffer buf;
		row_buffer_init (&buf);
		// TODO: draw the row using view_width

		// Combine attributes used by the handler with the defaults.
		// Avoiding attrset() because of row_buffer_flush().
		for (size_t i = 0; i < buf.chars_len; i++)
		{
			chtype *attrs = &buf.chars[i].attrs;
			if (item_index == g_ctx.item_selected)
				*attrs = (*attrs & ~(A_COLOR | A_REVERSE)) | row_attrs;
			else if ((*attrs & A_COLOR) && (row_attrs & A_COLOR))
				*attrs |= (row_attrs & ~A_COLOR);
			else
				*attrs |=  row_attrs;
		}

		move (g_ctx.header_height + row, 0);
		app_flush_buffer (&buf, view_width, row_attrs);
	}
}

static void
app_on_refresh (void *user_data)
{
	(void) user_data;
	poller_idle_reset (&g_ctx.refresh_event);

	app_draw_header ();
	app_draw_view ();

	refresh ();
}

// --- Actions -----------------------------------------------------------------

/// Checks what items are visible and returns if fixes were needed
static bool
app_fix_view_range (void)
{
	if (g_ctx.item_top < 0)
	{
		g_ctx.item_top = 0;
		app_invalidate ();
		return false;
	}

	// If the contents are at least as long as the screen, always fill it
	int max_item_top = (int) g_ctx.item_count - app_visible_items ();
	// But don't let that suggest a negative offset
	max_item_top = MAX (max_item_top, 0);

	if (g_ctx.item_top > max_item_top)
	{
		g_ctx.item_top = max_item_top;
		app_invalidate ();
		return false;
	}
	return true;
}

/// Scroll down (positive) or up (negative) @a n items
static bool
app_scroll (int n)
{
	g_ctx.item_top += n;
	app_invalidate ();
	return app_fix_view_range ();
}

static void
app_ensure_selection_visible (void)
{
	if (g_ctx.item_selected < 0)
		return;

	int too_high = g_ctx.item_top - g_ctx.item_selected;
	if (too_high > 0)
		app_scroll (-too_high);

	int too_low = g_ctx.item_selected
		- (g_ctx.item_top + app_visible_items () - 1);
	if (too_low > 0)
		app_scroll (too_low);
}

static bool
app_move_selection (int diff)
{
	int fixed = g_ctx.item_selected += diff;
	fixed = MAX (fixed, 0);
	fixed = MIN (fixed, (int) g_ctx.item_count - 1);

	bool result = g_ctx.item_selected != fixed;
	g_ctx.item_selected = fixed;
	app_invalidate ();

	app_ensure_selection_visible ();
	return result;
}

// --- User input handling -----------------------------------------------------

enum action
{
	ACTION_NONE,
	ACTION_QUIT,
	ACTION_REDRAW,
	ACTION_CHOOSE,
	ACTION_DELETE,
	ACTION_SCROLL_UP,
	ACTION_SCROLL_DOWN,
	ACTION_GOTO_TOP,
	ACTION_GOTO_BOTTOM,
	ACTION_GOTO_ITEM_PREVIOUS,
	ACTION_GOTO_ITEM_NEXT,
	ACTION_GOTO_PAGE_PREVIOUS,
	ACTION_GOTO_PAGE_NEXT,
	ACTION_COUNT
};

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static bool
app_process_action (enum action action)
{
	switch (action)
	{
	case ACTION_QUIT:
		app_quit ();
		break;
	case ACTION_REDRAW:
		clear ();
		app_invalidate ();
		break;

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

		// XXX: these should rather be parametrized
	case ACTION_SCROLL_UP:
		app_scroll (-3);
		break;
	case ACTION_SCROLL_DOWN:
		app_scroll (3);
		break;

	case ACTION_GOTO_TOP:
		if (g_ctx.item_count)
		{
			g_ctx.item_selected = 0;
			app_ensure_selection_visible ();
			app_invalidate ();
		}
		break;
	case ACTION_GOTO_BOTTOM:
		if (g_ctx.item_count)
		{
			g_ctx.item_selected = (int) g_ctx.item_count - 1;
			app_ensure_selection_visible ();
			app_invalidate ();
		}
		break;

	case ACTION_GOTO_ITEM_PREVIOUS:
		app_move_selection (-1);
		break;
	case ACTION_GOTO_ITEM_NEXT:
		app_move_selection (1);
		break;

	case ACTION_GOTO_PAGE_PREVIOUS:
		app_scroll ((int) g_ctx.header_height - LINES);
		app_move_selection ((int) g_ctx.header_height - LINES);
		break;
	case ACTION_GOTO_PAGE_NEXT:
		app_scroll (LINES - (int) g_ctx.header_height);
		app_move_selection (LINES - (int) g_ctx.header_height);
		break;

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

	case ACTION_NONE:
		break;
	default:
		beep ();
		return false;
	}
	return true;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static bool
app_process_left_mouse_click (int line, int column)
{
	if (line == g_ctx.header_height - 1)
	{
	}
	else
	{
		int row_index = line - g_ctx.header_height;
		if (row_index < 0
		 || row_index >= (int) g_ctx.item_count - g_ctx.item_top)
			return false;

		g_ctx.item_selected = row_index + g_ctx.item_top;
		app_invalidate ();
	}
	return true;
}

static bool
app_process_mouse (termo_mouse_event_t type, int line, int column, int button)
{
	if (type != TERMO_MOUSE_PRESS)
		return true;

	if (button == 1)
		return app_process_left_mouse_click (line, column);
	else if (button == 4)
		return app_process_action (ACTION_SCROLL_UP);
	else if (button == 5)
		return app_process_action (ACTION_SCROLL_DOWN);
	return false;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static struct binding
{
	const char *key;                    ///< Key definition
	enum action action;                 ///< Action to take
}
g_default_bindings[] =
{
	{ "Escape",     ACTION_QUIT               },
	{ "q",          ACTION_QUIT               },
	{ "C-l",        ACTION_REDRAW             },
	// TODO: Tab switches endianity

	{ "Home",       ACTION_GOTO_TOP           },
	{ "End",        ACTION_GOTO_BOTTOM        },
	{ "M-<",        ACTION_GOTO_TOP           },
	{ "M->",        ACTION_GOTO_BOTTOM        },
	{ "Up",         ACTION_GOTO_ITEM_PREVIOUS },
	{ "Down",       ACTION_GOTO_ITEM_NEXT     },
	{ "k",          ACTION_GOTO_ITEM_PREVIOUS },
	{ "j",          ACTION_GOTO_ITEM_NEXT     },
	{ "PageUp",     ACTION_GOTO_PAGE_PREVIOUS },
	{ "PageDown",   ACTION_GOTO_PAGE_NEXT     },
	{ "C-p",        ACTION_GOTO_ITEM_PREVIOUS },
	{ "C-n",        ACTION_GOTO_ITEM_NEXT     },
	{ "C-b",        ACTION_GOTO_PAGE_PREVIOUS },
	{ "C-f",        ACTION_GOTO_PAGE_NEXT     },

	// Not sure how to set these up, they're pretty arbitrary so far
	{ "Enter",      ACTION_CHOOSE             },
	{ "Delete",     ACTION_DELETE             },
};

static bool
app_process_termo_event (termo_key_t *event)
{
	// TODO: pre-parse the keys, order them by termo_keycmp() and binary search
	for (size_t i = 0; i < N_ELEMENTS (g_default_bindings); i++)
	{
		struct binding *binding = &g_default_bindings[i];
		termo_key_t key;
		hard_assert (!*termo_strpkey_utf8 (g_ctx.tk, binding->key, &key,
			TERMO_FORMAT_ALTISMETA));
		if (!termo_keycmp (g_ctx.tk, event, &key))
			return app_process_action (binding->action);
	}
	// TODO: use 0-9 a-f to overwrite nibbles
	return false;
}

// --- Signals -----------------------------------------------------------------

static int g_signal_pipe[2];            ///< A pipe used to signal... signals

/// Program termination has been requested by a signal
static volatile sig_atomic_t g_termination_requested;
/// The window has changed in size
static volatile sig_atomic_t g_winch_received;

static void
signals_postpone_handling (char id)
{
	int original_errno = errno;
	if (write (g_signal_pipe[1], &id, 1) == -1)
		soft_assert (errno == EAGAIN);
	errno = original_errno;
}

static void
signals_superhandler (int signum)
{
	switch (signum)
	{
	case SIGWINCH:
		g_winch_received = true;
		signals_postpone_handling ('w');
		break;
	case SIGINT:
	case SIGTERM:
		g_termination_requested = true;
		signals_postpone_handling ('t');
		break;
	default:
		hard_assert (!"unhandled signal");
	}
}

static void
signals_setup_handlers (void)
{
	if (pipe (g_signal_pipe) == -1)
		exit_fatal ("%s: %s", "pipe", strerror (errno));

	set_cloexec (g_signal_pipe[0]);
	set_cloexec (g_signal_pipe[1]);

	// So that the pipe cannot overflow; it would make write() block within
	// the signal handler, which is something we really don't want to happen.
	// The same holds true for read().
	set_blocking (g_signal_pipe[0], false);
	set_blocking (g_signal_pipe[1], false);

	signal (SIGPIPE, SIG_IGN);

	struct sigaction sa;
	sa.sa_flags = SA_RESTART;
	sa.sa_handler = signals_superhandler;
	sigemptyset (&sa.sa_mask);

	if (sigaction (SIGWINCH, &sa, NULL) == -1
	 || sigaction (SIGINT,   &sa, NULL) == -1
	 || sigaction (SIGTERM,  &sa, NULL) == -1)
		exit_fatal ("sigaction: %s", strerror (errno));
}

// --- Initialisation, event handling ------------------------------------------

static void
app_on_tty_readable (const struct pollfd *fd, void *user_data)
{
	(void) user_data;
	if (fd->revents & ~(POLLIN | POLLHUP | POLLERR))
		print_debug ("fd %d: unexpected revents: %d", fd->fd, fd->revents);

	poller_timer_reset (&g_ctx.tk_timer);
	termo_advisereadable (g_ctx.tk);

	termo_key_t event;
	termo_result_t res;
	while ((res = termo_getkey (g_ctx.tk, &event)) == TERMO_RES_KEY)
	{
		int y, x, button;
		termo_mouse_event_t type;
		if (termo_interpret_mouse (g_ctx.tk, &event, &type, &button, &y, &x))
		{
			if (!app_process_mouse (type, y, x, button))
				beep ();
		}
		else if (!app_process_termo_event (&event))
			beep ();
	}

	if (res == TERMO_RES_AGAIN)
		poller_timer_set (&g_ctx.tk_timer, termo_get_waittime (g_ctx.tk));
	else if (res == TERMO_RES_ERROR || res == TERMO_RES_EOF)
		app_quit ();
}

static void
app_on_key_timer (void *user_data)
{
	(void) user_data;

	termo_key_t event;
	if (termo_getkey_force (g_ctx.tk, &event) == TERMO_RES_KEY)
		if (!app_process_termo_event (&event))
			app_quit ();
}

static void
app_on_signal_pipe_readable (const struct pollfd *fd, void *user_data)
{
	(void) user_data;

	char id = 0;
	(void) read (fd->fd, &id, 1);

	if (g_termination_requested && !g_ctx.quitting)
		app_quit ();

	if (g_winch_received)
	{
		update_curses_terminal_size ();
		app_fix_view_range ();
		app_invalidate ();

		g_winch_received = false;
	}
}

static void
app_log_handler (void *user_data, const char *quote, const char *fmt,
	va_list ap)
{
	// We certainly don't want to end up in a possibly infinite recursion
	static bool in_processing;
	if (in_processing)
		return;

	in_processing = true;

	struct str message;
	str_init (&message);
	str_append (&message, quote);
	str_append_vprintf (&message, fmt, ap);

	// If the standard error output isn't redirected, try our best at showing
	// the message to the user
	if (!isatty (STDERR_FILENO))
		fprintf (stderr, "%s\n", message.str);
	else
	{
		// TODO: think of a location to print this, maybe over decoding fields
		// TODO: remember the position and restore it
		move (LINES - 1, 0);
		app_write_line (message.str, A_REVERSE);
	}
	str_free (&message);

	in_processing = false;
}

static void
app_init_poller_events (void)
{
	poller_fd_init (&g_ctx.signal_event, &g_ctx.poller, g_signal_pipe[0]);
	g_ctx.signal_event.dispatcher = app_on_signal_pipe_readable;
	poller_fd_set (&g_ctx.signal_event, POLLIN);

	poller_fd_init (&g_ctx.tty_event, &g_ctx.poller, STDIN_FILENO);
	g_ctx.tty_event.dispatcher = app_on_tty_readable;
	poller_fd_set (&g_ctx.tty_event, POLLIN);

	poller_timer_init (&g_ctx.tk_timer, &g_ctx.poller);
	g_ctx.tk_timer.dispatcher = app_on_key_timer;

	poller_idle_init (&g_ctx.refresh_event, &g_ctx.poller);
	g_ctx.refresh_event.dispatcher = app_on_refresh;
}

/// Decode size arguments according to similar rules to those that dd(1) uses;
/// we support octal and hexadecimal numbers but they clash with suffixes
static bool
decode_size (const char *s, uint64_t *out)
{
	char *end;
	errno = 0;
	uint64_t n = strtoul (s, &end, 0);
	if (errno != 0 || end == s)
		return false;

	uint64_t f = 1;
	switch (*end)
	{
	case 'c': f = 1 <<  0;                               end++;   break;
	case 'w': f = 1 <<  1;                               end++;   break;
	case 'b': f = 1 <<  9;                               end++;   break;

	case 'K': f = 1 << 10; if (*++end == 'B') { f = 1e3; end++; } break;
	case 'M': f = 1 << 20; if (*++end == 'B') { f = 1e6; end++; } break;
	case 'G': f = 1 << 30; if (*++end == 'B') { f = 1e9; end++; } break;
	}
	if (*end || n > UINT64_MAX / f)
		return false;

	*out = n * f;
	return true;
}

int
main (int argc, char *argv[])
{
	static const struct opt opts[] =
	{
		{ 'd', "debug", NULL, 0, "run in debug mode" },
		{ 'h', "help", NULL, 0, "display this help and exit" },
		{ 'V', "version", NULL, 0, "output version information and exit" },

		{ 'o', "offset", NULL, 0, "offset within the file" },
		{ 's', "size", NULL, 0, "size limit (1G by default)" },
		{ 0, NULL, NULL, 0, NULL }
	};

	struct opt_handler oh;
	opt_handler_init (&oh, argc, argv, opts, "[FILE]", "Hex viewer.");
	uint64_t size_limit = 1 << 30;

	int c;
	while ((c = opt_handler_get (&oh)) != -1)
	switch (c)
	{
	case 'd':
		g_debug_mode = true;
		break;
	case 'h':
		opt_handler_usage (&oh, stdout);
		exit (EXIT_SUCCESS);
	case 'V':
		printf (PROGRAM_NAME " " PROGRAM_VERSION "\n");
		exit (EXIT_SUCCESS);

	case 'o':
		if (!decode_size (optarg, &g_ctx.data_offset))
			exit_fatal ("invalid offset specified");
		break;
	case 's':
		if (!decode_size (optarg, &size_limit))
			exit_fatal ("invalid size limit specified");
		break;
	default:
		print_error ("wrong options");
		opt_handler_usage (&oh, stderr);
		exit (EXIT_FAILURE);
	}

	argc -= optind;
	argv += optind;

	// When no filename is given, read from stdin and replace it with the tty
	int input_fd;
	if (argc == 0)
	{
		if ((input_fd = dup (STDIN_FILENO)) < 0)
			exit_fatal ("cannot read input: %s", strerror (errno));
		close (STDIN_FILENO);
		if (open ("/dev/tty", O_RDWR))
			exit_fatal ("cannot open the terminal: %s", strerror (errno));
	}
	else if (argc == 1)
	{
		g_ctx.filename = xstrdup (argv[0]);
		if (!(input_fd = open (argv[0], O_RDONLY)))
			exit_fatal ("cannot open `%s': %s", argv[0], strerror (errno));
	}
	else
	{
		opt_handler_usage (&oh, stderr);
		exit (EXIT_FAILURE);
	}
	opt_handler_free (&oh);

	// Seek in the file or pipe however we can
	static char seek_buf[8192];
	if (lseek (input_fd, g_ctx.data_offset, SEEK_SET) == (off_t) -1)
		for (uint64_t remaining = g_ctx.data_offset; remaining; )
		{
			ssize_t n_read = read (input_fd,
				seek_buf, MIN (remaining, sizeof seek_buf));
			if (n_read <= 0)
				exit_fatal ("cannot seek: %s", strerror (errno));
			remaining -= n_read;
		}

	// Read up to "size_limit" bytes of data into a buffer
	struct str buf;
	str_init (&buf);

	while (buf.len < size_limit)
	{
		str_ensure_space (&buf, 8192);
		ssize_t n_read = read (input_fd, buf.str + buf.len,
			MIN (size_limit - buf.len, buf.alloc - buf.len));
		if (!n_read)
			break;
		if (n_read == -1)
			exit_fatal ("cannot read input: %s", strerror (errno));
		buf.len += n_read;
	}

	g_ctx.data = (uint8_t *) buf.str;
	g_ctx.data_len = buf.len;

	// We only need to convert to and from the terminal encoding
	if (!setlocale (LC_CTYPE, ""))
		print_warning ("failed to set the locale");

	app_init_context ();
	app_load_configuration ();
	app_init_terminal ();
	signals_setup_handlers ();
	app_init_poller_events ();

	// Redirect all messages from liberty so that they don't disrupt display
	g_log_message_real = app_log_handler;

	g_ctx.polling = true;
	while (g_ctx.polling)
		poller_run (&g_ctx.poller);

	endwin ();
	g_log_message_real = log_message_stdio;
	app_free_context ();
	return 0;
}
