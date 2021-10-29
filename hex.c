/*
 * hex -- hex viewer
 *
 * Copyright (c) 2016 - 2017, Přemysl Eric Janouch <p@janouch.name>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted.
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
	XX( FOOTER,     "footer",     -1,  -1, 0                  ) \
	XX( FOOTER_HL,  "footer_hl",  -1,  -1, A_BOLD             ) \
	/* Bar                                                   */ \
	XX( BAR,        "bar",        -1,  -1, A_REVERSE          ) \
	XX( BAR_HL,     "bar_hl",     -1,  -1, A_REVERSE | A_BOLD ) \
	/* View                                                  */ \
	XX( EVEN,       "even",       -1,  -1, 0                  ) \
	XX( ODD,        "odd",        -1,  -1, 0                  ) \
	XX( SELECTION,  "selection",  -1,  -1, A_REVERSE          ) \
	/* Field highlights                                      */ \
	XX( C1,         "c1",         22, 194, 0                  ) \
	XX( C2,         "c2",         88, 224, 0                  ) \
	XX( C3,         "c3",         58, 229, 0                  ) \
	XX( C4,         "c4",         20, 189, 0                  ) \
	/* These are for debugging only                          */ \
	XX( WARNING,    "warning",     3,  -1, 0                  ) \
	XX( ERROR,      "error",       1,  -1, 0                  )

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
#include "liberty/liberty.c"
#include "liberty/liberty-tui.c"

#include <locale.h>
#include <termios.h>
#ifndef TIOCGWINSZ
#include <sys/ioctl.h>
#endif  // ! TIOCGWINSZ

#include "termo.h"

#ifdef HAVE_LUA
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

#include <dirent.h>
#endif // HAVE_LUA

#define APP_TITLE  PROGRAM_NAME         ///< Left top corner

// --- Utilities ---------------------------------------------------------------

// The standard endwin/refresh sequence makes the terminal flicker
static void
update_curses_terminal_size (void)
{
#if defined HAVE_RESIZETERM && defined TIOCGWINSZ
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

struct mark
{
	int64_t offset;                     ///< Offset of the mark
	int64_t len;                        ///< Length of the mark
	size_t description;                 ///< Textual description string offset
};

// XXX: can we avoid constructing the marks_by_offset lookup array?
//   How much memory is it even going to consume in reality?

/// This is the final result suitable for display, including unmarked areas.
/// We might infer `color` from the index of this entry but then unmarked areas
/// would skip a color, which is undesired.
struct marks_by_offset
{
	int64_t offset;                     ///< Offset in the file
	size_t marks;                       ///< Offset into "offset_entries"
	int color;                          ///< Color of the area until next offset
};

static struct app_context
{
	// Event loop:

	struct poller poller;               ///< Poller
	bool quitting;                      ///< Quit signal for the event loop
	bool polling;                       ///< The event loop is running

	struct poller_fd tty_event;         ///< Terminal input event
	struct poller_fd signal_event;      ///< Signal FD event

#ifdef HAVE_LUA
	lua_State *L;                       ///< Lua state
	int ref_format;                     ///< Reference to "string.format"
	struct str_map coders;              ///< Map of coders by name
#endif // HAVE_LUA

	// Data:

	char *message;                      ///< Last logged message
	int message_attr;                   ///< Attributes for the logged message

	struct config config;               ///< Program configuration
	char *filename;                     ///< Target filename

	uint8_t *data;                      ///< Target data
	int64_t data_len;                   ///< Length of the data
	int64_t data_offset;                ///< Offset of the data within the file

	// Field marking:

	ARRAY (struct mark, marks)          ///< Marks
	struct str mark_strings;            ///< Storage for mark descriptions

	ARRAY (struct marks_by_offset, marks_by_offset)
	ARRAY (struct mark *, offset_entries)

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
	g_ctx.config = config_make ();

	ARRAY_INIT (g_ctx.marks);
	g_ctx.mark_strings = str_make ();
	ARRAY_INIT (g_ctx.marks_by_offset);
	ARRAY_INIT (g_ctx.offset_entries);

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
			// FIXME: we need a 256color default palette that fails gracefully
			//   to something like underlined fields
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

	free (g_ctx.marks);
	str_free (&g_ctx.mark_strings);
	free (g_ctx.marks_by_offset);
	free (g_ctx.offset_entries);

	cstr_set (&g_ctx.message, NULL);

	cstr_set (&g_ctx.filename, NULL);
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

// --- Field marking -----------------------------------------------------------

/// Find the "marks_by_offset" span covering the offset (if any)
static ssize_t
app_find_marks (int64_t offset)
{
	ssize_t min = 0, end = g_ctx.marks_by_offset_len;
	while (min < end)
	{
		ssize_t mid = min + (end - min) / 2;
		if (offset >= g_ctx.marks_by_offset[mid].offset)
			min = mid + 1;
		else
			end = mid;
	}
	return min - 1;
}

static struct marks_by_offset *
app_marks_at_offset (int64_t offset)
{
	ssize_t i = app_find_marks (offset);
	if (i < 0 || (size_t) i >= g_ctx.marks_by_offset_len)
		return NULL;

	struct marks_by_offset *marks = &g_ctx.marks_by_offset[i];
	if (marks->offset > offset)
		return NULL;
	return marks;
}

static int
app_mark_cmp (const void *first, const void *second)
{
	const struct mark *a = first, *b = second;
	// This ordering is pretty much arbitrary, seemed to make sense
	if (a->offset < b->offset) return -1;
	if (a->offset > b->offset) return  1;
	if (a->len    < b->len)    return  1;
	if (a->len    > b->len)    return -1;
	return 0;
}

static size_t
app_store_marks (struct mark **entries, size_t len)
{
	size_t result = g_ctx.offset_entries_len;
	ARRAY_RESERVE (g_ctx.offset_entries, len);
	memcpy (g_ctx.offset_entries + g_ctx.offset_entries_len, entries,
		sizeof *entries * len);
	g_ctx.offset_entries_len += len;
	return result;
}

/// Flattens marks into sequential non-overlapping spans suitable for search
/// by offset, assigning different colors to them in the process:
/// @code
///  ________    _______     ___
/// |________|__|_______|   |___|
///     |_________|
///  ___ ____ __ _ _____ ___ ___
/// |___|____|__|_|_____|___|___|
/// @endcode
static void
app_flatten_marks (void)
{
	qsort (g_ctx.marks, g_ctx.marks_len, sizeof *g_ctx.marks, app_mark_cmp);
	if (!g_ctx.marks_len)
		return;

	ARRAY (struct mark *, current)
	ARRAY_INIT (current);
	int current_color = 0;

	// Make offset zero actually point to an empty entry
	g_ctx.offset_entries[g_ctx.offset_entries_len++] = NULL;

	struct mark *next = g_ctx.marks;
	struct mark *end = next + g_ctx.marks_len;
	while (current_len || next < end)
	{
		// Find the closest offset at which marks change
		int64_t closest = g_ctx.data_offset + g_ctx.data_len;
		if (next < end)
			closest = next->offset;
		for (size_t i = 0; i < current_len; i++)
			closest = MIN (closest, current[i]->offset + current[i]->len);

		// Remove from "current" marks that have ended
		for (size_t i = 0; i < current_len; )
			if (closest == current[i]->offset + current[i]->len)
				memmove (current + i, current + i + 1,
					(--current_len - i) * sizeof *current);
			else
				i++;

		// Add any new marks at "closest"
		while (next < end && next->offset == closest)
		{
			current[current_len++] = next++;
			ARRAY_RESERVE (current, 1);
		}
		current[current_len] = NULL;

		// Save marks at that offset to be used by rendering
		size_t marks = 0;
		int color = -1;

		if (current_len)
		{
			marks = app_store_marks (current, current_len + 1);
			color = ATTRIBUTE_C1 + current_color++;
			current_color %= 4;
		}

		ARRAY_RESERVE (g_ctx.marks_by_offset, 1);
		g_ctx.marks_by_offset[g_ctx.marks_by_offset_len++] =
			(struct marks_by_offset) { closest, marks, color };
	}
	free (current);
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
	struct row_buffer buf = row_buffer_make ();
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

	struct row_buffer ascii = row_buffer_make ();
	row_buffer_append (&ascii, "  ", attrs);

	int64_t end_addr = g_ctx.data_offset + g_ctx.data_len;
	const char *hexa = "0123456789abcdef";
	for (int x = 0; x < ROW_SIZE; x++)
	{
		if (x % 8 == 0) row_buffer_append (buf, " ", attrs);
		if (x % 2 == 0) row_buffer_append (buf, " ", attrs);

		int64_t cell_addr = addr + x;
		if (cell_addr < g_ctx.data_offset
		 || cell_addr >= end_addr)
		{
			row_buffer_append (buf, "  ", attrs);
			row_buffer_append (&ascii, " ", attrs);
		}
		else
		{
			int attrs_mark = attrs;
			struct marks_by_offset *marks = app_marks_at_offset (cell_addr);
			if (marks && marks->color >= 0)
				attrs_mark = g_ctx.attrs[marks->color].attrs;

			int highlight = 0;
			if (cell_addr >= g_ctx.view_cursor
			 && cell_addr <  g_ctx.view_cursor + 8)
				highlight = A_UNDERLINE;

			// TODO: leave it up to the user to decide what should be colored
			uint8_t cell = g_ctx.data[cell_addr - g_ctx.data_offset];
			row_buffer_append (buf,
				(char[3]) { hexa[cell >> 4], hexa[cell & 7], 0 },
				attrs | highlight);

			char s[2] = { (cell >= 32 && cell < 127) ? cell : '.', 0 };
			row_buffer_append (&ascii, s, attrs_mark | highlight);
		}
	}
	row_buffer_append_buffer (buf, &ascii);
	row_buffer_free (&ascii);
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

		struct row_buffer buf = row_buffer_make ();
		app_make_row (&buf, addr, attrs);
		app_flush_buffer (&buf, COLS, attrs);
	}
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
app_draw_info (void)
{
	const struct marks_by_offset *marks;
	if (!(marks = app_marks_at_offset (g_ctx.view_cursor)))
		return;

	int x_offset = 70;
	struct mark *mark, **iter = g_ctx.offset_entries + marks->marks;
	for (int y = 0; y < app_visible_rows (); y++)
	{
		// TODO: we can use the field background
		// TODO: we can keep going through subsequent fields to fill the column
		if (!(mark = *iter++))
			break;

		struct row_buffer buf = row_buffer_make ();
		row_buffer_append (&buf,
			g_ctx.mark_strings.str + mark->description, 0);

		move (y, x_offset);
		app_flush_buffer (&buf, COLS - x_offset, 0);
	}
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static uint64_t
app_decode (const uint8_t *p, size_t len, enum endianity endianity)
{
	uint64_t val = 0;
	if (endianity == ENDIANITY_BE)
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

	va_list ap;
	va_start (ap, fmt);
	struct str value = str_make ();
	str_append_vprintf (&value, fmt, ap);
	va_end (ap);

	row_buffer_append (b, value.str, APP_ATTR (FOOTER));
	str_free (&value);
}

static void
app_draw_footer (void)
{
	move (app_visible_rows (), 0);

	struct row_buffer buf = row_buffer_make ();
	row_buffer_append (&buf, APP_TITLE, APP_ATTR (BAR));

	if (g_ctx.filename)
	{
		row_buffer_append (&buf, "  ", APP_ATTR (BAR));
		char *filename = (char *) u8_strconv_from_locale (g_ctx.filename);
		row_buffer_append (&buf, filename, APP_ATTR (BAR_HL));
		free (filename);
	}

	struct str right = str_make ();
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
	str_free (&right);

	int64_t end_addr = g_ctx.data_offset + g_ctx.data_len;
	if (g_ctx.view_cursor < g_ctx.data_offset
	 || g_ctx.view_cursor >= end_addr)
		return;

	int64_t len = end_addr - g_ctx.view_cursor;
	uint8_t *p = g_ctx.data + (g_ctx.view_cursor - g_ctx.data_offset);

	struct row_buffer x = row_buffer_make ();
	struct row_buffer u = row_buffer_make ();
	struct row_buffer s = row_buffer_make ();

	if (len >= 1)
	{
		app_footer_field (&x, 'x', 1, "   %02x  ", p[0]);
		app_footer_field (&u, 'u', 1, " %4u  ", p[0]);
		app_footer_field (&s, 's', 1, " %4d  ", (int8_t) p[0]);
	}
	if (len >= 2)
	{
		uint16_t val = app_decode (p, 2, g_ctx.endianity);
		app_footer_field (&x, 'x', 2, "   %04x  ", val);
		app_footer_field (&u, 'u', 2, " %6u  ", val);
		app_footer_field (&s, 's', 2, " %6d  ", (int16_t) val);
	}
	if (len >= 4)
	{
		uint32_t val = app_decode (p, 4, g_ctx.endianity);
		app_footer_field (&x, 'x', 4, "    %08x  ", val);
		app_footer_field (&u, 'u', 4, " %11u  ", val);
		app_footer_field (&s, 's', 4, " %11d  ", (int32_t) val);
	}
	if (len >= 8)
	{
		uint64_t val = app_decode (p, 8, g_ctx.endianity);
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
	app_draw_info ();
	app_draw_footer ();

	int64_t diff = g_ctx.view_cursor - g_ctx.view_top;
	int64_t y = diff / ROW_SIZE;
	int64_t x = diff % ROW_SIZE;
	if (diff >= 0 && y < app_visible_rows ())
	{
		curs_set (1);
		move (y, 10 + x*2 + g_ctx.view_skip_nibble + x/8 + x/2);
	}
	else
		curs_set (0);

	refresh ();
}

// --- Lua ---------------------------------------------------------------------

#ifdef HAVE_LUA

static void *
app_lua_alloc (void *ud, void *ptr, size_t o_size, size_t n_size)
{
	(void) ud;
	(void) o_size;

	if (n_size)
		return realloc (ptr, n_size);

	free (ptr);
	return NULL;
}

static int
app_lua_panic (lua_State *L)
{
	// XXX: we might be able to do something better
	print_fatal ("Lua panicked: %s", lua_tostring (L, -1));
	lua_close (L);
	exit (EXIT_FAILURE);
	return 0;
}

static bool
app_lua_getfield (lua_State *L, int idx, const char *name,
	int expected, bool optional)
{
	int found = lua_getfield (L, idx, name);
	if (found == expected)
		return true;
	if (optional && found == LUA_TNIL)
		return false;

	const char *message = optional
		? "invalid field \"%s\" (found: %s, expected: %s or nil)"
		: "invalid or missing field \"%s\" (found: %s, expected: %s)";
	return luaL_error (L, message, name,
		lua_typename (L, found), lua_typename (L, expected));
}

static int
app_lua_error_handler (lua_State *L)
{
	luaL_traceback (L, L, luaL_checkstring (L, 1), 1);
	return 1;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

struct app_lua_coder
{
	int ref_detect;                     ///< Reference to the "detect" method
	int ref_decode;                     ///< Reference to the "decode" method
};

static void
app_lua_coder_free (void *coder)
{
	struct app_lua_coder *self = coder;
	luaL_unref (g_ctx.L, LUA_REGISTRYINDEX, self->ref_decode);
	luaL_unref (g_ctx.L, LUA_REGISTRYINDEX, self->ref_detect);
	free (self);
}

static int
app_lua_register (lua_State *L)
{
	luaL_checktype (L, 1, LUA_TTABLE);

	(void) app_lua_getfield (L, 1, "type",   LUA_TSTRING,   false);
	const char *type = lua_tostring (L, -1);
	if (str_map_find (&g_ctx.coders, type))
		luaL_error (L, "a coder has already been registered for `%s'", type);

	(void) app_lua_getfield (L, 1, "detect", LUA_TFUNCTION, true);
	(void) app_lua_getfield (L, 1, "decode", LUA_TFUNCTION, false);

	struct app_lua_coder *coder = xcalloc (1, sizeof *coder);
	coder->ref_decode = luaL_ref (L, LUA_REGISTRYINDEX);
	coder->ref_detect = luaL_ref (L, LUA_REGISTRYINDEX);
	str_map_set (&g_ctx.coders, type, coder);
	return 0;
}

static luaL_Reg app_lua_library[] =
{
	{ "register", app_lua_register },
	{ NULL,       NULL             }
};

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

#define XLUA_CHUNK_METATABLE "chunk"

struct app_lua_chunk
{
	int64_t offset;                     ///< Offset from start of file
	int64_t len;                        ///< Length of the sequence

	int64_t position;                   ///< Read position in the sequence
	enum endianity endianity;           ///< Read endianity
};

static struct app_lua_chunk *
app_lua_chunk_new (lua_State *L)
{
	struct app_lua_chunk *chunk = lua_newuserdata (L, sizeof *chunk);
	luaL_setmetatable (L, XLUA_CHUNK_METATABLE);
	memset (chunk, 0, sizeof *chunk);
	return chunk;
}

static int
app_lua_chunk_len (lua_State *L)
{
	struct app_lua_chunk *self = luaL_checkudata (L, 1, XLUA_CHUNK_METATABLE);
	lua_pushinteger (L, self->len);
	return 1;
}

/// Create a new subchunk following Lua's string.sub() semantics.
/// An implication is that it is not possible to go extend a chunk's bounds.
static int
app_lua_chunk_call (lua_State *L)
{
	struct app_lua_chunk *self = luaL_checkudata (L, 1, XLUA_CHUNK_METATABLE);
	lua_Integer start = luaL_optinteger (L, 2,  1);
	lua_Integer end   = luaL_optinteger (L, 3, -1);

	if (start < 0) start += self->len + 1;
	if (end   < 0) end   += self->len + 1;

	start = MAX (start, 1);
	end = MIN (end, self->len);

	struct app_lua_chunk *clone = app_lua_chunk_new (L);
	clone->position = 0;
	clone->endianity = self->endianity;
	if (start > end)
	{
		// "start" can be too high and "end" can be too low;
		// the length is zero, so the offset doesn't matter much anyway
		clone->offset = self->offset;
		clone->len = 0;
	}
	else
	{
		clone->offset = self->offset + start - 1;
		clone->len = end - start + 1;
	}
	return 1;
}

static int
app_lua_chunk_index (lua_State *L)
{
	struct app_lua_chunk *self = luaL_checkudata (L, 1, XLUA_CHUNK_METATABLE);
	const char *key = luaL_checkstring (L, 2);
	if (luaL_getmetafield (L, 1, key))
		return 1;

	if (!strcmp (key, "offset"))
		lua_pushinteger (L, self->offset);
	else if (!strcmp (key, "endianity"))
		lua_pushstring (L, self->endianity == ENDIANITY_LE ? "le" : "be");
	else if (!strcmp (key, "position"))
		lua_pushinteger (L, self->position + 1);
	else if (!strcmp (key, "eof"))
		lua_pushboolean (L, self->position >= self->len);
	else
		return luaL_argerror (L, 2, "not a readable property");
	return 1;
}

static int
app_lua_chunk_newindex (lua_State *L)
{
	struct app_lua_chunk *self = luaL_checkudata (L, 1, XLUA_CHUNK_METATABLE);
	const char *key = luaL_checkstring (L, 2);
	if (!strcmp (key, "endianity"))
	{
		// Needs to be in the enum order
		const char *options[] = { "le", "be" };
		self->endianity = luaL_checkoption (L, 3, options[0], options);
	}
	else if (!strcmp (key, "position"))
	{
		lua_Integer position = luaL_checkinteger (L, 3);
		if (position < 1 || position > self->len + 1)
			return luaL_error (L, "position out of range: %I", position);
		self->position = position - 1;
	}
	else
		return luaL_argerror (L, 2, "not a writable property");
	return 0;
}

static void
app_lua_mark (int64_t offset, int64_t len, const char *desc)
{
	// That would cause stupid entries, making trouble in marks_by_offset
	if (len <= 0)
		return;

	ARRAY_RESERVE (g_ctx.marks, 1);
	g_ctx.marks[g_ctx.marks_len++] =
		(struct mark) { offset, len, g_ctx.mark_strings.len };

	str_append (&g_ctx.mark_strings, desc);
	str_append_c (&g_ctx.mark_strings, 0);
}

static int
app_lua_chunk_mark (lua_State *L)
{
	struct app_lua_chunk *self = luaL_checkudata (L, 1, XLUA_CHUNK_METATABLE);
	int n_args = lua_gettop (L);
	lua_rawgeti (L, LUA_REGISTRYINDEX, g_ctx.ref_format);
	lua_insert (L, 2);
	lua_call (L, n_args - 1, 1);
	app_lua_mark (self->offset, self->len, luaL_checkstring (L, -1));
	return 0;
}

/// Try to detect any registered type in the data and return its name
static int
app_lua_chunk_identify (lua_State *L)
{
	(void) luaL_checkudata (L, 1, XLUA_CHUNK_METATABLE);

	struct str_map_iter iter = str_map_iter_make (&g_ctx.coders);
	struct app_lua_coder *coder;
	while ((coder = str_map_iter_next (&iter)))
	{
		if (coder->ref_detect == LUA_REFNIL)
			continue;

		lua_rawgeti (L, LUA_REGISTRYINDEX, coder->ref_detect);

		// Clone the chunk first to reset its read position
		lua_pushcfunction (L, app_lua_chunk_call);
		lua_pushvalue (L, 1);
		lua_call (L, 1, 1);

		lua_call (L, 1, 1);
		if (lua_toboolean (L, -1))
		{
			lua_pushstring (L, iter.link->key);
			return 1;
		}
		lua_pop (L, 1);
	}
	return 0;
}

static int
app_lua_chunk_decode (lua_State *L)
{
	(void) luaL_checkudata (L, 1, XLUA_CHUNK_METATABLE);
	const char *type = luaL_optstring (L, 2, NULL);
	// TODO: further arguments should be passed to the decoding function

	if (!type)
	{
		lua_pushcfunction (L, app_lua_chunk_identify);
		lua_pushvalue (L, 1);
		lua_call (L, 1, 1);
		type = lua_tostring (L, -1);
	}
	// Can't identify -> can't decode, nothing to do here
	if (!type)
		return 0;

	// While we could call "detect" here, just to be sure, some kinds may not
	// even be detectable and it's better to leave it up to the plugin

	struct app_lua_coder *coder = str_map_find (&g_ctx.coders, type);
	if (!coder)
		return luaL_error (L, "unknown type: %s", type);

	lua_rawgeti (L, LUA_REGISTRYINDEX, coder->ref_decode);
	lua_pushvalue (L, 1);
	// TODO: the chunk could remember the name of the coder and prepend it
	//   to all marks set from the callback; then reset it back to NULL
	lua_call (L, 1, 0);
	return 0;
}

static int
app_lua_chunk_read (lua_State *L)
{
	struct app_lua_chunk *self = luaL_checkudata (L, 1, XLUA_CHUNK_METATABLE);
	lua_Integer len = luaL_checkinteger (L, 2);
	if (len < 0)
		return luaL_argerror (L, 2, "invalid read length");

	int64_t start = self->offset + self->position;
	// XXX: or just return a shorter string in this case?
	if (start + len > g_ctx.data_offset + g_ctx.data_len)
		return luaL_argerror (L, 2, "chunk is too short");

	lua_pushlstring (L, (char *) g_ctx.data + (start - g_ctx.data_offset), len);
	self->position += len;
	return 1;
}

/// Mark a field that has just been read from the chunk and advance position:
///  - the second argument, if present, is a simple format string for marking;
///  - the third argument, if present, is a filtering function.
///
/// I am aware of how ugly the implicit "string.format" is.  Convenience wins.
static void
app_lua_chunk_finish_read
	(lua_State *L, struct app_lua_chunk *self, int64_t len)
{
	int n_args = lua_gettop (L) - 1;
	if (n_args < 2)
	{
		self->position += len;
		return;
	}

	// Prepare <string.format>, <format>, <value>
	lua_rawgeti (L, LUA_REGISTRYINDEX, g_ctx.ref_format);
	lua_pushvalue (L, 2);

	int pre_filter_top = lua_gettop (L);
	lua_pushvalue (L, -3);

	// Transform the value if a filtering function is provided
	if (n_args >= 3)
	{
		lua_pushvalue (L, 3);
		lua_insert (L, -2);
		lua_call (L, 1, LUA_MULTRET);
		int n_ret = lua_gettop (L) - pre_filter_top;

		// When no value has been returned, keep the old one
		if (n_ret < 1)
			lua_pushvalue (L, pre_filter_top - 2);

		// Forward multiple return values to "string.format"
		if (n_ret > 1)
		{
			lua_pushvalue (L, pre_filter_top - 1);
			lua_insert (L, -n_ret - 1);
			lua_call (L, n_ret, 1);
		}
	}

	lua_call (L, 2, 1);
	app_lua_mark (self->offset + self->position, len, lua_tostring (L, -1));
	self->position += len;
	lua_pop (L, 1);
}

static int
app_lua_chunk_cstring (lua_State *L)
{
	struct app_lua_chunk *self = luaL_checkudata (L, 1, XLUA_CHUNK_METATABLE);
	void *s = g_ctx.data + (self->offset - g_ctx.data_offset) + self->position;

	void *nil;
	if (!(nil = memchr (s, '\0', self->len - self->position)))
		return luaL_error (L, "unexpected EOF");

	lua_pushlstring (L, s, nil - s);
	app_lua_chunk_finish_read (L, self, nil - s + 1);
	return 1;
}

/// Decode "len" bytes as a number starting at the current position in "self"
static uint64_t
app_lua_chunk_decode_int (lua_State *L, struct app_lua_chunk *self, size_t len)
{
	if (self->position + (int64_t) len > self->len)
		return luaL_error (L, "unexpected EOF");

	void *s = g_ctx.data + (self->offset - g_ctx.data_offset) + self->position;
	return app_decode (s, len, self->endianity);
}

#define APP_LUA_CHUNK_INT(name, type)                                          \
	static int                                                                 \
	app_lua_chunk_ ## name (lua_State *L)                                      \
	{                                                                          \
		struct app_lua_chunk *self =                                           \
			luaL_checkudata (L, 1, XLUA_CHUNK_METATABLE);                      \
		type v = app_lua_chunk_decode_int (L, self, sizeof v);                 \
		lua_pushinteger (L, v);                                                \
		app_lua_chunk_finish_read (L, self, sizeof v);                         \
		return 1;                                                              \
	}

APP_LUA_CHUNK_INT (u8,  uint8_t)  APP_LUA_CHUNK_INT (s8,  int8_t)
APP_LUA_CHUNK_INT (u16, uint16_t) APP_LUA_CHUNK_INT (s16, int16_t)
APP_LUA_CHUNK_INT (u32, uint32_t) APP_LUA_CHUNK_INT (s32, int32_t)
APP_LUA_CHUNK_INT (u64, uint64_t) APP_LUA_CHUNK_INT (s64, int64_t)

static luaL_Reg app_lua_chunk_table[] =
{
	{ "__len",      app_lua_chunk_len      },
	{ "__call",     app_lua_chunk_call     },
	{ "__index",    app_lua_chunk_index    },
	{ "__newindex", app_lua_chunk_newindex },
	{ "mark",       app_lua_chunk_mark     },
	{ "identify",   app_lua_chunk_identify },
	{ "decode",     app_lua_chunk_decode   },

	{ "read",       app_lua_chunk_read     },
	{ "cstring",    app_lua_chunk_cstring  },
	{ "u8",         app_lua_chunk_u8       },
	{ "s8",         app_lua_chunk_s8       },
	{ "u16",        app_lua_chunk_u16      },
	{ "s16",        app_lua_chunk_s16      },
	{ "u32",        app_lua_chunk_u32      },
	{ "s32",        app_lua_chunk_s32      },
	{ "u64",        app_lua_chunk_u64      },
	{ "s64",        app_lua_chunk_s64      },
	{ NULL,         NULL                   }
};

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
app_lua_load_plugins (const char *plugin_dir)
{
	DIR *dir;
	if (!(dir = opendir (plugin_dir)))
	{
		if (errno != ENOENT)
			print_error ("cannot open directory `%s': %s",
				plugin_dir, strerror (errno));
		return;
	}

	struct dirent *iter;
	while ((errno = 0, iter = readdir (dir)))
	{
		const char *dot = strrchr (iter->d_name, '.');
		if (!dot || strcmp (dot, ".lua"))
			continue;

		char *path = xstrdup_printf ("%s/%s", plugin_dir, iter->d_name);
		lua_pushcfunction (g_ctx.L, app_lua_error_handler);
		if (luaL_loadfile (g_ctx.L, path)
		 || lua_pcall (g_ctx.L, 0, 0, -2))
			exit_fatal ("Lua: %s", lua_tostring (g_ctx.L, -1));
		lua_pop (g_ctx.L, 1);
		free (path);
	}
	if (errno)
		exit_fatal ("readdir: %s", strerror (errno));
	closedir (dir);
}

static void
app_lua_init (void)
{
	if (!(g_ctx.L = lua_newstate (app_lua_alloc, NULL)))
		exit_fatal ("Lua initialization failed");

	g_ctx.coders = str_map_make (app_lua_coder_free);

	lua_atpanic (g_ctx.L, app_lua_panic);
	luaL_openlibs (g_ctx.L);
	luaL_checkversion (g_ctx.L);

	// I don't want to reimplement this and the C function is not exported
	hard_assert (lua_getglobal (g_ctx.L, LUA_STRLIBNAME));
	hard_assert (lua_getfield (g_ctx.L, -1, "format"));
	g_ctx.ref_format = luaL_ref (g_ctx.L, LUA_REGISTRYINDEX);

	luaL_newlib (g_ctx.L, app_lua_library);
	lua_setglobal (g_ctx.L, PROGRAM_NAME);

	luaL_newmetatable (g_ctx.L, XLUA_CHUNK_METATABLE);
	luaL_setfuncs (g_ctx.L, app_lua_chunk_table, 0);
	lua_pop (g_ctx.L, 1);

	struct strv v = strv_make ();
	get_xdg_data_dirs (&v);
	for (size_t i = 0; i < v.len; i++)
	{
		char *path = xstrdup_printf
			("%s/%s", v.vector[i], PROGRAM_NAME "/plugins");
		app_lua_load_plugins (path);
		free (path);
	}
	strv_free (&v);
}

#endif // HAVE_LUA

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

static bool
app_jump_to_marks (ssize_t i)
{
	if (i < 0 || (size_t) i >= g_ctx.marks_by_offset_len)
		return false;

	g_ctx.view_cursor = g_ctx.marks_by_offset[i].offset;
	g_ctx.view_skip_nibble = false;
	app_invalidate ();
	app_ensure_selection_visible ();
	return true;
}

// --- User input handling -----------------------------------------------------

enum action
{
	ACTION_NONE, ACTION_QUIT, ACTION_REDRAW, ACTION_TOGGLE_ENDIANITY,

	ACTION_SCROLL_UP,   ACTION_GOTO_TOP,    ACTION_GOTO_PAGE_PREVIOUS,
	ACTION_SCROLL_DOWN, ACTION_GOTO_BOTTOM, ACTION_GOTO_PAGE_NEXT,

	ACTION_UP, ACTION_DOWN, ACTION_LEFT, ACTION_RIGHT,
	ACTION_ROW_START, ACTION_ROW_END,
	ACTION_FIELD_PREVIOUS, ACTION_FIELD_NEXT,

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

	case ACTION_ROW_START:
	{
		int64_t new =  g_ctx.view_cursor / ROW_SIZE      * ROW_SIZE;
		new = MAX (new, g_ctx.data_offset);
		new = MIN (new, g_ctx.data_offset + g_ctx.data_len - 1);

		g_ctx.view_cursor = new;
		g_ctx.view_skip_nibble = false;
		app_invalidate ();
		break;
	}
	case ACTION_ROW_END:
	{
		int64_t new = (g_ctx.view_cursor / ROW_SIZE + 1) * ROW_SIZE - 1;
		new = MAX (new, g_ctx.data_offset);
		new = MIN (new, g_ctx.data_offset + g_ctx.data_len - 1);

		g_ctx.view_cursor = new;
		g_ctx.view_skip_nibble = false;
		app_invalidate ();
		break;
	}

	case ACTION_FIELD_PREVIOUS:
	{
		ssize_t i = app_find_marks (g_ctx.view_cursor);
		if (i >= 0 && (size_t) i < g_ctx.marks_by_offset_len
		 && g_ctx.marks_by_offset[i].offset == g_ctx.view_cursor)
			i--;
		return app_jump_to_marks (i);
	}
	case ACTION_FIELD_NEXT:
		return app_jump_to_marks (app_find_marks (g_ctx.view_cursor) + 1);

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
		// TODO: when holding a mouse button over a mark string,
		//   go to a locked mode that highlights that entire mark
		//   (probably by inverting colors)

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

	{ "Home",       ACTION_ROW_START,          {}},
	{ "End",        ACTION_ROW_END,            {}},
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

	{ "b",          ACTION_FIELD_PREVIOUS,     {}},
	{ "w",          ACTION_FIELD_NEXT,         {}},

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
		g_winch_received = false;
		update_curses_terminal_size ();
		app_fix_view_range ();
		app_invalidate ();
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

	struct str message = str_make ();
	str_append (&message, quote);
	str_append_vprintf (&message, fmt, ap);

	// If the standard error output isn't redirected, try our best at showing
	// the message to the user
	if (!isatty (STDERR_FILENO))
		fprintf (stderr, "%s\n", message.str);
	else
	{
		cstr_set (&g_ctx.message, xstrdup (message.str));
		g_ctx.message_attr = (intptr_t) user_data;
		app_invalidate ();
	}
	str_free (&message);

	in_processing = false;
}

static void
app_init_poller_events (void)
{
	g_ctx.signal_event = poller_fd_make (&g_ctx.poller, g_signal_pipe[0]);
	g_ctx.signal_event.dispatcher = app_on_signal_pipe_readable;
	poller_fd_set (&g_ctx.signal_event, POLLIN);

	g_ctx.tty_event = poller_fd_make (&g_ctx.poller, STDIN_FILENO);
	g_ctx.tty_event.dispatcher = app_on_tty_readable;
	poller_fd_set (&g_ctx.tty_event, POLLIN);

	g_ctx.tk_timer = poller_timer_make (&g_ctx.poller);
	g_ctx.tk_timer.dispatcher = app_on_key_timer;

	g_ctx.refresh_event = poller_idle_make (&g_ctx.poller);
	g_ctx.refresh_event.dispatcher = app_on_refresh;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

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

		{ 'o', "offset", "OFFSET", 0, "offset within the file" },
		{ 's', "size", "SIZE", 0, "size limit (1G by default)" },
#ifdef HAVE_LUA
		{ 't', "type", "TYPE", 0, "force interpretation as the given type" },
#endif // HAVE_LUA
		{ 0, NULL, NULL, 0, NULL }
	};

	struct opt_handler oh =
		opt_handler_make (argc, argv, opts, "[FILE]", "Hex viewer.");
	int64_t size_limit = 1 << 30;
	const char *forced_type = NULL;

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
	case 't':
		forced_type = optarg;
		break;
	default:
		print_error ("wrong options");
		opt_handler_usage (&oh, stderr);
		exit (EXIT_FAILURE);
	}

	argc -= optind;
	argv += optind;

#ifdef HAVE_LUA
	// We do it at this questionable location to catch plugin failure before
	// we read potentially hundreds of megabytes of data in
	app_lua_init ();

	if (forced_type && !strcmp (forced_type, "list"))
	{
		struct str_map_iter iter = str_map_iter_make (&g_ctx.coders);
		while (str_map_iter_next (&iter))
			puts (iter.link->key);
		exit (EXIT_SUCCESS);
	}
#endif // HAVE_LUA

	// When no filename is given, read from stdin and replace it with the tty
	int input_fd;
	if (argc == 0)
	{
		if ((input_fd = dup (STDIN_FILENO)) < 0)
			exit_fatal ("cannot read input: %s", strerror (errno));
		close (STDIN_FILENO);
		if (open ("/dev/tty", O_RDWR) != STDIN_FILENO)
			exit_fatal ("cannot open the terminal: %s", strerror (errno));
	}
	else if (argc == 1)
	{
		g_ctx.filename = xstrdup (argv[0]);
		if ((input_fd = open (argv[0], O_RDONLY)) < 0)
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
	struct str buf = str_make ();
	while (buf.len < (size_t) size_limit)
	{
		str_reserve (&buf, 8192);
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
#ifdef HAVE_LUA
	// TODO: eventually we should do this in a separate thread after load
	//   as it may take a long time (-> responsivity) and once we allow the user
	//   to edit the file, each change will need a background rescan
	lua_pushcfunction (g_ctx.L, app_lua_error_handler);
	lua_pushcfunction (g_ctx.L, app_lua_chunk_decode);

	struct app_lua_chunk *chunk = app_lua_chunk_new (g_ctx.L);
	chunk->offset = g_ctx.data_offset;
	chunk->len = g_ctx.data_len;

	if (forced_type)
		lua_pushstring (g_ctx.L, forced_type);
	else
		lua_pushnil (g_ctx.L);
	if (lua_pcall (g_ctx.L, 2, 0, -4))
		exit_fatal ("Lua: decoding failed: %s", lua_tostring (g_ctx.L, -1));

	lua_pop (g_ctx.L, 1);
#endif // HAVE_LUA
	app_flatten_marks ();

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

#ifdef HAVE_LUA
	str_map_free (&g_ctx.coders);
	lua_close (g_ctx.L);
#endif // HAVE_LUA

	return 0;
}
