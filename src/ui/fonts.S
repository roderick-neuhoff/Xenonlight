/* Embeds the UI fonts (Inter, SIL Open Font License; see assets/fonts). */
	.section .rodata
	.balign 16
	.globl font_regular_ttf
font_regular_ttf:
	.incbin "assets/fonts/Inter-Regular.ttf"
	.globl font_regular_ttf_end
font_regular_ttf_end:
	.balign 16
	.globl font_bold_ttf
font_bold_ttf:
	.incbin "assets/fonts/Inter-SemiBold.ttf"
	.globl font_bold_ttf_end
font_bold_ttf_end:
	.byte 0
