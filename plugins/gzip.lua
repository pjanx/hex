--
-- gzip.lua: GZIP File Format
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

local detect = function (c)
	return c:read (2) == "\x1f\x8b"
end

local function latin1_to_utf8 (s)
	local u = ""
	for _, c in ipairs (table.pack (s:byte (1, #s))) do
		if c < 0x80 then
			u = u .. string.char (c)
		else
			u = u .. string.char (0xc0 | c >> 6, 0x80 | c & 0x3f)
		end
	end
	return u
end

-- Everything here is based on RFC 1952 and some bits of dictzip
local crc32_table = function ()
	local table = {}
	for n = 0, 255 do
		local c = n
		for k = 0, 7 do
			if c & 1 ~= 0 then
				c = 0xedb88320 ~ (c >> 1)
			else
				c = c >> 1
			end
		end
		table[n] = c
	end
	return table
end

local crc32 = function (s)
	local table, c = crc32_table (), 0xffffffff
	for n = 1, #s do c = table[(c ~ s:byte (n)) & 0xff] ~ (c >> 8) end
	return c ~ 0xffffffff
end

local os_table = {
	[0]   = "FAT filesystem",
	[1]   = "Amiga",
	[2]   = "VMS",
	[3]   = "Unix",
	[4]   = "VM/CMS",
	[5]   = "Atari TOS",
	[6]   = "HPFS filesystem",
	[7]   = "Macintosh",
	[8]   = "Z-System",
	[9]   = "CP/M",
	[10]  = "TOPS-20",
	[11]  = "NTFS filesystem",
	[12]  = "QDOS",
	[13]  = "Acord RISCOS",
	[255] = "unknown"
}

local decode = function (c)
	if not detect (c ()) then error ("not a GZIP file") end
	local start = c.position

	c.endianity = 'le'
	c:u16 ("GZIP magic")

	local deflate
	c:u8 ("compression method: %s", function (u8)
		if u8 ~= 8 then return "unknown: %d", u8 end
		deflate = true
		return "deflate"
	end)

	local text, hcrc, extra, name, comment
	c:u8 ("flags: %s", function (u8)
		text    =  u8       & 1 == 1
		hcrc    = (u8 >> 1) & 1 == 1
		extra   = (u8 >> 2) & 1 == 1
		name    = (u8 >> 3) & 1 == 1
		comment = (u8 >> 4) & 1 == 1

		local flags = ""
		if text    then flags = flags .. ", text"       end
		if hcrc    then flags = flags .. ", header CRC" end
		if extra   then flags = flags .. ", extra"      end
		if name    then flags = flags .. ", filename"   end
		if comment then flags = flags .. ", comment"    end

		if flags == "" then
			return "none"
		else
			return "%s", flags:sub (3)
		end
	end)

	c:u32 ("modified time: %s", function (u32)
		if u32 == 0 then return "none" end
		return os.date ("!%F %T", u32)
	end)
	c:u8 ("extra flags: %s", function (u8)
		if deflate then
			if u8 == 2 then return "slowest (%d)", u8 end
			if u8 == 4 then return "fastest (%d)", u8 end
		end
		return "unknown: %d", u8
	end)
	c:u8 ("OS: %s", function (u8)
		local os = os_table[u8]
		if os then return os end
		return "unknown: %d", u8
	end)

	local extra_table = {}
	if extra then
		local len = c:u16 ("extra field length: %d")
		c (c.position, c.position + len - 1):mark ("extra field")

		-- This will handle even overflowing subfields
		while len >= 4 do
			local p, sid = c.position, c:read (2)
			c (p, c.position - 1):mark ("subfield ID: %s", sid)
			local sid_len = c:u16 ("subfield length: %d")

			local subfield = c (c.position, c.position + sid_len - 1)
			subfield:mark ("subfield data")
			extra_table[sid] = subfield
			c.position = c.position + sid_len
			len = len - 4 - sid_len
		end
		c.position = c.position + len
	end

	if name then
		c:cstring ("filename: %s", latin1_to_utf8)
	end
	if comment then
		c:cstring ("comment: %s", latin1_to_utf8)
	end
	if hcrc then
		c:u16 ("CRC-16: %s", function (u16)
			local crc = 0xffff & crc32 (c (start):read (c.position - 1))
			if crc == u16 then check = "ok" else check = "failed" end
			return "%#06x (%s)", u16, check
		end)
	end

	-- Compressed data follows immediately
	-- We can jump through it without decompression in dictzip v1 archives
	local ra = extra_table["RA"]
	if not ra then return end
	local ra_ver = ra:u16 ("RA version: %d")
	if ra_ver ~= 1 then return end

	local ra_chunk = ra:u16 ("chunk length: %d")
	local ra_count = ra:u16 ("chunk count: %d")
	for i = 1, ra_count do
		local len = ra:u16 ("chunk " .. i .. " compressed length: %d")
		c (c.position, c.position + len - 1):mark ("chunk " .. i)
		c.position = c.position + len
	end
	-- 1 final, 01 static, 0000000 end of block, padding discarded
	-- This is the kind of block that dictzip finalizes archives with
	if c:u16 () & 0x03ff == 0x0003 then
		c:u32 ("CRC-32: %#010x")
		c:u32 ("input size: %d")
	end
end

hex.register { type="gzip", detect=detect, decode=decode }
