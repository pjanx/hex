--
-- elf.lua: Executable and Linkable Format
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

-- See man 5 elf, /usr/include/elf.h and /usr/include/llvm/Support/ELF.h

local detect = function (c)
	return #c >= 4 and c:read (4) == "\x7FELF"
end

local ph_type_table = {
	[0]   = "unused segment",
	[1]   = "loadable segment",
	[2]   = "dynamic linking information",
	[3]   = "interpreter pathname",
	[4]   = "auxiliary information",
	[5]   = "reserved",
	[6]   = "the program header table itself",
	[7]   = "the thread-local storage template"
}

local xform_ph_flags = function (u32)
	local info = {}
	if u32 & 0x4 ~= 0 then table.insert (info, "read")    end
	if u32 & 0x2 ~= 0 then table.insert (info, "write")   end
	if u32 & 0x1 ~= 0 then table.insert (info, "execute") end

	local junk = u32 & ~0x7
	if junk ~= 0 then
		table.insert (info, ("unknown: %#x"):format (junk))
	end

	if #info == 0 then return 0 end

	local result = info[1]
	for i = 2, #info do result = result .. ", " .. info[i] end
	return result
end

local decode_ph = function (elf, c)
	local ph = {}
	ph.type = c:u32 ("type: %s", function (u32)
		-- TODO: there are more known weird values and ranges
		name = ph_type_table[u32]
		if name then return name end
		return "unknown: %#x", u32
	end)
	if elf.class == 2 then ph.flags = c:u32 ("flags: %s", xform_ph_flags) end
	ph.offset = elf.uwide (c, "offset in file: %#x")
	ph.vaddr = elf.uwide (c, "virtual address: %#x")
	ph.paddr = elf.uwide (c, "physical address: %#x")
	ph.filesz = elf.uwide (c, "size in file: %d")
	ph.memsz = elf.uwide (c, "size in memory: %d")
	if elf.class == 1 then ph.flags = c:u32 ("flags: %s", xform_ph_flags) end
	ph.align = elf.uwide (c, "alignment: %d")
	return ph
end

local sh_type_table = {
	[0]   = "null entry",
	[1]   = "program-defined contents",
	[2]   = "symbol table",
	[3]   = "string table",
	[4]   = "relocation entries",
	[5]   = "symbol hash table",
	[6]   = "dynamic linking information",
	[7]   = "information about the file",
	[8]   = "data occupies no space in file",
	[9]   = "relocation entries",
	[10]  = "reserved",
	[11]  = "symbol table",
	[14]  = "pointers to initialization functions",
	[15]  = "pointers to terminnation functions",
	[16]  = "pointers to pre-init functions",
	[17]  = "section group",
	[18]  = "indices for SHN_XINDEX entries"
}

local xform_sh_flags = function (u)
	-- TODO: there are more known weird values and ranges
	local info = {}
	if u & 0x1 ~= 0 then table.insert (info, "write") end
	if u & 0x2 ~= 0 then table.insert (info, "alloc") end
	if u & 0x4 ~= 0 then table.insert (info, "execinstr") end

	local junk = u & ~0x7
	if junk ~= 0 then
		table.insert (info, ("unknown: %#x"):format (junk))
	end

	if #info == 0 then return 0 end

	local result = info[1]
	for i = 2, #info do result = result .. ", " .. info[i] end
	return result
end

local decode_sh = function (elf, c)
	local sh = {}
	-- TODO: decode the values, give the fields meaning
	sh.name = c:u32 ("name index: %d")
	sh.type = c:u32 ("type: %s", function (u32)
		-- TODO: there are more known weird values and ranges
		name = sh_type_table[u32]
		if name then return name end
		return "unknown: %#x", u32
	end)
	sh.flags = elf.uwide (c, "flags: %s", xform_sh_flags)
	sh.addr = elf.uwide (c, "load address: %#x")
	sh.offset = elf.uwide (c, "offset in file: %#x")
	sh.size = elf.uwide (c, "size: %d")
	sh.link = c:u32 ("header table index link: %d")
	sh.info = c:u32 ("extra information: %d")
	sh.addralign = elf.uwide (c, "address alignment: %d")
	sh.entsize = elf.uwide (c, "size of records: %d")
	return sh
end

local abi_table = {
	[0]   = "UNIX System V ABI",
	[1]   = "HP-UX operating system",
	[2]   = "NetBSD",
	[3]   = "GNU/Linux",
	[4]   = "GNU/Hurd",
	[6]   = "Solaris",
	[7]   = "AIX",
	[8]   = "IRIX",
	[9]   = "FreeBSD",
	[10]  = "TRU64 UNIX",
	[11]  = "Novell Modesto",
	[12]  = "OpenBSD",
	[13]  = "OpenVMS",
	[14]  = "Hewlett-Packard Non-Stop Kernel",
	[15]  = "AROS",
	[16]  = "FenixOS",
	[17]  = "Nuxi CloudABI",
	[64]  = "Bare-metal TMS320C6000",
	[64]  = "AMD HSA runtime",
	[65]  = "Linux TMS320C6000",
	[97]  = "ARM",
	[255] = "Standalone (embedded) application"
}

local type_table = {
	[0]   = "no file type",
	[1]   = "relocatable file",
	[2]   = "executable file",
	[3]   = "shared object file",
	[4]   = "core file"
}

local machine_table = {
	[0]   = "no machine",
	[1]   = "AT&T WE 32100",
	[2]   = "SPARC",
	[3]   = "Intel 386",
	[4]   = "Motorola 68000",
	[5]   = "Motorola 88000",
	[6]   = "Intel MCU",
	[7]   = "Intel 80860",
	[8]   = "MIPS R3000",
	[9]   = "IBM System/370",
	[10]  = "MIPS RS3000 Little-endian",
	[15]  = "Hewlett-Packard PA-RISC",
	[17]  = "Fujitsu VPP500",
	[18]  = "Enhanced instruction set SPARC",
	[19]  = "Intel 80960",
	[20]  = "PowerPC",
	[21]  = "PowerPC64",
	[22]  = "IBM System/390",
	[23]  = "IBM SPU/SPC",
	[36]  = "NEC V800",
	[37]  = "Fujitsu FR20",
	[38]  = "TRW RH-32",
	[39]  = "Motorola RCE",
	[40]  = "ARM",
	[41]  = "DEC Alpha",
	[42]  = "Hitachi SH",
	[43]  = "SPARC V9",
	[44]  = "Siemens TriCore",
	[45]  = "Argonaut RISC Core",
	[46]  = "Hitachi H8/300",
	[47]  = "Hitachi H8/300H",
	[48]  = "Hitachi H8S",
	[49]  = "Hitachi H8/500",
	[50]  = "Intel IA-64 processor architecture",
	[51]  = "Stanford MIPS-X",
	[52]  = "Motorola ColdFire",
	[53]  = "Motorola M68HC12",
	[54]  = "Fujitsu MMA Multimedia Accelerator",
	[55]  = "Siemens PCP",
	[56]  = "Sony nCPU embedded RISC processor",
	[57]  = "Denso NDR1 microprocessor",
	[58]  = "Motorola Star*Core processor",
	[59]  = "Toyota ME16 processor",
	[60]  = "STMicroelectronics ST100 processor",
	[61]  = "Advanced Logic Corp. TinyJ embedded processor family",
	[62]  = "AMD x86-64 architecture",
	[63]  = "Sony DSP Processor",
	[64]  = "Digital Equipment Corp. PDP-10",
	[65]  = "Digital Equipment Corp. PDP-11",
	[66]  = "Siemens FX66 microcontroller",
	[67]  = "STMicroelectronics ST9+ 8/16 bit microcontroller",
	[68]  = "STMicroelectronics ST7 8-bit microcontroller",
	[69]  = "Motorola MC68HC16 Microcontroller",
	[70]  = "Motorola MC68HC11 Microcontroller",
	[71]  = "Motorola MC68HC08 Microcontroller",
	[72]  = "Motorola MC68HC05 Microcontroller",
	[73]  = "Silicon Graphics SVx",
	[74]  = "STMicroelectronics ST19 8-bit microcontroller",
	[75]  = "Digital VAX",
	[76]  = "Axis Communications 32-bit embedded processor",
	[77]  = "Infineon Technologies 32-bit embedded processor",
	[78]  = "Element 14 64-bit DSP Processor",
	[79]  = "LSI Logic 16-bit DSP Processor",
	[80]  = "Donald Knuth's educational 64-bit processor",
	[81]  = "Harvard University machine-independent object files",
	[82]  = "SiTera Prism",
	[83]  = "Atmel AVR 8-bit microcontroller",
	[84]  = "Fujitsu FR30",
	[85]  = "Mitsubishi D10V",
	[86]  = "Mitsubishi D30V",
	[87]  = "NEC v850",
	[88]  = "Mitsubishi M32R",
	[89]  = "Matsushita MN10300",
	[90]  = "Matsushita MN10200",
	[91]  = "picoJava",
	[92]  = "OpenRISC 32-bit embedded processor",
	[93]  = "ARC International ARCompact processor",
	[94]  = "Tensilica Xtensa Architecture",
	[95]  = "Alphamosaic VideoCore processor",
	[96]  = "Thompson Multimedia General Purpose Processor",
	[97]  = "National Semiconductor 32000 series",
	[98]  = "Tenor Network TPC processor",
	[99]  = "Trebia SNP 1000 processor",
	[100] = "STMicroelectronics (www.st.com) ST200",
	[101] = "Ubicom IP2xxx microcontroller family",
	[102] = "MAX Processor",
	[103] = "National Semiconductor CompactRISC microprocessor",
	[104] = "Fujitsu F2MC16",
	[105] = "Texas Instruments embedded microcontroller msp430",
	[106] = "Analog Devices Blackfin (DSP) processor",
	[107] = "S1C33 Family of Seiko Epson processors",
	[108] = "Sharp embedded microprocessor",
	[109] = "Arca RISC Microprocessor",
	[110] = "microprocessor series from PKU-Unity Ltd." ..
		" and MPRC of Peking University",
	[111] = "eXcess: 16/32/64-bit configurable embedded CPU",
	[112] = "Icera Semiconductor Inc. Deep Execution Processor",
	[113] = "Altera Nios II soft-core processor",
	[114] = "National Semiconductor CompactRISC CRX",
	[115] = "Motorola XGATE embedded processor",
	[116] = "Infineon C16x/XC16x processor",
	[117] = "Renesas M16C series microprocessors",
	[118] = "Microchip Technology dsPIC30F Digital Signal Controller",
	[119] = "Freescale Communication Engine RISC core",
	[120] = "Renesas M32C series microprocessors",
	[131] = "Altium TSK3000 core",
	[132] = "Freescale RS08 embedded processor",
	[133] = "Analog Devices SHARC family of 32-bit DSP processors",
	[134] = "Cyan Technology eCOG2 microprocessor",
	[135] = "Sunplus S+core7 RISC processor",
	[136] = "New Japan Radio (NJR) 24-bit DSP Processor",
	[137] = "Broadcom VideoCore III processor",
	[138] = "RISC processor for Lattice FPGA architecture",
	[139] = "Seiko Epson C17 family",
	[140] = "The Texas Instruments TMS320C6000 DSP family",
	[141] = "The Texas Instruments TMS320C2000 DSP family",
	[142] = "The Texas Instruments TMS320C55x DSP family",
	[160] = "STMicroelectronics 64bit VLIW Data Signal Processor",
	[161] = "Cypress M8C microprocessor",
	[162] = "Renesas R32C series microprocessors",
	[163] = "NXP Semiconductors TriMedia architecture family",
	[164] = "Qualcomm Hexagon processor",
	[165] = "Intel 8051 and variants",
	[166] = "STMicroelectronics STxP7x family of configurable" ..
		" and extensible RISC processors",
	[167] = "Andes Technology compact code size embedded RISC processor family",
	[168] = "Cyan Technology eCOG1X family",
	[168] = "Cyan Technology eCOG1X family",
	[169] = "Dallas Semiconductor MAXQ30 Core Micro-controllers",
	[170] = "New Japan Radio (NJR) 16-bit DSP Processor",
	[171] = "M2000 Reconfigurable RISC Microprocessor",
	[172] = "Cray Inc. NV2 vector architecture",
	[173] = "Renesas RX family",
	[174] = "Imagination Technologies META processor architecture",
	[175] = "MCST Elbrus general purpose hardware architecture",
	[176] = "Cyan Technology eCOG16 family",
	[177] = "National Semiconductor CompactRISC CR16 16-bit microprocessor",
	[178] = "Freescale Extended Time Processing Unit",
	[179] = "Infineon Technologies SLE9X core",
	[180] = "Intel L10M",
	[181] = "Intel K10M",
	[183] = "ARM AArch64",
	[185] = "Atmel Corporation 32-bit microprocessor family",
	[186] = "STMicroeletronics STM8 8-bit microcontroller",
	[187] = "Tilera TILE64 multicore architecture family",
	[188] = "Tilera TILEPro multicore architecture family",
	[190] = "NVIDIA CUDA architecture",
	[191] = "Tilera TILE-Gx multicore architecture family",
	[192] = "CloudShield architecture family",
	[193] = "KIPO-KAIST Core-A 1st generation processor family",
	[194] = "KIPO-KAIST Core-A 2nd generation processor family",
	[195] = "Synopsys ARCompact V2",
	[196] = "Open8 8-bit RISC soft processor core",
	[197] = "Renesas RL78 family",
	[198] = "Broadcom VideoCore V processor",
	[199] = "Renesas 78KOR family",
	[200] = "Freescale 56800EX Digital Signal Controller (DSC)",
	[201] = "Beyond BA1 CPU architecture",
	[202] = "Beyond BA2 CPU architecture",
	[203] = "XMOS xCORE processor family",
	[204] = "Microchip 8-bit PIC(r) family",
	[205] = "reserved by Intel",
	[206] = "reserved by Intel",
	[207] = "reserved by Intel",
	[208] = "reserved by Intel",
	[209] = "reserved by Intel",
	[210] = "KM211 KM32 32-bit processor",
	[211] = "KM211 KMX32 32-bit processor",
	[212] = "KM211 KMX16 16-bit processor",
	[213] = "KM211 KMX8 8-bit processor",
	[214] = "KM211 KVARC processor",
	[215] = "Paneve CDP architecture family",
	[216] = "Cognitive Smart Memory Processor",
	[217] = "iCelero CoolEngine",
	[218] = "Nanoradio Optimized RISC",
	[219] = "CSR Kalimba architecture family",
	[224] = "AMD GPU architecture",
	[244] = "Lanai 32-bit processor",
	[247] = "Linux kernel bpf virtual machine"
}

local decode = function (c)
	assert (c.position == 1)
	if not detect (c ()) then error ("not an ELF file") end

	local p = c.position, c:read (4)
	c (p, p + 3):mark ("ELF magic")

	local elf = {}
	elf.class = c:u8 ("ELF class: %s", function (u8)
		if u8 == 1 then return "32-bit" end
		if u8 == 2 then return "64-bit" end
		return "invalid: %d", u8
	end)
	elf.data = c:u8 ("ELF data: %s", function (u8)
		if u8 == 1 then
			c.endianity = "le"
			return "little-endian"
		end
		if u8 == 2 then
			c.endianity = "be"
			return "big-endian"
		end
		return "invalid: %d", u8
	end)
	elf.version = c:u8 ("ELF version: %d")
	elf.abi = c:u8 ("OS ABI: %s", function (u8)
		name = abi_table[u8]
		if name then return name end
		return "unknown: %d", u8
	end)
	elf.abi_version = c:u8 ("OS ABI version: %d")

	-- The padding is reserved, no big sense in marking it
	local padding = c:read (7)

	-- We cannot decode anything further if we don't know endianity
	if elf.data ~= 1 and elf.data ~= 2 then return end

	-- And the same applies to the class
	if     elf.class == 1 then elf.uwide = c.u32
	elseif elf.class == 2 then elf.uwide = c.u64
	else return end

	elf.type = c:u16 ("type of file: %s", function (u16)
		name = type_table[u16]
		if name then return name end
		return "unknown: %d", u16
	end)
	elf.machine = c:u16 ("required architecture: %s", function (u16)
		name = machine_table[u16]
		if name then return name end
		return "unknown: %d", u16
	end)
	elf.version = c:u32 ("version: %d")
	elf.entry = elf.uwide (c, "program entry address: %#x")
	elf.ph_offset = elf.uwide (c, "program header table offset: %#x")
	elf.sh_offset = elf.uwide (c, "section header table offset: %#x")
	elf.flags = c:u32 ("processor-specific flags: %#x")
	elf.eh_size = c:u16 ("ELF header size: %d")
	c (1, elf.eh_size):mark ("ELF header")

	elf.ph_entry_size = c:u16 ("program header size: %d")
	elf.ph_number = c:u16 ("program header count: %d")
	elf.sh_entry_size = c:u16 ("section header size: %d")
	elf.sh_number = c:u16 ("section header count: %d")
	elf.sh_string_index = c:u16 ("section header index for strings: %d")

	for i = 1, elf.ph_number do
		local start = elf.ph_offset + (i - 1) * elf.ph_entry_size
		local pchunk = c (1 + start, start + elf.ph_entry_size)
		pchunk:mark ("ELF program header %d", i - 1)
		decode_ph (elf, pchunk)
	end

	-- Only mark section headers after we've decoded them all,
	-- so that we can name them using the section containing section names
	local shs = {}
	for i = 1, elf.sh_number do
		local start = elf.sh_offset + (i - 1) * elf.sh_entry_size
		local schunk = c (1 + start, start + elf.sh_entry_size)
		sh = decode_sh (elf, schunk)
		shs[i], shs[sh] = sh, schunk
	end

	local strings
	if elf.sh_string_index ~= 0 and elf.sh_string_index < elf.sh_number then
		local sh = shs[elf.sh_string_index + 1]
		strings = c (sh.offset + 1, sh.offset + sh.size)
	end
	for i, sh in ipairs (shs) do
		local schunk = shs[sh]
		if strings and sh.name < #strings then
			sh.name_string = strings (sh.name + 1):cstring ()
			schunk:mark ("ELF section header %d (%s)", i - 1, sh.name_string)
		else
			schunk:mark ("ELF section header %d", i - 1)
		end
	end
end

hex.register { type="elf", detect=detect, decode=decode }
