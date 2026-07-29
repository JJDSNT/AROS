/*
 * Initial Emu68 entry point.
 *
 * The native (non-PiStorm) Emu68 initramfs loader starts an m68k ELF with:
 *   A6 = flattened device tree
 *   A7 = supervisor stack
 *   D0 = framebuffer pitch
 *   D1 = framebuffer width
 *   D2 = framebuffer height
 *   A0 = framebuffer address
 *
 * Emu68 loads ET_REL images by section and enters at the beginning of the
 * read-only allocation. Keep this stub first in .text.boot.
 */

	.section .text.boot,"ax",@progbits
	.balign 4
	.globl _start
	.type _start,@function

_start:
	move.l	%a6,emu68_fdt
	move.l	%a0,emu68_framebuffer
	move.l	%d0,emu68_framebuffer_pitch
	move.l	%d1,emu68_framebuffer_width
	move.l	%d2,emu68_framebuffer_height

	/*
	 * Milestone-one handoff: the loader has entered our ELF and its ABI has
	 * been captured. Exec initialization will replace this idle loop.
	 */
1:
	stop	#0x2700
	bra.s	1b

	.size _start,.-_start

	.section .bss,"aw",@nobits
	.balign 4
	.globl emu68_fdt
emu68_fdt:
	.space 4
	.globl emu68_framebuffer
emu68_framebuffer:
	.space 4
	.globl emu68_framebuffer_pitch
emu68_framebuffer_pitch:
	.space 4
	.globl emu68_framebuffer_width
emu68_framebuffer_width:
	.space 4
	.globl emu68_framebuffer_height
emu68_framebuffer_height:
	.space 4

