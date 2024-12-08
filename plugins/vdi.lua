--
-- vdi.lua: VirtualBox Disk Image
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
	if #c < 68 then
		return false
	end
	c.position = 65
	return c:read (4) == "\x7F\x10\xDA\xBE"
end

-- As described by https://forums.virtualbox.org/viewtopic.php?t=8046
local decode = function (c)
	if not detect (c ()) then error ("not a VDI file") end
	c.endianity = "le"

	-- The header is preceded by an arbitrary string
	c.position = 65
	local signature = c:u32 ("image signature")

	local p, vmajor, vminor = c.position, c:u16 (), c:u16 ()
	c (p, c.position - 1):mark ("VDI version: %d.%d", vmajor, vminor)

	local size = c:u32 ("size of header: %d")
	c (64 + 1, 64 + size):mark ("VDI header")

	local type = c:u32 ("image type: %s", function (u32)
		if u32 == 1 then return "dynamic" end
		if u32 == 2 then return "static" end
		return "unknown: %d", u32
	end)
	local flags = c:u32 ("image flags: %#x")

	local p, desc = c.position, c:read (256)
	c (p, c.position - 1):mark ("image description: %s", desc:match ("%C+"))

	local offset_blocks = c:u32 ("offset to blocks: %#x")
	local offset_data = c:u32 ("offset to data: %#x")
	local n_cylinders = c:u32 ("#cylinders: %d")
	local n_heads = c:u32 ("#heads: %d")
	local n_sectors = c:u32 ("#sectors: %d")
	local sector_size = c:u32 ("sector size: %d")
	local unused = c:read (4)
	local disk_size = c:u64 ("disk size: %d bytes")
	local block_size = c:u32 ("block size: %d")
	-- TODO: we should probably count that in -> is it in "blocks" or "data"?
	local block_extra_data = c:u32 ("block extra data: %d")
	local n_blocks = c:u32 ("#blocks in HDD: %d")
	local n_blocks_allocated = c:u32 ("#blocks allocated: %d")

	local function read_uuid4 (c, name)
		local p, uuid = c.position, c:read (16)
		local b = table.pack (uuid:byte (1, #uuid))
		c (p, c.position - 1):mark ("%s: %02x%02x%02x%02x-"
			.. "%02x%02x-%02x%02x-%02x%02x-"
			.. "%02x%02x%02x%02x%02x%02x", name,
			b[1], b[2], b[3], b[4],
			b[5], b[6], b[7], b[8], b[9], b[10],
			b[11], b[12], b[13], b[14], b[15], b[16])
		return uuid
	end

	local uuid_vdi = read_uuid4 (c, "UUID of VDI")
	local uuid_last_snap = read_uuid4 (c, "UUID of last snapshot")
	local uuid_link = read_uuid4 (c, "UUID of link")
	local uuid_parent = read_uuid4 (c, "UUID of parent")

	-- TODO: perhaps this should be more granular and identify all blocks
	c (offset_blocks + 1, offset_blocks + 4 * n_blocks)
		:mark ("VDI blocks")
	c (offset_data + 1, offset_data + block_size * n_blocks_allocated)
		:mark ("VDI data")
end

hex.register { type="vdi", detect=detect, decode=decode }

