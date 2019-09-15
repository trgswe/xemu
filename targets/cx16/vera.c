/* The Xemu project.
   Copyright (C)2016-2019 LGB (Gábor Lénárt) <lgblgblgb@gmail.com>

   This is the Commander X16 emulation. Note: the source is overcrowded with comments by intent :)
   That it can useful for other people as well, or someone wants to contribute, etc ...

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

#include "xemu/emutools.h"
#include "xemu/emutools_files.h"
#include "vera.h"
#include "xemu/cpu65.h"


#define DEBUGVERA DEBUGPRINT
//#define DEBUGVERA(...)


#define MODE_NTSC_COMPOSITE	2

static struct {			// current_port variable selects one of these
	Uint8	regspace[3];	// used as "storage only", ie to be able to read back the registers by CPU, nothing more
	int	addr;
	int	inc;
} dataport[2];
static int	current_port;	// current dataport in use for CPU VERA registers 0/1/2

static struct {
	Uint8	regspace[0x10];
	int	map_base, tile_base;
	int	hscroll, vscroll, bitmap_palette_offset;
	int	enabled, mode, tileh, tilew, mapw, maph;
} layer[2];

static struct {
	int	index;
	Uint32	colour;
	int	changed;
} border;

static struct {
	Uint8	regspace[0x10];
	int	hstart, hstop, vstart, vstop;
	int	hscale, vscale;
	int 	mode, field;
	int	irqline;
	int	chromakill;
	int	is_mono_mode;	// a value takes chromakill and NTSC mode to be true to use the mono mode for real
} composer;

static struct {
	Uint32	colours[0x100];
	Uint32	all[4096];
	Uint32	all_mono[4096];
	Uint8	regspace[0x200];
} palette;

static Uint8	ien_reg, isr_reg;
// VRAM is actually only 128K. However this is a kind of optimization:
// We can read from "vmem" (which actually means here the entries Vera decoded address space including "gaps") with no further decoding.
// And it's much easier to handle things like "is the character set now in ROM or RAM?" since we need only offset in VMEM, no matter RAM/ROM or even setting to undecoded area, etc ...
static Uint8	vmem[1 * 1024 * 1024];


// This table is taken from Mist's X16 emulator, since I have no idea what the default palette is ...
static const Uint16 default_palette[] = {
	0x0000,0xfff,0x800,0xafe,0xc4c,0x0c5,0x00a,0xee7,0xd85,0x640,0xf77,0x333,0x777,0xaf6,0x08f,0xbbb,0x000,0x111,0x222,0x333,0x444,0x555,0x666,0x777,0x888,0x999,0xaaa,0xbbb,0xccc,0xddd,0xeee,0xfff,0x211,0x433,0x644,0x866,0xa88,0xc99,0xfbb,0x211,0x422,0x633,0x844,0xa55,0xc66,0xf77,0x200,0x411,0x611,0x822,0xa22,0xc33,0xf33,0x200,0x400,0x600,0x800,0xa00,0xc00,0xf00,0x221,0x443,0x664,0x886,0xaa8,0xcc9,0xfeb,0x211,0x432,0x653,0x874,0xa95,0xcb6,0xfd7,0x210,0x431,0x651,0x862,0xa82,0xca3,0xfc3,0x210,0x430,0x640,0x860,0xa80,0xc90,0xfb0,0x121,0x343,0x564,0x786,0x9a8,0xbc9,0xdfb,0x121,0x342,0x463,0x684,0x8a5,0x9c6,0xbf7,0x120,0x241,0x461,0x582,0x6a2,0x8c3,0x9f3,0x120,0x240,0x360,0x480,0x5a0,0x6c0,0x7f0,0x121,0x343,0x465,0x686,0x8a8,0x9ca,0xbfc,0x121,0x242,0x364,0x485,0x5a6,0x6c8,0x7f9,0x020,0x141,0x162,0x283,0x2a4,0x3c5,0x3f6,0x020,0x041,0x061,0x082,0x0a2,0x0c3,0x0f3,0x122,0x344,0x466,0x688,0x8aa,0x9cc,0xbff,0x122,0x244,0x366,0x488,0x5aa,0x6cc,0x7ff,0x022,0x144,0x166,0x288,0x2aa,0x3cc,0x3ff,0x022,0x044,0x066,0x088,0x0aa,0x0cc,0x0ff,0x112,0x334,0x456,0x668,0x88a,0x9ac,0xbcf,0x112,0x224,0x346,0x458,0x56a,0x68c,0x79f,0x002,0x114,0x126,0x238,0x24a,0x35c,0x36f,0x002,0x014,0x016,0x028,0x02a,0x03c,0x03f,0x112,0x334,0x546,0x768,0x98a,0xb9c,0xdbf,0x112,0x324,0x436,0x648,0x85a,0x96c,0xb7f,0x102,0x214,0x416,0x528,0x62a,0x83c,0x93f,0x102,0x204,0x306,0x408,0x50a,0x60c,0x70f,0x212,0x434,0x646,0x868,0xa8a,0xc9c,0xfbe,0x211,0x423,0x635,0x847,0xa59,0xc6b,0xf7d,0x201,0x413,0x615,0x826,0xa28,0xc3a,0xf3c,0x201,0x403,0x604,0x806,0xa08,0xc09,0xf0b
};



static XEMU_INLINE void UPDATE_IRQ ( void )
{
	// leave any upper bits as-is, used by other IRQ sources than VERA, in the emulation of VIAs for example
	// This works at the 65XX emulator level, that is a single irqLevel variable is though to be boolean as zero (no IRQ) or non-ZERO (IRQ request),
	// but, on other component level we use it as bit fields from various IRQ sources. That is, we save emulator time to create an 'OR" logic for
	// various interrupt sources.
	cpu65.irqLevel = (cpu65.irqLevel & ~7) | (ien_reg & isr_reg);
}


// Updates a palette enty based on the "addr" in palette-memory
// Also gives "monochrome" version if it's needed
// Also, updates the border colour if the changed x16 colour index is used for the border
static void update_palette_entry ( int addr )
{
	addr &= 0x1FE;
	int index = addr >> 1;
	Uint32 colour = *((XEMU_UNLIKELY(composer.is_mono_mode) ? palette.all_mono : palette.all) + (palette.regspace[addr] + ((palette.regspace[addr + 1] & 0xF) << 8)));
	//palatte[index] = XEMU_LIKELY(index) ? colour : 0;	// transparent, if index is zero
	palette.colours[index] = colour;
	if (index == border.index) {
		border.colour = colour;
		border.changed = 1;
	}
}


static XEMU_INLINE void update_all_palette_entries ( void )
{
	for (int a = 0; a < 0x200; a += 2)
		update_palette_entry(a);
}



static void write_layer_register ( int ln, int reg, Uint8 data )
{
	switch (reg) {
		case 0x0:
			layer[ln].enabled = data & 1;
			layer[ln].mode = data >> 5;
			if (layer[ln].mode >= 5) // bitmap mode
				layer[ln].hscroll &= 0xFF;
			else
				layer[ln].hscroll = (layer[ln].hscroll & 0xFF) | (layer[ln].bitmap_palette_offset << 8);
			DEBUGVERA("VERA: LAYER-%d#0 register write: enabled=%d mode=%d" NL, ln, layer[ln].enabled, layer[ln].mode);
			data &= 0x80 | 0x40 | 0x20 | 0x01;
			break;
		case 0x1:
			layer[ln].tileh = (data & 32) ? 1 : 0;
			layer[ln].tilew = (data & 16) ? 1 : 0;
			layer[ln].maph  = (data >> 2) & 3;
			layer[ln].mapw  =  data       & 3;
			DEBUGVERA("VERA: LAYER-%d#1 register write: tilew=%d tileh=%d mapw=%d maph=%d" NL, ln, layer[ln].tilew, layer[ln].tileh, layer[ln].mapw, layer[ln].maph);
			data &= 0xFF - 0x80 - 0x40;
			break;
		case 0x2:	// MAP base, bits 9 downto 2
			layer[ln].map_base = (layer[ln].map_base & 0x3FC00) | (data << 2);
			DEBUGVERA("VERA: LAYER-%d#2 register write: map_base=$%05X" NL, ln, layer[ln].map_base);
			break;
		case 0x3:	// MAP base, bits 17 downto 10
			layer[ln].map_base = (layer[ln].map_base & 0x3FC) | (data << 10);
			DEBUGVERA("VERA: LAYER-%d#3 register write: map_base=$%05X" NL, ln, layer[ln].map_base);
			break;
		case 0x4:	// TILE base, bits 9 downto 2
			layer[ln].tile_base = (layer[ln].tile_base & 0x3FC00) | (data << 2);
			DEBUGVERA("VERA: LAYER-%d#4 register write: tile_base=$%05X" NL, ln, layer[ln].tile_base);
			break;
		case 0x5:	// TILE base, bits 17 downto 10
			layer[ln].tile_base = (layer[ln].tile_base & 0x3FC) | (data << 10);
			DEBUGVERA("VERA: LAYER-%d#5 register write: tile_base=$%05X" NL, ln, layer[ln].tile_base);
			break;
		case 0x6:
			if (layer[ln].mode >= 5) // in bitmap modes ...
				layer[ln].hscroll = data;
			else
				layer[ln].hscroll = data | (layer[ln].bitmap_palette_offset << 8);
			DEBUGVERA("VERA: LAYER-%d#6 register write: hscroll=%d" NL, ln, layer[ln].hscroll);
			break;
		case 0x7:
			data &= 0xF;
			layer[ln].bitmap_palette_offset = data;	// only used in bitmap modes, but we always update it for reasons above ... [meaning of these bit can change!]
			if (layer[ln].mode < 5) // not biitmap modes ...
				layer[ln].hscroll = (layer[ln].hscroll & 0xFF) | (data << 8);
			DEBUGVERA("VERA: LAYER-%d#7 register write: hscroll=%d" NL, ln, layer[ln].hscroll);
			break;
		case 0x8:
			layer[ln].vscroll = (layer[ln].vscroll & 0xF00) | data;
			DEBUGVERA("VERA: LAYER-%d#8 register write: vscroll=%d" NL, ln, layer[ln].vscroll);
			break;
		case 0x9:
			data &= 0xF;
			layer[ln].vscroll = (layer[ln].vscroll & 0xFF) | (data << 8);
			DEBUGVERA("VERA: LAYER-%d#2 register write: vscroll=%d" NL, ln, layer[ln].vscroll);
			break;
		default:
			DEBUGVERA("VERA: LAYER-%d#%d register write: ?UNKNOWN_REGISTER?=?%d?" NL, ln, reg & 0xF, data);
			data = 0xFF;	// FIXME: unused register returns with 0xFF?
			break;
	}
	layer[ln].regspace[reg] = data;
}


static void write_composer_register ( int reg, Uint8 data )
{
	switch (reg) {
		case 0:	{
			composer.chromakill = data & 4;
			composer.mode = data & 3;
			int is_mono_mode = (composer.mode == MODE_NTSC_COMPOSITE) && (data & 4);
			if (is_mono_mode != composer.is_mono_mode) {
				composer.is_mono_mode = is_mono_mode;
				update_all_palette_entries();
			}
			DEBUGVERA("VERA: COMPOSER #0 register write: chroma_kill=%d output_mode=%d MONO_MODE=%d" NL, composer.chromakill ? 1 : 0, composer.mode, composer.is_mono_mode ? 1 : 0);
			data = composer.field | (data & 7);	// also use the interlace bit field [0x80 or 0x00] when register is used
			}
			break;
		case 1:
			composer.hscale = data;
			break;
		case 2:
			composer.vscale = data;
			break;
		default:
			data = 0xFF;	// FIXME: unused register returns with 0xFF?
			break;
	}
	composer.regspace[reg] = data;
}

static void write_vmem_through_dataport ( int port, Uint8 data )
{
	// FIXME: what is the wrapping point on incrementation? Ie, data port can address 24 bit, but VERA may not have such an addressing space
	int addr = dataport[port].addr;
	dataport[port].addr = (addr + dataport[port].inc) & 0xFFFFF;
	// OK, let's see, what is about to be written ...
	DEBUGPRINT("VERA: writing VMEM at $%05X with data $%02X" NL, addr, data);
	if (XEMU_LIKELY(addr < 0x20000)) {
		vmem[addr] = data;
		return;
	}
	if (XEMU_UNLIKELY(addr < 0xF0000 || addr > 0xF5FFF))
		return;	// writes between the "gap" and writes above the last used address do not have any affect
	if (addr < 0xF1000) {
		write_composer_register(addr & 0xF, data);
		return;
	}
	if (addr < 0xF2000) {
		addr &= 0x1FF;
		palette.regspace[addr] = data;
		update_palette_entry(addr);
		return;
	}
	if (addr < 0xF3000) {
		write_layer_register(0, addr & 0xF, data);
		return;
	}
	if (addr < 0xF4000) {
		write_layer_register(1, addr & 0xF, data);
		return;
	}
	// FIXME: implement other registers as well!
}


static Uint8 read_vmem_through_dataport ( int port )
{
	// FIXME: what is the wrapping point on incrementation? Ie, data port can address 24 bit, but VERA may not have such an addressing space
	int addr = dataport[port].addr;
	dataport[port].addr = (addr + dataport[port].inc) & 0xFFFFF;
	// OK, let's see, wwhat is about to read ...
	if (XEMU_LIKELY(addr < 0x20000)) {
		return vmem[addr];
	}
	if (XEMU_UNLIKELY(addr < 0xF0000 || addr > 0xF5FFF))
		return 0xFF;	// reads between the "gap" and writes above the last used address do not have any affect
	if (addr < 0xF1000) {
		return composer.regspace[addr & 0xF];
	}
	if (addr < 0xF2000) {
		return palette.regspace[addr & 0x1FF];
	}
	if (addr < 0xF3000) {
		return layer[0].regspace[addr & 0xF];
	}
	if (addr < 0xF4000) {
		return layer[1].regspace[addr & 0xF];
	}
	// FIXME: implement other registers as well!
	return 0xFF;
}


void vera_write_cpu_register ( int reg, Uint8 data )
{
	//increment = (1 << ((data >> 4) - 1)) & 0xFFFF;
	DEBUGPRINT("VERA: writing register %d with data $%02X" NL, reg & 7, data);
	switch (reg & 7) {
		case 0:
			dataport[current_port].regspace[0] = data;
			dataport[current_port].addr = (dataport[current_port].addr & 0xFFF00) | data;
			break;
		case 1:
			dataport[current_port].regspace[1] = data;
			dataport[current_port].addr = (dataport[current_port].addr & 0xF00FF) | (data << 8);
			break;
		case 2:
			dataport[current_port].regspace[2] = data;
			dataport[current_port].inc  = (1 << ((data >> 4) - 1)) & 0xFFFF;
			dataport[current_port].addr = (dataport[current_port].addr & 0x0FFFF) | ((data & 0xF) << 16);
			break;
		case 3:
			write_vmem_through_dataport(0, data);
			break;
		case 4:
			write_vmem_through_dataport(1, data);
			break;
		case 5:
			if (XEMU_UNLIKELY(data & 0x80))
				vera_reset();
			else
				current_port = data & 1;
			break;
		case 6:
			ien_reg = data & 7;
			UPDATE_IRQ();
			break;
		case 7:
			isr_reg &= (~data) & 7;
			UPDATE_IRQ();
			break;
	}
}


Uint8 vera_read_cpu_register ( int reg )
{
	switch (reg & 7) {
		case 0:
			return dataport[current_port].regspace[0];
		case 1:
			return dataport[current_port].regspace[1];
		case 2:
			return dataport[current_port].regspace[2];
		case 3:
			return read_vmem_through_dataport(0);
		case 4:
			return read_vmem_through_dataport(1);
		case 5:
			return current_port;		// only give back dataport. We always reports back "no reset" state, though we DO it on write, within zero cycles ;-P
		case 6:
			return ien_reg;
		case 7:
			return isr_reg & ien_reg;	// it seems from the doc, that only the 'effective' bits are set, ie if also that source is enabled
	}
	return 0xFF;	// some C compilers cannot figure out, that all possible cases ARE handled above, and gives a warning without this ... :(
}


static Uint32 *pixel;
static int scanline;
//static int map_base; //, tile_base;
static int tile_row;
static int map_counter;

static int hacky_shit = 0;


static void hacky_shitter ( void )
{
	FILE *f = fopen("vmem.dump", "w");
	if (f) {
		fwrite(vmem, sizeof vmem, 1, f);
		fclose(f);
	}
}


void vera_vsync ( void )
{
	int tail;
	pixel = xemu_start_pixel_buffer_access(&tail);
	scanline = 0;
	//map_base = (L1REGS[2] << 2) | (L1REGS[3] << 10);
	//tile_base = (L1REGS[4] << 2) | (L1REGS[5] << 10);
	map_counter = 0;
	tile_row = 0;
#if 0
	fprintf(stderr, "L1: EN=%d MODE=%d MAP_BASE=%d TILE_BASE=%d TILEH=%d TILEW=%d MAPH=%d MAPW=%d "
			"HSTOP=%d HSTART=%d VSTOP=%d VSTART=%d OUT_MODE=%d "
			"IEN=%d\n",
			L1REGS[0] & 1, L1REGS[0] >> 5, layer[0].map_base, layer[0].tile_base,
			(L1REGS[1] >> 5) & 1,	// TILEH
			(L1REGS[1] >> 4) & 1,	// TILEW
			(L1REGS[1] >> 2) & 3,	// MAPH
			L1REGS[1] & 3,		// MAPW
			DCREGS[5] | (((DCREGS[8] >> 2) & 3) << 8), // HSTOP
			DCREGS[4] | ((DCREGS[8] & 3) << 8), // HSTART
			DCREGS[7] | (((DCREGS[8] >> 5) & 1) << 8), // VSTOP
			DCREGS[6] | (((DCREGS[8] >> 4) & 1) << 8),  // VSTART
			DCREGS[0] & 3,
			ien_reg
	);
#endif
	if (!hacky_shit) {
		hacky_shit = 1;
		atexit(hacky_shitter);
	}
	isr_reg |= 1;
	UPDATE_IRQ();
}


// CURRENTLY a hard coded case, for 16 colour text mode using layer-1 only (what is the default used one by the KERNAL/BASIC at least)
int vera_render_line ( void )
{
	Uint8 *v = vmem + layer[0].map_base + map_counter;
	for (int x = 0; x < 80; x++) {
		Uint8 chr = vmem[layer[0].tile_base + ((*v++) << 3) + tile_row];
		Uint32 fg = palette.colours[*v & 0xF];
		Uint32 bg = palette.colours[(*v++) >> 4 ];
		for (int b = 128; b; b >>= 1) {
			*pixel++ = (chr & b) ? fg : bg;
		}
	}
	if (isr_reg & 1) {
		isr_reg &= ~1;
		UPDATE_IRQ();
	}
	scanline++;
	if (tile_row == 7) {
		tile_row = 0;
//		map_base += 256 - 80*2;
		map_counter += 256;
		return (scanline >= 480);
	} else {
		tile_row++;
//		map_base -= 80 * 2;
		return 0;
	}
}



void vera_reset ( void )
{
	current_port = 0;
	memset(dataport, 0, sizeof dataport);
	// Make sure, that we clear (maybe there was some during the reset request) + disable interrupts ...
	ien_reg = 0;
	isr_reg = 0;
	UPDATE_IRQ();
	// Populate the default palette for VERA after reset.
	border.index = 0;	// update palette step below will take it account then ...
	for (int a = 0, b = 0; a < 0x100;) {
		palette.regspace[b++] = default_palette[a  ];
		palette.regspace[b  ] = default_palette[a++] >> 8;
		update_palette_entry(b++);
	}
	// Clear all registers
	// This is needed, as those write functions decodes the meanings to structs
	// It's more easy,quick to solve this way, rather than to init those decoded stuffs by "hand",
	// also would result in code duplication more or less ...
	for (int a = 0; a < 0x10; a++) {
		write_layer_register(0, a, 0);
		write_layer_register(1, a, 0);
		write_composer_register(a, 0);
	}
}



// This function must be called only ONCE, before other vera... functions!
void vera_init ( void )
{
	// Generate full system palette [4096 colours]
	// Yeah, it's 4096 * 4 * 2 bytes of memory (*2 because of chroma killed mono version as well), but
	// that it's still "only" 32Kbytes, nothing for a modern PC, but then we have the advantage to get SDL formatted
	// DWORD available for any colour, without any conversion work in run-time!
	for (int r = 0, index = 0; r < 0x10; r++)
		for (int g = 0; g < 0x10; g++)
			for (int b = 0; b < 0x10; b++, index++) {
				// 0xFF = alpha channel
				// "*17" => 255/15 = 17.0 Thus we can reach full brightness on 4 bit RGB component values
				palette.all[index] = SDL_MapRGBA(sdl_pix_fmt, r * 17, g * 17, b * 17, 0xFF);
				// About CHROMAKILL:
				//	this is not the right solution too much, but anyway
				//	RGB -> mono conversion would need some gamma correction and whatever black magic still ...
				//	the formula is taken from wikipedia/MSDN, BUT as I noted above, it's maybe incorrect because of the lack of gamma correction
				//	FIXME: we would need to see HDL implmenetation of VERA to be sure what it uses ...
				//int mono = ((0.2125 * r) + (0.7154 * g) + (0.0721 * b)) * 17;
				int mono = ((0.299 * (float)r) + (0.587 * (float)g) + (0.114 * (float)b)) * 17.0;
				palette.all_mono[index] = SDL_MapRGBA(sdl_pix_fmt, mono, mono, mono, 0xFF);
			}
	SDL_free(sdl_pix_fmt);	// we don't need this any more
	sdl_pix_fmt = NULL;
	// Clear all of vmem
	memset(vmem, 0xFF, sizeof vmem);
	// Reset VERA
	vera_reset();
}
