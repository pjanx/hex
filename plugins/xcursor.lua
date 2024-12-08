--
-- xcursor.lua: X11 cursors
--
-- Copyright (c) 2021, Přemysl Eric Janouch <p@janouch.name>
--
-- Permission to use, copy, modify, and/or distribute this software for any
-- purpose with or without fee is hereby granted.
--
-- THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
-- WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
-- MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
-- SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
-- WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
-- OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
-- CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
--

local detect = function (c)
	return #c >= 4 and c:read (4) == "Xcur"
end

-- https://www.x.org/releases/current/doc/man/man3/Xcursor.3.xhtml
local decode = function (c)
	if not detect (c) then error ("not an Xcursor file") end

	-- libXcursor shows that they are always little-endian
	c (1, 4):mark ("Xcursor magic")
	c.endianity = "le"

	local size = c:u32 ("header size: %d")
	-- TODO: check the version, although it is essentially set in stone
	--   as 1.0, with X.org being a nearly abandoned project
	local version = c:u32 ("file version: %s", function (u32)
		return "%d.%d", u32 >> 16, u32 & 0xffff
	end)
	local ntoc = c:u32 ("number of ToC entries: %d")

	local i
	for i = 1, ntoc do
		local start = c.position
		local type = c:u32 ("entry type: %s", function (u32)
			if u32 == 0xfffe0001 then return "comment" end
			if u32 == 0xfffd0002 then return "image" end
			return "unknown: %d", u32
		end)
		local subtype = c:u32 ("entry subtype: %d")
		local position = c:u32 ("chunk position: %d")
		c (start, c.position - 1):mark (("ToC entry %d"):format (i))
	end

	-- TODO: decode all entries as well
end

hex.register { type="xcursor", detect=detect, decode=decode }
