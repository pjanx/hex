/*
 * hex -- hex viewer
 *
 * Copyright (c) 2016 - 2024, Přemysl Eric Janouch <p@janouch.name>
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
// FIXME: we need a 256color default palette that fails gracefully
//   to something like underlined fields
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

#ifdef WITH_X11
#define LIBERTY_XUI_WANT_X11
#endif // WITH_X11
#include "liberty/liberty-xui.c"

#include <locale.h>

#ifdef WITH_LUA
#include <dirent.h>

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

// This test is too annoying to do in CMake due to CheckTypeSize() being unable
// to take link_directories(), and the Lua constant is documented.
#if LUA_MAXINTEGER < INT64_MAX
#error Lua must have at least 64-bit integers
#endif
#endif // WITH_LUA

#define APP_TITLE  PROGRAM_NAME         ///< Left top corner

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
	bool polling;                       ///< The event loop is running

	struct poller_fd signal_event;      ///< Signal FD event

#ifdef WITH_LUA
	lua_State *L;                       ///< Lua state
	int ref_format;                     ///< Reference to "string.format"
	struct str_map coders;              ///< Map of coders by name
#endif // WITH_LUA

	// Data:

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

	// User interface:

	struct poller_timer message_timer;  ///< Message timeout
	char *message;                      ///< Last logged message

	int digitw;                         ///< Width of a single digit

	struct attrs attrs[ATTRIBUTE_COUNT];
}
g;

/// Shortcut to retrieve named terminal attributes
#define APP_ATTR(name) g.attrs[ATTRIBUTE_ ## name].attrs

// --- Configuration -----------------------------------------------------------

static const struct config_schema g_config_colors[] =
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
		g.attrs[ATTRIBUTE_ ## name] = attrs_decode (value);
	ATTRIBUTE_TABLE (XX)
#undef XX
}

static void
app_load_configuration (void)
{
	struct config *config = &g.config;
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
		config_load (&g.config, root);
		config_schema_call_changed (g.config.root);
	}
}

// --- Application -------------------------------------------------------------

static void
app_init_attributes (void)
{
#define XX(name, config, fg_, bg_, attrs_)          \
	g.attrs[ATTRIBUTE_ ## name].fg    = fg_;    \
	g.attrs[ATTRIBUTE_ ## name].bg    = bg_;    \
	g.attrs[ATTRIBUTE_ ## name].attrs = attrs_;
	ATTRIBUTE_TABLE (XX)
#undef XX
}

static bool
app_on_insufficient_color (void)
{
	app_init_attributes ();
	return true;
}

static void
app_init_context (void)
{
	poller_init (&g.poller);
	g.config = config_make ();

	ARRAY_INIT (g.marks);
	g.mark_strings = str_make ();
	ARRAY_INIT (g.marks_by_offset);
	ARRAY_INIT (g.offset_entries);

	app_init_attributes ();
}

static void
app_free_context (void)
{
	config_free (&g.config);
	poller_free (&g.poller);

	free (g.marks);
	str_free (&g.mark_strings);
	free (g.marks_by_offset);
	free (g.offset_entries);

	cstr_set (&g.message, NULL);

	cstr_set (&g.filename, NULL);
	free (g.data);
}

static void
app_quit (void)
{
	g.polling = false;
}

// --- Field marking -----------------------------------------------------------

/// Find the "marks_by_offset" span covering the offset (if any)
static ssize_t
app_find_marks (int64_t offset)
{
	ssize_t min = 0, end = g.marks_by_offset_len;
	while (min < end)
	{
		ssize_t mid = min + (end - min) / 2;
		if (offset >= g.marks_by_offset[mid].offset)
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
	if (i < 0 || (size_t) i >= g.marks_by_offset_len)
		return NULL;

	struct marks_by_offset *marks = &g.marks_by_offset[i];
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
	size_t result = g.offset_entries_len;
	ARRAY_RESERVE (g.offset_entries, len);
	memcpy (g.offset_entries + g.offset_entries_len, entries,
		sizeof *entries * len);
	g.offset_entries_len += len;
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
	qsort (g.marks, g.marks_len, sizeof *g.marks, app_mark_cmp);
	if (!g.marks_len)
		return;

	ARRAY (struct mark *, current)
	ARRAY_INIT (current);
	int current_color = 0;

	// Make offset zero actually point to an empty entry
	g.offset_entries[g.offset_entries_len++] = NULL;

	struct mark *next = g.marks;
	struct mark *end = next + g.marks_len;
	while (current_len || next < end)
	{
		// Find the closest offset at which marks change
		int64_t closest = g.data_offset + g.data_len;
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

		ARRAY_RESERVE (g.marks_by_offset, 1);
		g.marks_by_offset[g.marks_by_offset_len++] =
			(struct marks_by_offset) { closest, marks, color };
	}
	free (current);
}

// --- Layouting ---------------------------------------------------------------

enum
{
	WIDGET_NONE = 0, WIDGET_HEX, WIDGET_ASCII, WIDGET_ENDIANITY,
};

struct layout
{
	struct widget *head;
	struct widget *tail;
};

static struct widget *
app_label (chtype attrs, const char *label)
{
	return g_xui.ui->label (attrs, 0, label);
}

static struct widget *
app_mono_label (chtype attrs, const char *label)
{
	return g_xui.ui->label (attrs, XUI_ATTR_MONOSPACE, label);
}

static struct widget *
app_mono_padding (chtype attrs, float width, float height)
{
	struct widget *w = g_xui.ui->padding (attrs, width, height);
	w->width = width * g.digitw;
	return w;
}

static struct widget *
app_push (struct layout *l, struct widget *w)
{
	LIST_APPEND_WITH_TAIL (l->head, l->tail, w);
	return w;
}

static struct widget *
app_push_hfill (struct layout *l, struct widget *w)
{
	w->width = -1;
	return app_push (l, w);
}

static struct widget *
app_push_vfill (struct layout *l, struct widget *w)
{
	w->height = -1;
	return app_push (l, w);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static int
app_visible_rows (void)
{
	int occupied = 1 /* bar */ + 3 /* decoder */;
	return MAX (0, g_xui.height - occupied * g_xui.vunit) / g_xui.vunit;
}

static inline void
app_layout_cell (struct layout *hex, struct layout *ascii, int attrs,
	int64_t addr)
{
	const char *hexa = "0123456789abcdef";

	struct marks_by_offset *marks = app_marks_at_offset (addr);
	int attrs_mark = attrs;
	if (marks && marks->color >= 0)
		attrs_mark = g.attrs[marks->color].attrs;

	if (addr >= g.view_cursor
	 && addr <  g.view_cursor + 8)
	{
		attrs      |= A_UNDERLINE;
		attrs_mark |= A_UNDERLINE;
	}

	// TODO: leave it up to the user to decide what should be colored
	uint8_t cell = g.data[addr - g.data_offset];
	if (addr != g.view_cursor)
	{
		char s[] = { hexa[cell >> 4], hexa[cell & 0xf], 0 };
		app_push (hex, app_mono_label (attrs, s));
	}
	else if (g.view_skip_nibble)
	{
		char s1[] = { hexa[cell >> 4], 0 }, s2[] = { hexa[cell & 0xf], 0 };
		app_push (hex, app_mono_label (attrs, s1));
		app_push (hex, app_mono_label (attrs ^ A_REVERSE, s2));
	}
	else
	{
		char s1[] = { hexa[cell >> 4], 0 }, s2[] = { hexa[cell & 0xf], 0 };
		app_push (hex, app_mono_label (attrs ^ A_REVERSE, s1));
		app_push (hex, app_mono_label (attrs, s2));
	}

	char s[] = { (cell >= 32 && cell < 127) ? cell : '.', 0 };
	app_push (ascii, app_mono_label (attrs_mark, s));
}

// XXX: This per-character layouting is very inefficient, but not extremely so.
static struct widget *
app_layout_row (int64_t addr, int y, int attrs)
{
	struct layout l = {};
	char *row_addr_str = xstrdup_printf ("%08" PRIx64, addr);
	app_push (&l, app_mono_label (attrs, row_addr_str));
	free (row_addr_str);

	struct layout hex = {};
	struct layout ascii = {};
	app_push (&ascii, app_mono_padding (attrs, 2, 1));

	int64_t end_addr = g.data_offset + g.data_len;
	for (int x = 0; x < ROW_SIZE; x++)
	{
		if (x % 8 == 0) app_push (&hex, app_mono_padding (attrs, 1, 1));
		if (x % 2 == 0) app_push (&hex, app_mono_padding (attrs, 1, 1));

		int64_t cell_addr = addr + x;
		if (cell_addr < g.data_offset
		 || cell_addr >= end_addr)
		{
			app_push (&hex,   app_mono_padding (attrs, 2, 1));
			app_push (&ascii, app_mono_padding (attrs, 1, 1));
		}
		else
			app_layout_cell (&hex, &ascii, attrs, cell_addr);
	}

	struct widget *w = NULL;
	app_push (&l, (w = xui_hbox (hex.head)))->id = WIDGET_HEX;
	w->userdata = y;
	app_push (&l, (w = xui_hbox (ascii.head)))->id = WIDGET_ASCII;
	w->userdata = y;
	return xui_hbox (l.head);
}

static struct widget *
app_layout_view (void)
{
	struct layout l = {};
	int64_t end_addr = g.data_offset + g.data_len;
	for (int y = 0; y <= app_visible_rows (); y++)
	{
		int64_t addr = g.view_top + y * ROW_SIZE;
		if (addr >= end_addr)
			break;

		int attrs = (addr / ROW_SIZE & 1) ? APP_ATTR (ODD) : APP_ATTR (EVEN);
		app_push (&l, app_layout_row (addr, y, attrs));
	}
	return xui_vbox (l.head);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static struct widget *
app_layout_info (void)
{
	const struct marks_by_offset *marks;
	struct layout l = {};
	if (!(marks = app_marks_at_offset (g.view_cursor)))
		goto out;

	struct mark *mark, **iter = g.offset_entries + marks->marks;
	for (int y = 0; y <= app_visible_rows (); y++)
	{
		// TODO: we can use the field background
		// TODO: we can keep going through subsequent fields to fill the column
		if (!(mark = *iter++))
			break;

		const char *description = g.mark_strings.str + mark->description;
		app_push (&l, app_label (0, description));
	}
out:
	return xui_vbox (l.head);
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

static struct widget *
app_footer_field (char id, int size, const char *fmt, ...)
	ATTRIBUTE_PRINTF (3, 4);

static struct widget *
app_footer_field (char id, int size, const char *fmt, ...)
{
	const char *coding = "";
	if (size <= 1)
		;
	else if (g.endianity == ENDIANITY_LE)
		coding = "le";
	else if (g.endianity == ENDIANITY_BE)
		coding = "be";

	struct layout l = {};
	char *key = xstrdup_printf ("%c%d%s", id, size * 8, coding);
	app_push (&l, app_mono_label (APP_ATTR (FOOTER_HL), key));
	free (key);

	app_push (&l, app_mono_padding (0, 1, 1));

	va_list ap;
	va_start (ap, fmt);
	struct str value = str_make ();
	str_append_vprintf (&value, fmt, ap);
	va_end (ap);

	// Right-aligned
	app_push_hfill (&l, g_xui.ui->padding (0, 1, 1));
	app_push (&l, app_mono_label (APP_ATTR (FOOTER), value.str));
	str_free (&value);
	return xui_hbox (l.head);
}

static void
app_footer_group (struct layout *out, int size, int64_t u, int64_t s, int align)
{
	struct layout l = {};
	app_push (&l, app_footer_field ('x', size, "%0*" PRIx64, size * 2, u));
	app_push (&l, app_footer_field ('u', size, "%" PRIu64, u));
	app_push (&l, app_footer_field ('s', size, "%" PRId64, s));
	l.head->width = MAX (l.head->width,
		g.digitw * align /* sign + ceil(log10(U/INT*_MAX)) */);
	if (out->head)
		app_push (out, app_mono_padding (APP_ATTR (FOOTER), 2, 1));
	app_push (out, xui_vbox (l.head));
}

static struct widget *
app_layout_footer (void)
{
	struct layout statusl = {};
	app_push (&statusl, app_label (APP_ATTR (BAR), APP_TITLE));
	app_push (&statusl, g_xui.ui->padding (APP_ATTR (BAR), 1, 1));

	if (g.message)
		app_push (&statusl, app_label (APP_ATTR (BAR_HL), g.message));
	else if (g.filename)
	{
		char *filename = (char *) u8_strconv_from_locale (g.filename);
		app_push (&statusl, app_label (APP_ATTR (BAR_HL), filename));
		free (filename);
		app_push (&statusl, g_xui.ui->padding (APP_ATTR (BAR), 1, 1));
	}

	app_push_hfill (&statusl, g_xui.ui->padding (APP_ATTR (BAR), 1, 1));

	char *address = xstrdup_printf ("%08" PRIx64, g.view_cursor);
	app_push (&statusl, app_mono_label (APP_ATTR (BAR), address));
	free (address);
	app_push (&statusl, g_xui.ui->padding (APP_ATTR (BAR), 1, 1));

	app_push (&statusl, app_mono_label (APP_ATTR (BAR),
		g.endianity == ENDIANITY_LE ? "LE" : "BE"))->id = WIDGET_ENDIANITY;
	app_push (&statusl, g_xui.ui->padding (APP_ATTR (BAR), 1, 1));

	int64_t top = g.view_top;
	int64_t bot = g.view_top + app_visible_rows () * ROW_SIZE;
	struct str where = str_make ();
	if (top <= g.data_offset
	 && bot >= g.data_offset + g.data_len)
		str_append (&where, "All");
	else if (top <= g.data_offset)
		str_append (&where, "Top");
	else if (bot >= g.data_offset + g.data_len)
		str_append (&where, "Bot");
	else
	{
		int64_t end_addr = g.data_offset + g.data_len;
		int64_t cur = g.view_top / ROW_SIZE;
		int64_t max = (end_addr - 1) / ROW_SIZE - app_visible_rows () + 1;

		cur -= g.data_offset / ROW_SIZE;
		max -= g.data_offset / ROW_SIZE;
		str_append_printf (&where, "%2d%%", (int) (100 * cur / max));
	}

	app_push (&statusl, app_mono_label (APP_ATTR (BAR), where.str));
	str_free (&where);

	int64_t end_addr = g.data_offset + g.data_len;
	if (g.view_cursor < g.data_offset
	 || g.view_cursor >= end_addr)
		return xui_hbox (statusl.head);

	int64_t len = end_addr - g.view_cursor;
	uint8_t *p = g.data + (g.view_cursor - g.data_offset);

	// TODO: The entire bottom part perhaps should be pre-painted
	//   with APP_ATTR (FOOTER).
	struct layout groupl = {};
	if (len >= 1)
		app_footer_group (&groupl, 1, p[0], (int8_t) p[0], 3 + 4);
	if (len >= 2)
	{
		uint16_t value = app_decode (p, 2, g.endianity);
		app_footer_group (&groupl, 2, value, (int16_t) value, 6 + 6);
	}
	if (len >= 4)
	{
		uint32_t value = app_decode (p, 4, g.endianity);
		app_footer_group (&groupl, 4, value, (int32_t) value, 6 + 11);
	}
	if (len >= 8)
	{
		uint64_t value = app_decode (p, 8, g.endianity);
		app_footer_group (&groupl, 8, value, (int64_t) value, 6 + 20);
	}

	struct layout lll = {};
	app_push (&lll, xui_hbox (statusl.head));
	app_push (&lll, xui_hbox (groupl.head));
	return xui_vbox (lll.head);
}

static void
app_layout (void)
{
	struct layout topl = {};
	app_push (&topl, app_layout_view ());
	app_push (&topl, g_xui.ui->padding (0, 1, 1));
	app_push_hfill (&topl, app_layout_info ());

	struct layout l = {};
	app_push_vfill (&l, xui_hbox (topl.head));
	app_push (&l, app_layout_footer ());

	struct widget *root = g_xui.widgets = xui_vbox (l.head);
	root->width = g_xui.width;
	root->height = g_xui.height;
}

// --- Lua ---------------------------------------------------------------------

#ifdef WITH_LUA

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
	luaL_unref (g.L, LUA_REGISTRYINDEX, self->ref_decode);
	luaL_unref (g.L, LUA_REGISTRYINDEX, self->ref_detect);
	free (self);
}

static int
app_lua_register (lua_State *L)
{
	luaL_checktype (L, 1, LUA_TTABLE);

	(void) app_lua_getfield (L, 1, "type",   LUA_TSTRING,   false);
	const char *type = lua_tostring (L, -1);
	if (str_map_find (&g.coders, type))
		luaL_error (L, "a coder has already been registered for `%s'", type);

	(void) app_lua_getfield (L, 1, "detect", LUA_TFUNCTION, true);
	(void) app_lua_getfield (L, 1, "decode", LUA_TFUNCTION, false);

	struct app_lua_coder *coder = xcalloc (1, sizeof *coder);
	coder->ref_decode = luaL_ref (L, LUA_REGISTRYINDEX);
	coder->ref_detect = luaL_ref (L, LUA_REGISTRYINDEX);
	str_map_set (&g.coders, type, coder);
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

	ARRAY_RESERVE (g.marks, 1);
	g.marks[g.marks_len++] =
		(struct mark) { offset, len, g.mark_strings.len };

	str_append (&g.mark_strings, desc);
	str_append_c (&g.mark_strings, 0);
}

static int
app_lua_chunk_mark (lua_State *L)
{
	struct app_lua_chunk *self = luaL_checkudata (L, 1, XLUA_CHUNK_METATABLE);
	int n_args = lua_gettop (L);
	lua_rawgeti (L, LUA_REGISTRYINDEX, g.ref_format);
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

	struct str_map_iter iter = str_map_iter_make (&g.coders);
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

	struct app_lua_coder *coder = str_map_find (&g.coders, type);
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
	if (start + len > g.data_offset + g.data_len)
		return luaL_argerror (L, 2, "chunk is too short");

	lua_pushlstring (L, (char *) g.data + (start - g.data_offset), len);
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
	lua_rawgeti (L, LUA_REGISTRYINDEX, g.ref_format);
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
	void *s = g.data + (self->offset - g.data_offset) + self->position;

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

	void *s = g.data + (self->offset - g.data_offset) + self->position;
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

	lua_pushcfunction (g.L, app_lua_error_handler);

	struct dirent *iter;
	while ((errno = 0, iter = readdir (dir)))
	{
		const char *dot = strrchr (iter->d_name, '.');
		if (!dot || strcmp (dot, ".lua"))
			continue;

		char *path = xstrdup_printf ("%s/%s", plugin_dir, iter->d_name);
		if (luaL_loadfile (g.L, path)
		 || lua_pcall (g.L, 0, 0, -2))
		{
			print_error ("%s: %s", path, lua_tostring (g.L, -1));
			lua_pop (g.L, 1);
		}
		free (path);
	}
	if (errno)
		exit_fatal ("readdir: %s", strerror (errno));
	closedir (dir);

	lua_pop (g.L, 1);
}

static void
app_lua_init (void)
{
	if (!(g.L = lua_newstate (app_lua_alloc, NULL)))
		exit_fatal ("Lua initialization failed");

	g.coders = str_map_make (app_lua_coder_free);

	lua_atpanic (g.L, app_lua_panic);
	luaL_openlibs (g.L);
	luaL_checkversion (g.L);

	// I don't want to reimplement this and the C function is not exported
	hard_assert (lua_getglobal (g.L, LUA_STRLIBNAME));
	hard_assert (lua_getfield (g.L, -1, "format"));
	g.ref_format = luaL_ref (g.L, LUA_REGISTRYINDEX);

	luaL_newlib (g.L, app_lua_library);
	lua_setglobal (g.L, PROGRAM_NAME);

	luaL_newmetatable (g.L, XLUA_CHUNK_METATABLE);
	luaL_setfuncs (g.L, app_lua_chunk_table, 0);
	lua_pop (g.L, 1);

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

#endif // WITH_LUA

// --- Actions -----------------------------------------------------------------

/// Checks what items are visible and returns if fixes were needed
static bool
app_fix_view_range (void)
{
	int64_t data_view_start = g.data_offset / ROW_SIZE * ROW_SIZE;
	if (g.view_top < data_view_start)
	{
		g.view_top = data_view_start;
		xui_invalidate ();
		return false;
	}

	// If the contents are at least as long as the screen, always fill it
	int64_t last_byte = g.data_offset + g.data_len - 1;
	int64_t max_view_top =
		(last_byte / ROW_SIZE - app_visible_rows () + 1) * ROW_SIZE;
	// But don't let that suggest a negative offset
	max_view_top = MAX (max_view_top, 0);

	if (g.view_top > max_view_top)
	{
		g.view_top = max_view_top;
		xui_invalidate ();
		return false;
	}
	return true;
}

/// Scroll down (positive) or up (negative) @a n items
static bool
app_scroll (int n)
{
	g.view_top += n * ROW_SIZE;
	xui_invalidate ();
	return app_fix_view_range ();
}

static void
app_ensure_selection_visible (void)
{
	int too_high = g.view_top / ROW_SIZE - g.view_cursor / ROW_SIZE;
	if (too_high > 0)
		app_scroll (-too_high);

	int too_low = g.view_cursor / ROW_SIZE - g.view_top / ROW_SIZE
		- app_visible_rows () + 1;
	if (too_low > 0)
		app_scroll (too_low);
}

static bool
app_move_cursor_by_rows (int diff)
{
	// TODO: disallow partial up/down movement
	int64_t fixed = g.view_cursor += diff * ROW_SIZE;
	fixed = MAX (fixed, g.data_offset);
	fixed = MIN (fixed, g.data_offset + g.data_len - 1);

	bool result = g.view_cursor == fixed;
	g.view_cursor = fixed;
	xui_invalidate ();

	app_ensure_selection_visible ();
	return result;
}

static bool
app_jump_to_marks (ssize_t i)
{
	if (i < 0 || (size_t) i >= g.marks_by_offset_len)
		return false;

	g.view_cursor = g.marks_by_offset[i].offset;
	g.view_skip_nibble = false;
	xui_invalidate ();
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
		g.view_cursor = g.data_offset;
		g.view_skip_nibble = false;
		app_ensure_selection_visible ();
		xui_invalidate ();
		break;
	case ACTION_GOTO_BOTTOM:
		if (!g.data_len)
			return false;

		g.view_cursor = g.data_offset + g.data_len - 1;
		g.view_skip_nibble = false;
		app_ensure_selection_visible ();
		xui_invalidate ();
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
		if (g.view_skip_nibble)
			g.view_skip_nibble = false;
		else
		{
			if (g.view_cursor <= g.data_offset)
				return false;

			g.view_skip_nibble = true;
			g.view_cursor--;
			app_ensure_selection_visible ();
		}
		xui_invalidate ();
		break;
	case ACTION_RIGHT:
		if (!g.view_skip_nibble)
			g.view_skip_nibble = true;
		else
		{
			if (g.view_cursor >= g.data_offset + g.data_len - 1)
				return false;

			g.view_skip_nibble = false;
			g.view_cursor++;
			app_ensure_selection_visible ();
		}
		xui_invalidate ();
		break;

	case ACTION_ROW_START:
	{
		int64_t new =  g.view_cursor / ROW_SIZE      * ROW_SIZE;
		new = MAX (new, g.data_offset);
		new = MIN (new, g.data_offset + g.data_len - 1);

		g.view_cursor = new;
		g.view_skip_nibble = false;
		xui_invalidate ();
		break;
	}
	case ACTION_ROW_END:
	{
		int64_t new = (g.view_cursor / ROW_SIZE + 1) * ROW_SIZE - 1;
		new = MAX (new, g.data_offset);
		new = MIN (new, g.data_offset + g.data_len - 1);

		g.view_cursor = new;
		g.view_skip_nibble = false;
		xui_invalidate ();
		break;
	}

	case ACTION_FIELD_PREVIOUS:
	{
		ssize_t i = app_find_marks (g.view_cursor);
		if (i >= 0 && (size_t) i < g.marks_by_offset_len
		 && g.marks_by_offset[i].offset == g.view_cursor)
			i--;
		return app_jump_to_marks (i);
	}
	case ACTION_FIELD_NEXT:
		return app_jump_to_marks (app_find_marks (g.view_cursor) + 1);

	case ACTION_QUIT:
		app_quit ();
	case ACTION_NONE:
		break;
	case ACTION_REDRAW:
		clear ();
		xui_invalidate ();
		break;

	case ACTION_TOGGLE_ENDIANITY:
		g.endianity = (g.endianity == ENDIANITY_LE)
			? ENDIANITY_BE : ENDIANITY_LE;
		xui_invalidate ();
		break;
	default:
		return false;
	}
	return true;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static bool
app_process_left_mouse_click (struct widget *w, int x, int y)
{
	if (w->id == WIDGET_ENDIANITY)
		return app_process_action (ACTION_TOGGLE_ENDIANITY);

	// XXX: This is really ugly.
	x = x / g.digitw - 2;
	y = w->userdata;
	switch (w->id)
	{
	case WIDGET_HEX:
		x -= x/5 + x/21;
		g.view_skip_nibble = x % 2;
		x /= 2;
		break;
	case WIDGET_ASCII:
		g.view_skip_nibble = false;
		break;
	default:
		return true;
	}

	g.view_cursor = g.view_top + y * ROW_SIZE + x;
	return app_move_cursor_by_rows (0);
}

/// Returns the deepest child at the cursor that has a non-zero ID, if any.
static struct widget *
app_find_widget (struct widget *list, int x, int y)
{
	struct widget *target = NULL;
	LIST_FOR_EACH (struct widget, w, list)
	{
		if (x < w->x || x >= w->x + w->width
		 || y < w->y || y >= w->y + w->height)
			continue;

		struct widget *child = app_find_widget (w->children, x, y);
		if (child)
			target = child;
		else if (w->id)
			target = w;
	}
	return target;
}

static bool
app_process_mouse (termo_mouse_event_t type, int x, int y, int button,
	int modifiers)
{
	(void) modifiers;

	// TODO: when holding a mouse button over a mark string,
	//   go to a locked mode that highlights that entire mark
	//   (probably by inverting colors)
	if (type != TERMO_MOUSE_PRESS)
		return true;
	if (button == 4)
		return app_process_action (ACTION_SCROLL_UP);
	if (button == 5)
		return app_process_action (ACTION_SCROLL_DOWN);

	struct widget *target = app_find_widget (g_xui.widgets, x, y);
	if (!target)
		return false;

	x -= target->x;
	y -= target->y;
	if (button == 1)
		return app_process_left_mouse_click (target, x, y);
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
	{ "C-a",        ACTION_ROW_START,          {}},
	{ "C-e",        ACTION_ROW_END,            {}},
	{ "_",          ACTION_ROW_START,          {}},
	{ "$",          ACTION_ROW_END,            {}},
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
	return termo_keycmp (g_xui.tk,
		&((struct binding *) a)->decoded, &((struct binding *) b)->decoded);
}

static void
app_init_bindings (void)
{
	for (size_t i = 0; i < N_ELEMENTS (g_default_bindings); i++)
	{
		struct binding *binding = &g_default_bindings[i];
		hard_assert (!*termo_strpkey_utf8 (g_xui.tk,
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
	return event->type == TERMO_TYPE_FOCUS;
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
app_on_signal_pipe_readable (const struct pollfd *fd, void *user_data)
{
	(void) user_data;

	char id = 0;
	(void) read (fd->fd, &id, 1);

	if (g_termination_requested)
		app_quit ();

	if (g_winch_received)
	{
		g_winch_received = false;
		if (g_xui.ui->winch)
			g_xui.ui->winch ();
		app_fix_view_range ();
	}
}

static void
app_show_message (char *message)
{
	cstr_set (&g.message, message);
	poller_timer_set (&g.message_timer, 5000);
	xui_invalidate ();
}

static void
app_hide_message (void)
{
	if (!g.message)
		return;

	cstr_set (&g.message, NULL);
	poller_timer_reset (&g.message_timer);
	xui_invalidate ();
}

static void
app_on_message_timer (void *user_data)
{
	(void) user_data;

	app_hide_message ();
}

static void
app_log_handler (void *user_data, const char *quote, const char *fmt,
	va_list ap)
{
	(void) user_data;

	// We certainly don't want to end up in a possibly infinite recursion
	static bool in_processing;
	if (in_processing)
		return;

	in_processing = true;

	struct str message = str_make ();
	str_append (&message, quote);
	str_append_vprintf (&message, fmt, ap);

	app_show_message (xstrdup (message.str));

	// If the standard error output isn't redirected, try our best at showing
	// the message to the user
	if (g_debug_mode && !isatty (STDERR_FILENO))
		fprintf (stderr, "%s\n", message.str);
	str_free (&message);

	in_processing = false;
}

static void
app_on_clipboard_copy (const char *text)
{
	// TODO: Resolve encoding.
	print_status ("Text copied to clipboard: %s", text);
}

static void
app_init_poller_events (void)
{
	g.signal_event = poller_fd_make (&g.poller, g_signal_pipe[0]);
	g.signal_event.dispatcher = app_on_signal_pipe_readable;
	poller_fd_set (&g.signal_event, POLLIN);

	g.message_timer = poller_timer_make (&g.poller);
	g.message_timer.dispatcher = app_on_message_timer;
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
#ifdef WITH_X11
		{ 'x', "x11", NULL, 0, "use X11 even when run from a terminal" },
#endif  // WITH_X11
		{ 'h', "help", NULL, 0, "display this help and exit" },
		{ 'V', "version", NULL, 0, "output version information and exit" },

		{ 'o', "offset", "OFFSET", 0, "offset within the file" },
		{ 's', "size", "SIZE", 0, "size limit (1G by default)" },
#ifdef WITH_LUA
		{ 't', "type", "TYPE", 0, "force interpretation as the given type" },
#endif // WITH_LUA
		{ 0, NULL, NULL, 0, NULL }
	};

	bool requested_x11 = false;
	struct opt_handler oh = opt_handler_make (argc, argv, opts, "[FILE]",
		"Interpreting hex viewer.");
	int64_t size_limit = 1 << 30;
	const char *forced_type = NULL;

	int c;
	while ((c = opt_handler_get (&oh)) != -1)
	switch (c)
	{
	case 'd':
		g_debug_mode = true;
		break;
	case 'x':
		requested_x11 = true;
		break;
	case 'h':
		opt_handler_usage (&oh, stdout);
		exit (EXIT_SUCCESS);
	case 'V':
		printf (PROGRAM_NAME " " PROGRAM_VERSION "\n");
		exit (EXIT_SUCCESS);

	case 'o':
		if (!decode_size (optarg, &g.data_offset))
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

#ifdef WITH_LUA
	// We do it at this questionable location to catch plugin failure before
	// we read potentially hundreds of megabytes of data in
	app_lua_init ();

	if (forced_type && !strcmp (forced_type, "list"))
	{
		struct str_map_iter iter = str_map_iter_make (&g.coders);
		while (str_map_iter_next (&iter))
			puts (iter.link->key);
		exit (EXIT_SUCCESS);
	}
#endif // WITH_LUA

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
		g.filename = xstrdup (argv[0]);
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
	if (lseek (input_fd, g.data_offset, SEEK_SET) == (off_t) -1)
		for (uint64_t remaining = g.data_offset; remaining; )
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

	g.data = (uint8_t *) buf.str;
	g.data_len = buf.len;

	g.view_top = g.data_offset / ROW_SIZE * ROW_SIZE;
	g.view_cursor = g.data_offset;

	// We only need to convert to and from the terminal encoding
	if (!setlocale (LC_CTYPE, ""))
		print_warning ("failed to set the locale");

	app_init_context ();
#ifdef WITH_LUA
	// TODO: eventually we should do this in a separate thread after load
	//   as it may take a long time (-> responsivity) and once we allow the user
	//   to edit the file, each change will need a background rescan
	lua_pushcfunction (g.L, app_lua_error_handler);
	lua_pushcfunction (g.L, app_lua_chunk_decode);

	struct app_lua_chunk *chunk = app_lua_chunk_new (g.L);
	chunk->offset = g.data_offset;
	chunk->len = g.data_len;

	if (forced_type)
		lua_pushstring (g.L, forced_type);
	else
		lua_pushnil (g.L);
	if (lua_pcall (g.L, 2, 0, -4))
		exit_fatal ("Lua: decoding failed: %s", lua_tostring (g.L, -1));

	lua_pop (g.L, 1);
#endif // WITH_LUA
	app_flatten_marks ();

	app_load_configuration ();
	signals_setup_handlers ();
	app_init_poller_events ();

	xui_preinit ();
	app_init_bindings ();
	xui_start (&g.poller,
		requested_x11, g.attrs, N_ELEMENTS (g.attrs));
	xui_invalidate ();

	struct widget *w = g_xui.ui->label (0, XUI_ATTR_MONOSPACE, "8");
	g.digitw = w->width;
	widget_destroy (w);

	// Redirect all messages from liberty so that they don't disrupt display
	g_log_message_real = app_log_handler;

	g.polling = true;
	while (g.polling)
		poller_run (&g.poller);

	xui_stop ();
	g_log_message_real = log_message_stdio;
	app_free_context ();

#ifdef WITH_LUA
	str_map_free (&g.coders);
	lua_close (g.L);
#endif // WITH_LUA

	return 0;
}
