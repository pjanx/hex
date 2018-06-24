--
-- bencode.lua: BitTorrent data encoding
--
-- Copyright (c) 2017, Přemysl Janouch <p@janouch.name>
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
	-- There is no magic to go by
	return false
end

-- We're not making it exactly readable but at least we're trying
local function one_value (c, level)
	local p, type = c.position, c:read (1)
	if type == "e" then
		c (p, c.position - 1):mark ("%d: container end", level - 1)
		return false
	elseif type == "i" then
		local n = ""
		type = c:read (1)
		while type ~= "e" do
			n = n .. type
			type = c:read (1)
		end
		c (p, c.position - 1):mark ("%d: integer: %s", level, n)
	elseif type == "l" then
		c (p, p):mark ("%d: list begin", level)
		local i = 0
		while true do
			local p = c.position
			if not one_value (c, level + 1) then break end
			c (p, c.position - 1):mark ("%d: list value %d", level, i)
			i = i + 1
		end
	elseif type == "d" then
		c (p, p):mark ("%d: dictionary begin", level)
		while true do
			local p = c.position
			if not one_value (c, level + 1) then break end
			c (p, c.position - 1):mark ("%d: dictionary key", level)

			local p = c.position
			if not one_value (c, level + 1) then break end
			c (p, c.position - 1):mark ("%d: dictionary value", level)
		end
	else
		local n = type
		type = c:read (1)
		while type ~= ":" do
			n = n .. type
			type = c:read (1)
		end
		c (p, c.position - 1):mark ("%d: string length: %s", level, n)

		p = c.position
		c:read (tonumber (n))
		c (p, c.position - 1):mark ("%d: string value", level)
	end
	return true
end

local decode = function (c)
	while not c.eof do
		one_value (c, 0)
	end
end

hex.register { type="bencode", detect=detect, decode=decode }
