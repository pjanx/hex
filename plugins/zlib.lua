--
-- zlib.lua: ZLIB Compressed Data Format
--
-- Copyright (c) 2017, Přemysl Janouch <p.janouch@gmail.com>
--
-- Permission to use, copy, modify, and/or distribute this software for any
-- purpose with or without fee is hereby granted, provided that the above
-- copyright notice and this permission notice appear in all copies.
--
-- THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
-- WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
-- MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
-- SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
-- WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
-- OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
-- CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
--

-- Based on RFC 1950, this format isn't very widely used
local decode = function (c)
	c.endianity = 'le'

	local deflate_levels = { "fastest", "fast", "default", "slowest" }
	local deflate
	local cmf = c:u8 ("compression method and flags: %s", function (u8)
		local cm,    cm_name    = u8 & 0xf, "unknown"
		local cinfo, cinfo_name = u8 >> 4,  "unknown"
		if cm == 8 then
			deflate = true
			cm_name = "deflate"
			cinfo_name = ("window size = %d"):format (2 ^ (cinfo + 8))
		end
		return "%s (%d), %s (%d)", cm_name, cm, cinfo_name, cinfo
	end)
	local have_dict
	local flags = c:u8 ("flags: %s", function (u8)
		local check = (cmf * 256 + u8) % 31 == 0
		if check then check = "ok" else check = "failed" end

		local level, level_name = (u8 >> 6) & 3, "unknown"
		if deflate then level_name = deflate_levels[level + 1] end
		have_dict = (u8 >> 5) & 1 == 1

		local info = "preset dictionary"
		if not have_dict then info = "no " .. info end
		return "level %s (%d), check %s, %s", level_name, level, check, info
	end)
	if have_dict then
		c:u16 ("dictionary Adler-32 checksum: %04x")
	end

	-- Compressed data follows immediately
	-- Not going to decompress it, so we don't know where the final checksum is
end

hex.register { type="zlib", detect=nil, decode=decode }
