--
-- pcap.lua: libpcap file format
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

local detect = function (c)
	local magic = c:u32 ()
	return magic == 0xa1b2c3d4 or magic == 0xd4c3b2a1
end

local detect_ng = function (c)
	local magic = c (9):u32 ()
	return c:u32 () == 0x0a0d0d0a
		and (magic == 0x1a2b3c4d or magic == 0x4d3c2b1a)
end

-- Specified in http://www.tcpdump.org/linktypes.html
local link_types = {
	[0]   = "NULL",
	[1]   = "ETHERNET",
	[3]   = "AX25",
	[6]   = "IEEE802_5",
	[7]   = "ARCNET_BSD",
	[8]   = "SLIP",
	[9]   = "PPP",
	[10]  = "FDDI",
	[50]  = "PPP_HDLC",
	[51]  = "PPP_ETHER",
	[100] = "ATM_RFC1483",
	[101] = "RAW",
	[104] = "C_HDLC",
	[105] = "IEEE802_11",
	[107] = "FRELAY",
	[108] = "LOOP",
	[113] = "LINUX_SLL",
	[114] = "LTALK",
	[117] = "PFLOG",
	[119] = "IEEE802_11_PRISM",
	[122] = "IP_OVER_FC",
	[123] = "SUNATM",
	[127] = "IEEE802_11_RADIOTAP",
	[129] = "ARCNET_LINUX",
	[138] = "APPLE_IP_OVER_IEEE1394",
	[139] = "MTP2_WITH_PHDR",
	[140] = "MTP2",
	[141] = "MTP3",
	[142] = "SCCP",
	[143] = "DOCSIS",
	[144] = "LINUX_IRDA",
	[147] = "USER0",
	[148] = "USER1",
	[149] = "USER2",
	[150] = "USER3",
	[151] = "USER4",
	[152] = "USER5",
	[153] = "USER6",
	[154] = "USER7",
	[155] = "USER8",
	[156] = "USER9",
	[157] = "USER10",
	[158] = "USER11",
	[159] = "USER12",
	[160] = "USER13",
	[161] = "USER14",
	[162] = "USER15",
	[163] = "IEEE802_11_AVS",
	[165] = "BACNET_MS_TP",
	[166] = "PPP_PPPD",
	[169] = "GPRS_LLC",
	[170] = "GPF_T",
	[171] = "GPF_F",
	[177] = "LINUX_LAPD",
	[187] = "BLUETOOTH_HCI_H4",
	[189] = "USB_LINUX",
	[192] = "PPI",
	[195] = "IEEE802_15_4",
	[196] = "SITA",
	[197] = "ERF",
	[201] = "BLUETOOTH_HCI_H4_WITH_PHDR",
	[202] = "AX25_KISS",
	[203] = "LAPD",
	[204] = "PPP_WITH_DIR",
	[205] = "C_HDLC_WITH_DIR",
	[206] = "FRELAY_WITH_DIR",
	[209] = "IPMB_LINUX",
	[215] = "IEEE802_15_4_NONASK_PHY",
	[220] = "USB_LINUX_MMAPPED",
	[224] = "FC_2",
	[225] = "FC_2_WITH_FRAME_DELIMS",
	[226] = "IPNET",
	[227] = "CAN_SOCKETCAN",
	[228] = "IPV4",
	[229] = "IPV6",
	[230] = "IEEE802_15_4_NOFCS",
	[231] = "DBUS",
	[235] = "DVB_CI",
	[236] = "MUX27010",
	[237] = "STANAG_5066_D_PDU",
	[239] = "NFLOG",
	[240] = "NETANALYZER",
	[241] = "NETANALYZER_TRANSPARENT",
	[242] = "IPOIB",
	[243] = "MPEG_2_TS",
	[244] = "NG40",
	[245] = "NFC_LLCP",
	[247] = "INFINIBAND",
	[248] = "SCTP",
	[249] = "USBPCAP",
	[250] = "RTAC_SERIAL",
	[251] = "BLUETOOTH_LE_LL",
	[253] = "NETLINK",
	[254] = "BLUETOOTH_LINUX_MONITOR",
	[255] = "BLUETOOTH_BREDR_BB",
	[256] = "BLUETOOTH_LE_LL_WITH_PHDR",
	[257] = "PROFIBUS_DL",
	[258] = "PKTAP",
	[259] = "EPON",
	[260] = "IPMI_HPM_2",
	[261] = "ZWAVE_R1_R2",
	[262] = "ZWAVE_R3",
	[263] = "WATTSTOPPER_DLM",
	[264] = "ISO_14443",
	[265] = "RDS",
	[266] = "USB_DARWIN"
}

-- As described by https://wiki.wireshark.org/Development/LibpcapFileFormat
local decode = function (c)
	if not detect (c ()) then error ("not a PCAP file") end

	c.endianity = "le"
	c:u32 ("PCAP magic: %s", function (u32)
		if u32 == 0xa1b2c3d4 then return "little-endian" end

		c.endianity = "be"
		return "big-endian"
	end)

	local p, vmajor, vminor = c.position, c:u16 (), c:u16 ()
	c (p, c.position - 1):mark ("PCAP version: %d.%d", vmajor, vminor)

	local zone = c:i32 ("UTC to local TZ correction: %d seconds")
	local sigfigs = c:u32 ("timestamp accuracy")
	local snaplen = c:u32 ("max. length of captured packets")

	local network = c:u32 ("data link type: %s", function (u32)
		name = link_types[u32]
		if name then return name end
		return "unknown: %d", u32
	end)

	local i = 0
	while not c.eof do
		c (c.position, c.position + 23):mark ("PCAP record %d header", i)
		i = i + 1

		local p, ts_sec, ts_usec = p, c:u32 (), c:u32 ()
		c (p, c.position - 1):mark ("timestamp: %s.%06d",
			os.date ("!%F %T", ts_sec + zonen), ts_usec)
		local incl_len = c:u32 ("included record length")
		local orig_len = c:u32 ("original record length")

		local p = c.position
		c.position = c.position + incl_len
		-- TODO: also decode record contents as per the huge table
		c (p, c.position - 1):mark ("PCAP record %d data", i)
	end
end

hex.register { type="pcap", detect=detect, decode=decode }

-- As described by https://github.com/pcapng/pcapng
local decode_ng = function (c)
	assert (c.position == 1)
	if not detect_ng (c ()) then error ("not a PCAPNG file") end

	c.endianity = "le"
	c (9):u32 ("byte-order magic: %s", function (u32)
		if u32 == 0x1a2b3c4d then return "little-endian" end

		c.endianity = "be"
		return "big-endian"
	end)

	local function decode_block_type (u32)
		if u32 == 0x0a0d0d0a then return "Section Header Block" end
		if u32 == 0x00000001 then return "Interface Description Block" end
		if u32 == 0x00000003 then return "Simple Packet Block" end
		if u32 == 0x00000004 then return "Name Resolution Block" end
		if u32 == 0x00000005 then return "Interface Statistics Block" end
		if u32 == 0x00000006 then return "Enhanced Packet Block" end

		if u32 == 0x00000BAD or u32 == 0x40000BAD then
			return "Custom Block"
		end
		return "unknown: %d", u32
	end

	local function decode_shb (c)
		local magic = c:u32 ()
		local p, vmajor, vminor = c.position, c:u16 (), c:u16 ()
		c (p, c.position - 1):mark ("PCAPNG version: %d.%d", vmajor, vminor)
		-- XXX: what exactly does section_len mean?
		local section_len = c:u64 ("section length: %d")

		while not c.eof do
			-- TODO: decode the meaning of options as well
			local type = c:u16 ("option type: %d")
			local length = c:u16 ("option length: %d")

			local p = c.position
			c.position = c.position + length + (-length & 3)
			c (p, c.position - 1):mark ("option value")
		end
	end

	while not c.eof do
		local block_start = c.position
		local block_type = c:u32 ("PCAPNG block type: %s", decode_block_type)
		local block_len = c:u32 ("PCAPNG block length: %d")

		local data_start = c.position
		c.position = block_start + block_len - 4

		local data = c (data_start, c.position - 1)
		-- TODO: also decode other types of blocks
		if block_type == 0x0a0d0d0a then decode_shb (data) end

		local shb_len_end = c:u32 ("PCAPNG trailing block length: %d")
	end
end

hex.register { type="pcapng", detect=detect_ng, decode=decode_ng }
