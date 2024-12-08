--
-- pdf.lua: Portable Document Format
--
-- Based on PDF Reference, version 1.7
-- In practice almost useless, I just wanted to learn about the file format.
-- FIXME: it's also not very robust and doesn't support all documents.
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

local oct_alphabet = "01234567"
local dec_alphabet = "0123456789"
local hex_alphabet = "0123456789abcdefABCDEF"
local whitespace = "\x00\t\n\f\r "
local delimiters = "()<>[]{}/%"

local strchr = function (s, ch) return s:find (ch, 1, true) end

-- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

local Lexer = {}
Lexer.__index = Lexer

function Lexer:new (c)
	return setmetatable ({ c = c }, self)
end

-- TODO: make it possible to follow a string, we should probably be able to
--   supply callbacks to the constructor, or a wrapper object;
--   this will be used for object streams
function Lexer:getc ()
	if self.c.eof then return nil end
	return self.c:read (1)
end

function Lexer:ungetc ()
	self.c.position = self.c.position - 1
end

function Lexer:token (type, value, description)
	if description then
		self.c (self.start, self.c.position - 1):mark (description)
	end
	return { type=type, value=value,
		start=self.start, stop=self.c.position - 1 }
end

-- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

function Lexer:eat_newline (ch)
	if ch == '\r' then
		ch = self:getc ()
		if ch and ch ~= '\n' then self:ungetc () end
		return true
	elseif ch == '\n' then
		return true
	end
end

function Lexer:string ()
	local value, level, ch = "", 1
::continue::
	while true do
		ch = self:getc ()
		if not ch then return nil
		elseif ch == '\\' then
			ch = self:getc ()
			if not ch then return nil
			elseif ch == 'n' then ch = '\n'
			elseif ch == 'r' then ch = '\r'
			elseif ch == 't' then ch = '\t'
			elseif ch == 'b' then ch = '\b'
			elseif ch == 'f' then ch = '\f'
			elseif self:eat_newline (ch) then goto continue
			elseif strchr (oct_alphabet, ch) then
				local buf, i = ch
				for i = 1, 2 do
					ch = self:getc ()
					if not ch then return nil
					elseif not strchr (oct_alphabet, ch) then
						self:ungetc ()
						break
					end
					buf = buf .. ch
				end
				ch = string.char (tonumber (buf, 8))
			end
		elseif self:eat_newline (ch) then
			ch = '\n'
		elseif ch == '(' then
			level = level + 1
		elseif ch == ')' then
			level = level - 1
			if level == 0 then break end
		end
		value = value .. ch
	end
	return self:token ('string', value, "string literal")
end

function Lexer:string_hex ()
	local value, buf, ch = ""
	while true do
		ch = self:getc ()
		if not ch then return nil
		elseif ch == '>' then
			break
		elseif not strchr (hex_alphabet, ch) then
			return nil
		elseif buf then
			value = value .. string.char (tonumber (buf .. ch, 16))
			buf = nil
		else
			buf = ch
		end
	end
	if buf then value = value .. string.char (tonumber (buf .. '0', 16)) end
	return self:token ('string', value, "string hex")
end

function Lexer:name ()
	local value, ch = ""
	while true do
		ch = self:getc ()
		if not ch then break
		elseif ch == '#' then
			local ch1, ch2 = self:getc (), self:getc ()
			if not ch1 or not ch2
			or not strchr (hex_alphabet, ch1)
			or not strchr (hex_alphabet, ch2) then
				return nil
			end
			ch = string.char (tonumber (ch1 .. ch2, 16))
		elseif strchr (whitespace .. delimiters, ch) then
			self:ungetc ()
			break
		end
		value = value .. ch
	end
	if value == "" then return nil end
	return self:token ('name', value, "name")
end

function Lexer:comment ()
	local value, ch = ""
	while true do
		ch = self:getc ()
		if not ch then break
		elseif ch == '\r' or ch == '\n' then
			self:ungetc ()
			break
		end
		value = value .. ch
	end
	return self:token ('comment', value, "comment")
end

function Lexer:number (ch)
	local value, real, digits = "", false, false
	if ch == '-' then
		value = ch
		ch = self:getc ()
	end
	while ch do
		if strchr (dec_alphabet, ch) then
			digits = true
		elseif ch == '.' and not real then
			real = true
		else
			self:ungetc ()
			break
		end
		value = value .. ch
		ch = self:getc ()
	end
	-- XXX: perhaps we should instead let it be interpreted as a keyword
	if not digits then return nil end
	-- XXX: maybe we should differentiate between integers and real values
	return self:token ('number', tonumber (value, 10), "number")
end

function Lexer:get_token ()
::restart::
	self.start = self.c.position
	local ch = self:getc ()

	if not ch then return nil
	elseif ch == '(' then return self:string ()
	elseif ch == '[' then return self:token ('begin_array')
	elseif ch == ']' then return self:token ('end_array')
	elseif ch == '<' then
		-- It seems they ran out of paired characters, yet {} is unused
		ch = self:getc ()
		if not ch then return nil
		elseif ch == '<' then return self:token ('begin_dictionary')
		else
			self:ungetc ()
			return self:string_hex ()
		end
	elseif ch == '>' then
		ch = self:getc ()
		if not ch then return nil
		elseif ch == '>' then return self:token ('end_dictionary')
		else return nil end
	elseif ch == '/' then return self:name ()
	elseif ch == '%' then return self:comment ()
	elseif strchr ("-0123456789.", ch) then return self:number (ch)
	elseif self:eat_newline       (ch) then return self:token ('newline')
	elseif strchr (whitespace,     ch) then goto restart
	else
		-- {} end up being keywords but we should probably error out
		local value = ch
		while true do
			ch = self:getc ()
			if not ch then break
			elseif strchr (whitespace .. delimiters, ch) then
				self:ungetc ()
				break
			end
			value = value .. ch
		end
		if     value == "null" then
			return self:token ('null',    nil,   "null")
		elseif value == "true" then
			return self:token ('boolean', true,  "boolean")
		elseif value == "false" then
			return self:token ('boolean', false, "boolean")
		end
		return self:token ('keyword', value, "keyword")
	end
end

-- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

local is_value = function (t)
	return t == 'null' or t == 'boolean' or t == 'name'
		or t == 'number' or t == 'string'
end

-- Retrieve the next thing in the stream, possibly popping values from the stack
local function get_object (lex, stack, deref)
::restart::
	local token = lex:get_token ()
	if token == nil then return nil
	elseif token.type == 'begin_array' then
		local array = {}
		repeat
			local object = get_object (lex, array, deref)
			if not object then error ("array doesn't end") end
			table.insert (array, object)
		until object.type == 'end_array'
		local stop = table.remove (array)
		return { type='array', value=array, start=token.start, stop=stop.stop }
	elseif token.type == 'begin_dictionary' then
		local dict = {}
		repeat
			local object = get_object (lex, dict, deref)
			if not object then error ("dictionary doesn't end") end
			table.insert (dict, object)
		until object.type == 'end_dictionary'
		local stop, kv = table.remove (dict), {}
		if #dict % 2 == 1 then error ("unbalanced dictionary") end
		for i = 1, #dict, 2 do
			local k, v = dict[i], dict[i + 1]
			if k.type ~= 'name' then error ("invalid dictionary key type") end
			kv[k.value] = v
		end
		return { type='dict', value=kv, start=token.start, stop=stop.stop }
	elseif token.type == 'keyword' and token.value == 'stream' then
		if #stack < 1 then error ("no dictionary for stream") end
		local d = table.remove (stack)
		if d.type ~= 'dict' then error ("stream not preceded by dictionary") end

		if not lex:eat_newline (lex:getc ()) then
			error ("'stream' not followed by newline")
		end

		local len = deref (d.value['Length'])
		if not len or len.type ~= 'number' then
			error ("missing stream length")
		end

		local data, stop = lex.c:read (len.value), get_object (lex, {}, deref)
		if not stop or stop.type ~= 'keyword' or stop.value ~= 'endstream' then
			error ("missing 'endstream'")
		end

		return { type='stream', value={ dict=dict, data=data },
			start=token.start, stop=stop.stop }
	elseif token.type == 'keyword' and token.value == 'obj' then
		if #stack < 2 then error ("missing object ID pair") end
		local gen, n = table.remove (stack), table.remove (stack)
		if n.type ~= 'number' or gen.type ~= 'number' then
			error ("object ID pair must be two integers")
		end

		local tmp = {}
		repeat
			local object = get_object (lex, tmp, deref)
			if not object then error ("object doesn't end") end
			table.insert (tmp, object)
		until object.type == 'keyword' and object.value == 'endobj'
		local stop = table.remove (tmp)

		if #tmp ~= 1 then error ("objects must contain exactly one value") end
		local value = table.remove (tmp)
		return { type='object', n=n.value, gen=gen.value, value=value,
			start=n.start, stop=stop.stop }
	elseif token.type == 'keyword' and token.value == 'R' then
		if #stack < 2 then error ("missing reference ID pair") end
		local gen, n = table.remove (stack), table.remove (stack)
		if n.type ~= 'number' or gen.type ~= 'number' then
			error ("reference ID pair must be two integers")
		end
		return { type='reference', value={ n.value, gen.value } }
	elseif token.type == 'newline' or token.type == 'comment' then
		-- These are not objects and our callers aren't interested
		goto restart
	else
		return token
	end
end

-- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

local detect = function (c)
	return #c >= 5 and c:read (5) == "%PDF-"
end

local decode_xref_subsection = function (lex, start, count, result)
	if not lex:eat_newline (lex:getc ()) then
		error ("xref subsection must start on a new line")
	end
	for i = 0, count - 1 do
		local entry = lex.c:read (20)
		local off, gen, typ = entry:match
			("^(%d%d%d%d%d%d%d%d%d%d) (%d%d%d%d%d) ([fn])[\r ][\r\n]$")
		if not off then error ("invalid xref entry") end

		-- Translated to the extended XRefStm format
		result[start + i] = {
			t = typ == 'n' and 1 or 0,
			o = math.tointeger (off),
			g = math.tointeger (gen),
		}
	end
end

-- A deref that can't actually resolve anything, for early stages of processing
local deref_nil = function (x)
	if not x or x.type == 'reference' then return nil end
	return x
end

-- Creates a table with named indexes from the trailer and items indexed by
-- object numbers containing { XRefStm fields... }
local decode_xref_normal = function (lex)
	local result = {}
	while true do
		local a = get_object (lex, {}, deref_nil)
		local b = get_object (lex, {}, deref_nil)
		if not a or not b then
			error ("xref section ends too soon")
		elseif a.type == 'number' and b.type == 'number' then
			decode_xref_subsection (lex, a.value, b.value, result)
		elseif a.type == 'keyword' and a.value == 'trailer'
		and b.type == 'dict' then
			for k, v in pairs (b.value) do
				result[k] = v
			end
			return result
		else
			error ("invalid xref contents")
		end
	end
end

local decode_xref_stream = function (lex, stream)
	if stream.dict['Type'] ~= 'XRef' then error ("expected an XRef stream") end

	-- TODO: decode a cross-reference stream from stream.{dict,data};
	--   the compression filter, if present, is always going to be FlateDecode,
	--   which we'll have to import or implement
	-- TODO: take care to also cache cross-reference streams by offset when
	--   they're actually implemented
	error ("cross-reference streams not implemented")
end

local decode_xref = function (c)
	local lex, stack = Lexer:new (c), {}
	while true do
		local object = get_object (lex, stack, deref_nil)
		if object == nil then
			return nil
		elseif object.type == 'keyword' and object.value == 'xref' then
			return decode_xref_normal (lex)
		elseif object.type == 'stream' then
			return decode_xref_stream (lex, object)
		end
		table.insert (stack, object)
	end
end

-- Return all objects found in xref tables as a table indexed by object number,
-- pointing to a list of generations and overwrites, from newest to oldest.
local read_all_xrefs = function (c, start_offset)
	local loaded, result, offset = {}, {}, start_offset
	while true do
		-- Prevent an infinite loop with malicious files
		if loaded[offset] then error ("cyclic cross-reference sections") end

		local xref = decode_xref (c (1 + offset, #c))
		if not xref then break end
		for k, v in pairs (xref) do
			if type (k) == 'number' then
				if not result[k] then result[k] = {} end
				table.insert (result[k], v)
			end
		end
		loaded[offset] = true

		-- TODO: when 'XRefStm' is found, it has precedence over this 'Prev',
		--   and also has its own 'Prev' chain
		local prev = xref['Prev']
		if not prev or prev.type ~= 'number' then break end
		offset = prev.value
	end
	return result
end

local decode = function (c)
	assert (c.position == 1)
	if not detect (c ()) then error ("not a PDF file") end

	-- Look for a pointer to the xref section within the last kibibyte
	-- NOTE: we could probably look backwards for the "trailer" line from here
	--   but we don't know how long the trailer is and we don't want to regex
	--   scan the whole file (ignoring that dictionary contents might, possibly
	--   legally, include the word "trailer" at the beginning of a new line)
	local tail_len = math.min (1024, #c)
	local tail = c (#c - tail_len, #c):read (tail_len)
	local xref_loc = tail:match (".*%sstartxref%s+(%d+)%s+%%%%EOF")
	if not xref_loc then error ("cannot find trailer") end

	-- We need to decode xref sections in order to be able to resolve indirect
	-- references to stream lengths
	local xref = read_all_xrefs (c, math.tointeger (xref_loc))
	local deref

	-- We have to make sure that we don't decode objects twice as that would
	-- duplicate all marks, so we simply cache all objects by offset.
	-- This may be quite the memory load but it seems to be the best thing.
	local cache = {}
	local read_object = function (offset)
		if cache[offset] then return cache[offset] end

		local lex, stack = Lexer:new (c (1 + offset, #c)), {}
		repeat
			local object = get_object (lex, stack, deref)
			if not object then error ("object doesn't end") end
			table.insert (stack, object)
		until object.type == 'object'

		local object = table.remove (stack)
		cache[offset] = object
		c (offset + object.start, offset + object.stop)
			:mark ("object " .. object.n .. " " .. object.gen)
		return object
	end

	-- Resolve an object -- if it's a reference, look it up in "xref",
	-- otherwise just return the object as it was passed
	deref = function (x)
		if not x or x.type ~= 'reference' then return x end
		local n, gen = x.value[1], x.value[2]

		-- TODO: we should also ignore object numbers >= trailer /Size
		local bin = xref[n]
		if not bin then return nil end
		local entry = bin[1]
		if not entry or entry.t ~= 1 or entry.g ~= gen then return nil end

		local object = read_object (entry.o)
		if not object or object.n ~= n or object.gen ~= gen then return nil end
		return object.value
	end

	-- Read all objects accessible from the current version of the document
	for n, bin in pairs (xref) do
		local entry = bin[1]
		if entry and entry.t == 1 then
			read_object (entry.o)
		end
	end

	-- TODO: we should actually try to decode even unreferenced objects.
	--   The problem with decoding content from previous versions of the
	--   document is that we must ignore xref updates from newer versions.
	--   The version information needs to be propagated everywhere.
end

hex.register { type="pdf", detect=detect, decode=decode }
