--
-- zip.lua: ZIP archives
--
-- Copyright (c) 2017, Přemysl Eric Janouch <p@janouch.name>
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

-- Heuristics required, see https://en.wikipedia.org/wiki/Zip_(file_format)
local detect = function (c)
	c.endianity = "le"
	for o = #c - 22 + 1, #c - 65535 - 22 + 1, -1 do
		if o < 1 then break end
		c.position = o
		if c:u32 () == 0x06054b50 then return o end
	end
end

local decode = function (c)
	local eocd = detect (c ())
	if not eocd then error ("not a ZIP file") end
	c.endianity = "le"

	c.position = eocd
	c:u32 ("end of CD magic: %#x")
	c:u16 ("# of this disk: %d")
	c:u16 ("disk # where CD starts: %d")
	local cd_len = c:u16 ("# of CD records on this disk: %d")
	c:u16 ("total # of CD records: %d")
	c:u32 ("size of CD: %d")
	local cd_offset = c:u32 ("offset of (start of CD - start of archive): %d")
	local comment_len = c:u16 ("comment length: %d")
	c (c.position, c.position + comment_len - 1):mark ("comment")

	-- TODO: decode the fields better
	--   https://stackoverflow.com/a/30028491/76313
	-- TODO: also mark actual file data if someone wants to put in the effort
	c.position = cd_offset + 1
	for i = 1, cd_len do
		local p, magic = c.position, c:u32 ()
		if magic ~= 0x02014b50 then break end
		c (p, c.position - 1):mark ("CD file header magic: %#x", magic)
		c:u16 ("version made by: %d")
		c:u16 ("version needed to extract: %d")
		c:u16 ("general purpose bit flag: %#x")
		c:u16 ("compression method: %d")
		c:u16 ("file last modification time: %d")
		c:u16 ("file last modification date: %d")
		c:u32 ("CRC-32: %#x")
		c:u32 ("compressed size: %d")
		c:u32 ("uncompressed size: %d")
		local filename_len = c:u16 ("file name length: %d")
		local extra_len = c:u16 ("extra field length: %d")
		local comment_len = c:u16 ("file comment length: %d")
		c:u16 ("disk # where file starts: %d")
		c:u16 ("internal file attributes: %#x")
		c:u32 ("external file attributes: %#x")
		c:u32 ("offset of (start of local file header - start of archive): %d")

		c (c.position, c.position + filename_len - 1):mark ("filename")
		c.position = c.position + filename_len

		c (c.position, c.position + extra_len - 1):mark ("extra field")
		c.position = c.position + extra_len

		c (c.position, c.position + comment_len - 1):mark ("file comment")
		c.position = c.position + comment_len
	end
end

hex.register { type="zip", detect=detect, decode=decode }

