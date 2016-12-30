/*
 * hex -- hex viewer
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
#define ATTRIBUTE_TABLE(XX)                                    \
	XX( FOOTER,     "footer",     -1, -1, 0                  ) \
	XX( FOOTER_HL,  "footer_hl",  -1, -1, A_BOLD             ) \
	/* Bar                                                  */ \
	XX( BAR,        "bar",        -1, -1, A_REVERSE          ) \
	XX( BAR_HL,     "bar_hl",     -1, -1, A_REVERSE | A_BOLD ) \
	/* View                                                 */ \
	XX( EVEN,       "even",       -1, -1, 0                  ) \
	XX( ODD,        "odd",        -1, -1, 0                  ) \
	XX( SELECTION,  "selection",  -1, -1, A_REVERSE          ) \
	/* These are for debugging only                         */ \
	XX( WARNING,    "warning",     3, -1, 0                  ) \
	XX( ERROR,      "error",       1, -1, 0                  )

enum
{
#define XX(name, config, fg_, bg_, attrs_) ATTRIBUTE_ ## name,
	ATTRIBUTE_TABLE (XX)
#undef XX
	ATTRIBUTE_COUNT
};

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

#include "tui.c"
#include "termo.h"

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

// --- Application -------------------------------------------------------------

enum
{
	ROW_SIZE = 16,                      ///< How many bytes on a row
};

enum endianity
{
	ENDIANITY_LE,                       ///< Little endian
	ENDIANITY_BE                        ///< Big endian
};

static struct app_context
{
	// Event loop:

	struct poller poller;               ///< Poller
	bool quitting;                      ///< Quit signal for the event loop
	bool polling;                       ///< The event loop is running

	struct poller_fd tty_event;         ///< Terminal input event
	struct poller_fd signal_event;      ///< Signal FD event

	// Data:

	char *message;                      ///< Last logged message
	int message_attr;                   ///< Attributes for the logged message

	struct config config;               ///< Program configuration
	char *filename;                     ///< Target filename

	uint8_t *data;                      ///< Target data
	int64_t data_len;                   ///< Length of the data
	int64_t data_offset;                ///< Offset of the data within the file

	// View:

	int64_t view_top;                   ///< Offset of the top of the screen
	int64_t view_cursor;                ///< Offset of the cursor
	bool view_skip_nibble;              ///< Half-byte offset

	enum endianity endianity;           ///< Endianity

	// Emulated widgets:

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

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
load_config_colors (struct config_item *subtree, void *user_data)
{
	config_schema_apply_to_object (g_config_colors,   subtree, user_data);

	// The attributes cannot be changed dynamically right now, so it doesn't
	// make much sense to make use of "on_change" callbacks either.
	// For simplicity, we should reload the entire table on each change anyway.
	const char *value;
#define XX(name, config, fg_, bg_, attrs_) \
	if ((value = get_config_string (subtree, config))) \
		g_ctx.attrs[ATTRIBUTE_ ## name] = attrs_decode (value);
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

	free (g_ctx.message);

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

static int
app_visible_rows (void)
{
	return MAX (0, LINES - 1 /* bar */ - 3 /* decoder */ - !!g_ctx.message);
}

static void
app_make_row (struct row_buffer *buf, int64_t addr, int attrs)
{
	char *row_addr_str = xstrdup_printf ("%08" PRIx64, addr);
	row_buffer_append (buf, row_addr_str, attrs);
	free (row_addr_str);

	struct str ascii;
	str_init (&ascii);
	str_append (&ascii, "  ");

	int64_t end_addr = g_ctx.data_offset + g_ctx.data_len;
	for (int x = 0; x < ROW_SIZE; x++)
	{
		if (x % 8 == 0) row_buffer_append (buf, " ", attrs);
		if (x % 2 == 0) row_buffer_append (buf, " ", attrs);

		int64_t cell_addr = addr + x;
		if (cell_addr < g_ctx.data_offset
		 || cell_addr >= end_addr)
		{
			row_buffer_append (buf, "  ", attrs);
			str_append_c (&ascii, ' ');
		}
		else
		{
			uint8_t cell = g_ctx.data[cell_addr - g_ctx.data_offset];
			char *hex = xstrdup_printf ("%02x", cell);
			row_buffer_append (buf, hex, attrs);
			free (hex);

			str_append_c (&ascii, (cell >= 32 && cell < 127) ? cell : '.');
		}
	}
	row_buffer_append (buf, ascii.str, attrs);
	str_free (&ascii);
}

static void
app_draw_view (void)
{
	move (0, 0);

	int64_t end_addr = g_ctx.data_offset + g_ctx.data_len;
	for (int y = 0; y < app_visible_rows (); y++)
	{
		int64_t addr = g_ctx.view_top + y * ROW_SIZE;
		if (addr >= end_addr)
			break;

		int attrs = (addr / ROW_SIZE & 1) ? APP_ATTR (ODD) : APP_ATTR (EVEN);

		struct row_buffer buf;
		row_buffer_init (&buf);
		app_make_row (&buf, addr, attrs);
		app_flush_buffer (&buf, COLS, attrs);
	}
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static uint64_t
app_decode (const uint8_t *p, size_t len)
{
	uint64_t val = 0;
	if (g_ctx.endianity == ENDIANITY_BE)
		for (size_t i = 0; i < len; i++)
			val = val << 8 | (uint64_t) p[i];
	else
		while (len--)
			val = val << 8 | (uint64_t) p[len];
	return val;
}

static void
app_write_footer (struct row_buffer *b, char id, int len, const char *fmt, ...)
	ATTRIBUTE_PRINTF (4, 5);

static void
app_footer_field (struct row_buffer *b, char id, int len, const char *fmt, ...)
{
	const char *coding;
	if (len <= 1)
		coding = "";
	else if (g_ctx.endianity == ENDIANITY_LE)
		coding = "le";
	else if (g_ctx.endianity == ENDIANITY_BE)
		coding = "be";

	char *key = xstrdup_printf ("%c%d%s", id, len * 8, coding);
	row_buffer_append (b, key, APP_ATTR (FOOTER_HL));
	free (key);

	struct str value;
	str_init (&value);

	va_list ap;
	va_start (ap, fmt);
	str_append_vprintf (&value, fmt, ap);
	va_end (ap);

	row_buffer_append (b, value.str, APP_ATTR (FOOTER));
	str_free (&value);
}

static void
app_draw_footer (void)
{
	move (app_visible_rows (), 0);

	struct row_buffer buf;
	row_buffer_init (&buf);
	row_buffer_append (&buf, APP_TITLE, APP_ATTR (BAR));

	if (g_ctx.filename)
	{
		row_buffer_append (&buf, "  ", APP_ATTR (BAR));
		char *filename = (char *) u8_strconv_from_locale (g_ctx.filename);
		row_buffer_append (&buf, filename, APP_ATTR (BAR_HL));
		free (filename);
	}

	struct str right;
	str_init (&right);
	str_append_printf (&right, "  %08" PRIx64, g_ctx.view_cursor);
	str_append (&right, g_ctx.endianity == ENDIANITY_LE ? "  LE  " : "  BE  ");

	int64_t top = g_ctx.view_top;
	int64_t bot = g_ctx.view_top + app_visible_rows () * ROW_SIZE;
	if (top <= g_ctx.data_offset
	 && bot >= g_ctx.data_offset + g_ctx.data_len)
		str_append (&right, "All");
	else if (top <= g_ctx.data_offset)
		str_append (&right, "Top");
	else if (bot >= g_ctx.data_offset + g_ctx.data_len)
		str_append (&right, "Bot");
	else
	{
		int64_t end_addr = g_ctx.data_offset + g_ctx.data_len;
		int64_t cur = g_ctx.view_top / ROW_SIZE;
		int64_t max = (end_addr - 1) / ROW_SIZE - app_visible_rows () + 1;

		cur -= g_ctx.data_offset / ROW_SIZE;
		max -= g_ctx.data_offset / ROW_SIZE;
		str_append_printf (&right, "%2d%%", (int) (100 * cur / max));
	}

	row_buffer_align (&buf, COLS - right.len, APP_ATTR (BAR));
	row_buffer_append (&buf, right.str, APP_ATTR (BAR));
	app_flush_buffer (&buf, COLS, APP_ATTR (BAR));

	int64_t end_addr = g_ctx.data_offset + g_ctx.data_len;
	if (g_ctx.view_cursor < g_ctx.data_offset
	 || g_ctx.view_cursor >= end_addr)
		return;

	int64_t len = end_addr - g_ctx.view_cursor;
	uint8_t *p = g_ctx.data + (g_ctx.view_cursor - g_ctx.data_offset);

	struct row_buffer x; row_buffer_init (&x);
	struct row_buffer u; row_buffer_init (&u);
	struct row_buffer s; row_buffer_init (&s);

	if (len >= 1)
	{
		app_footer_field (&x, 'x', 1, "   %02x  ", p[0]);
		app_footer_field (&u, 'u', 1, " %4u  ", p[0]);
		app_footer_field (&s, 's', 1, " %4d  ", (int8_t) p[0]);
	}
	if (len >= 2)
	{
		uint16_t val = app_decode (p, 2);
		app_footer_field (&x, 'x', 2, "   %04x  ", val);
		app_footer_field (&u, 'u', 2, " %6u  ", val);
		app_footer_field (&s, 's', 2, " %6d  ", (int16_t) val);
	}
	if (len >= 4)
	{
		uint32_t val = app_decode (p, 4);
		app_footer_field (&x, 'x', 4, "    %08x  ", val);
		app_footer_field (&u, 'u', 4, " %11u  ", val);
		app_footer_field (&s, 's', 4, " %11d  ", (int32_t) val);
	}
	if (len >= 8)
	{
		uint64_t val = app_decode (p, 8);
		app_footer_field (&x, 'x', 8, "     %016" PRIx64, val);
		app_footer_field (&u, 'u', 8, " %20" PRIu64, val);
		app_footer_field (&s, 's', 8, " %20" PRId64, (int64_t) val);
	}

	app_flush_buffer (&x, COLS, APP_ATTR (FOOTER));
	app_flush_buffer (&u, COLS, APP_ATTR (FOOTER));
	app_flush_buffer (&s, COLS, APP_ATTR (FOOTER));

	if (g_ctx.message)
		app_write_line (g_ctx.message, g_ctx.attrs[g_ctx.message_attr].attrs);
}

static void
app_on_refresh (void *user_data)
{
	(void) user_data;
	poller_idle_reset (&g_ctx.refresh_event);

	erase ();
	app_draw_view ();
	app_draw_footer ();

	int64_t diff = g_ctx.view_cursor - g_ctx.view_top;
	int64_t y = diff / ROW_SIZE;
	int64_t x = diff % ROW_SIZE;
	if (y >= 0 && y < app_visible_rows ())
	{
		curs_set (1);
		move (y, 10 + x*2 + g_ctx.view_skip_nibble + x/8 + x/2);
	}
	else
		curs_set (0);

	refresh ();
}

// --- Actions -----------------------------------------------------------------

/// Checks what items are visible and returns if fixes were needed
static bool
app_fix_view_range (void)
{
	int64_t data_view_start = g_ctx.data_offset / ROW_SIZE * ROW_SIZE;
	if (g_ctx.view_top < data_view_start)
	{
		g_ctx.view_top = data_view_start;
		app_invalidate ();
		return false;
	}

	// If the contents are at least as long as the screen, always fill it
	int64_t last_byte = g_ctx.data_offset + g_ctx.data_len - 1;
	int64_t max_view_top =
		(last_byte / ROW_SIZE - app_visible_rows () + 1) * ROW_SIZE;
	// But don't let that suggest a negative offset
	max_view_top = MAX (max_view_top, 0);

	if (g_ctx.view_top > max_view_top)
	{
		g_ctx.view_top = max_view_top;
		app_invalidate ();
		return false;
	}
	return true;
}

/// Scroll down (positive) or up (negative) @a n items
static bool
app_scroll (int n)
{
	g_ctx.view_top += n * ROW_SIZE;
	app_invalidate ();
	return app_fix_view_range ();
}

static void
app_ensure_selection_visible (void)
{
	int too_high = g_ctx.view_top / ROW_SIZE - g_ctx.view_cursor / ROW_SIZE;
	if (too_high > 0)
		app_scroll (-too_high);

	int too_low = g_ctx.view_cursor / ROW_SIZE - g_ctx.view_top / ROW_SIZE
		- app_visible_rows () + 1;
	if (too_low > 0)
		app_scroll (too_low);
}

static bool
app_move_cursor_by_rows (int diff)
{
	// TODO: disallow partial up/down movement
	int64_t fixed = g_ctx.view_cursor += diff * ROW_SIZE;
	fixed = MAX (fixed, g_ctx.data_offset);
	fixed = MIN (fixed, g_ctx.data_offset + g_ctx.data_len - 1);

	bool result = g_ctx.view_cursor == fixed;
	g_ctx.view_cursor = fixed;
	app_invalidate ();

	app_ensure_selection_visible ();
	return result;
}

// --- User input handling -----------------------------------------------------

enum action
{
	ACTION_NONE, ACTION_QUIT, ACTION_REDRAW, ACTION_TOGGLE_ENDIANITY,

	ACTION_SCROLL_UP,   ACTION_GOTO_TOP,    ACTION_GOTO_PAGE_PREVIOUS,
	ACTION_SCROLL_DOWN, ACTION_GOTO_BOTTOM, ACTION_GOTO_PAGE_NEXT,

	ACTION_UP, ACTION_DOWN, ACTION_LEFT, ACTION_RIGHT,

	ACTION_COUNT
};

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static bool
app_process_action (enum action action)
{
	switch (action)
	{
		// XXX: these should rather be parametrized
	case ACTION_SCROLL_UP:   app_scroll (-1); break;
	case ACTION_SCROLL_DOWN: app_scroll  (1); break;

	case ACTION_GOTO_TOP:
		g_ctx.view_cursor = g_ctx.data_offset;
		g_ctx.view_skip_nibble = false;
		app_ensure_selection_visible ();
		app_invalidate ();
		break;
	case ACTION_GOTO_BOTTOM:
		if (!g_ctx.data_len)
			return false;

		g_ctx.view_cursor = g_ctx.data_offset + g_ctx.data_len - 1;
		g_ctx.view_skip_nibble = false;
		app_ensure_selection_visible ();
		app_invalidate ();
		break;

	case ACTION_GOTO_PAGE_PREVIOUS:
		app_scroll (-app_visible_rows ());
		app_move_cursor_by_rows (-app_visible_rows ());
		break;
	case ACTION_GOTO_PAGE_NEXT:
		app_scroll (app_visible_rows ());
		app_move_cursor_by_rows (app_visible_rows ());
		break;

	case ACTION_UP:   app_move_cursor_by_rows (-1); break;
	case ACTION_DOWN: app_move_cursor_by_rows  (1); break;

	case ACTION_LEFT:
		if (g_ctx.view_skip_nibble)
			g_ctx.view_skip_nibble = false;
		else
		{
			if (g_ctx.view_cursor <= g_ctx.data_offset)
				return false;

			g_ctx.view_skip_nibble = true;
			g_ctx.view_cursor--;
			app_ensure_selection_visible ();
		}
		app_invalidate ();
		break;
	case ACTION_RIGHT:
		if (!g_ctx.view_skip_nibble)
			g_ctx.view_skip_nibble = true;
		else
		{
			if (g_ctx.view_cursor >= g_ctx.data_offset + g_ctx.data_len - 1)
				return false;

			g_ctx.view_skip_nibble = false;
			g_ctx.view_cursor++;
			app_ensure_selection_visible ();
		}
		app_invalidate ();
		break;

	case ACTION_QUIT:
		app_quit ();
	case ACTION_NONE:
		break;
	case ACTION_REDRAW:
		clear ();
		app_invalidate ();
		break;

	case ACTION_TOGGLE_ENDIANITY:
		g_ctx.endianity = (g_ctx.endianity == ENDIANITY_LE)
			? ENDIANITY_BE : ENDIANITY_LE;
		app_invalidate ();
		break;
	default:
		return false;
	}
	return true;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static bool
app_process_left_mouse_click (int line, int column)
{
	if (line < 0)
		return false;

	if (line == app_visible_rows ())
	{
		if (column <  COLS - 7
		 || column >= COLS - 5)
			return false;
		return app_process_action (ACTION_TOGGLE_ENDIANITY);
	}
	else if (line < app_visible_rows ())
	{
		// TODO: employ strict checking here before the autofix
		int offset;
		if (column >= 10 && column < 50)
		{
			offset = column - 10;
			offset -= offset/5 + offset/21;
			g_ctx.view_skip_nibble = offset % 2;
			offset /= 2;
		}
		else if (column >= 52 && column < 68)
		{
			offset = column - 52;
			g_ctx.view_skip_nibble = false;
		}
		else
			return false;

		g_ctx.view_cursor = g_ctx.view_top + line * ROW_SIZE + offset;
		return app_move_cursor_by_rows (0);
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
	termo_key_t decoded;                ///< Decoded key definition
}
g_default_bindings[] =
{
	{ "Escape",     ACTION_QUIT,               {}},
	{ "q",          ACTION_QUIT,               {}},
	{ "C-l",        ACTION_REDRAW,             {}},
	{ "Tab",        ACTION_TOGGLE_ENDIANITY,   {}},

	{ "Home",       ACTION_GOTO_TOP,           {}},
	{ "End",        ACTION_GOTO_BOTTOM,        {}},
	{ "M-<",        ACTION_GOTO_TOP,           {}},
	{ "M->",        ACTION_GOTO_BOTTOM,        {}},
	{ "g",          ACTION_GOTO_TOP,           {}},
	{ "G",          ACTION_GOTO_BOTTOM,        {}},
	{ "PageUp",     ACTION_GOTO_PAGE_PREVIOUS, {}},
	{ "PageDown",   ACTION_GOTO_PAGE_NEXT,     {}},
	{ "C-b",        ACTION_GOTO_PAGE_PREVIOUS, {}},
	{ "C-f",        ACTION_GOTO_PAGE_NEXT,     {}},

	{ "Up",         ACTION_UP,                 {}},
	{ "Down",       ACTION_DOWN,               {}},
	{ "Left",       ACTION_LEFT,               {}},
	{ "Right",      ACTION_RIGHT,              {}},
	{ "k",          ACTION_UP,                 {}},
	{ "j",          ACTION_DOWN,               {}},
	{ "h",          ACTION_LEFT,               {}},
	{ "l",          ACTION_RIGHT,              {}},
	{ "C-p",        ACTION_UP,                 {}},
	{ "C-n",        ACTION_DOWN,               {}},

	{ "C-y",        ACTION_SCROLL_UP,          {}},
	{ "C-e",        ACTION_SCROLL_DOWN,        {}},
};

static int
app_binding_cmp (const void *a, const void *b)
{
	return termo_keycmp (g_ctx.tk,
		&((struct binding *) a)->decoded, &((struct binding *) b)->decoded);
}

static void
app_init_bindings (void)
{
	for (size_t i = 0; i < N_ELEMENTS (g_default_bindings); i++)
	{
		struct binding *binding = &g_default_bindings[i];
		hard_assert (!*termo_strpkey_utf8 (g_ctx.tk,
			binding->key, &binding->decoded, TERMO_FORMAT_ALTISMETA));
	}
	qsort (g_default_bindings, N_ELEMENTS (g_default_bindings),
		sizeof *g_default_bindings, app_binding_cmp);
}

static bool
app_process_termo_event (termo_key_t *event)
{
	struct binding dummy = { NULL, 0, *event }, *binding =
		bsearch (&dummy, g_default_bindings, N_ELEMENTS (g_default_bindings),
			sizeof *g_default_bindings, app_binding_cmp);
	if (binding)
		return app_process_action (binding->action);

	// TODO: once we become an editor, use 0-9 a-f to overwrite nibbles
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
		bool success;
		if (termo_interpret_mouse (g_ctx.tk, &event, &type, &button, &y, &x))
			success = app_process_mouse (type, y, x, button);
		else
			success = app_process_termo_event (&event);

		if (!success)
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
		free (g_ctx.message);
		g_ctx.message = xstrdup (message.str);
		g_ctx.message_attr = (intptr_t) user_data;
		app_invalidate ();
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
decode_size (const char *s, int64_t *out)
{
	char *end;
	errno = 0;
	int64_t n = strtol (s, &end, 0);
	if (errno != 0 || end == s || n < 0)
		return false;

	int64_t f = 1;
	switch (*end)
	{
	case 'c': f = 1 <<  0;                               end++;   break;
	case 'w': f = 1 <<  1;                               end++;   break;
	case 'b': f = 1 <<  9;                               end++;   break;

	case 'K': f = 1 << 10; if (*++end == 'B') { f = 1e3; end++; } break;
	case 'M': f = 1 << 20; if (*++end == 'B') { f = 1e6; end++; } break;
	case 'G': f = 1 << 30; if (*++end == 'B') { f = 1e9; end++; } break;
	}
	if (*end || n > INT64_MAX / f)
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
	int64_t size_limit = 1 << 30;

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

	while (buf.len < (size_t) size_limit)
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

	g_ctx.view_top = g_ctx.data_offset / ROW_SIZE * ROW_SIZE;
	g_ctx.view_cursor = g_ctx.data_offset;

	// We only need to convert to and from the terminal encoding
	if (!setlocale (LC_CTYPE, ""))
		print_warning ("failed to set the locale");

	app_init_context ();
	app_load_configuration ();
	app_init_terminal ();
	signals_setup_handlers ();
	app_init_poller_events ();
	app_invalidate ();
	app_init_bindings ();

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
