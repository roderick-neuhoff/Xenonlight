/*
 * void xthread_switch(struct xctx *from, struct xctx *to);
 *
 * Saves the callee-saved state of the running coroutine into *from and
 * resumes *to. The code is built -m32 -mpowerpc64, so GCC may keep 64-bit
 * values in the upper halves of GPRs; save full 64-bit registers.
 *
 * struct xctx layout (must be 16-byte aligned, see xthread.c):
 *   0   r1 (sp)        8   lr        16  cr        24  r2
 *   32  r13..r31 (19 x 8 bytes, ends at 184)
 *   184 f14..f31 (18 x 8 bytes, ends at 328)
 *   328 vrsave (4 bytes, padded to 336)
 *   336 v20..v31 (12 x 16 bytes, ends at 528)
 *   528 fpscr (8 bytes)
 */

	.section .text.xthread_switch,"ax",@progbits
	.globl xthread_switch
	.type xthread_switch,@function
	.align 4
xthread_switch:
	mflr	%r0
	mfcr	%r5
	std	%r1, 0(%r3)
	std	%r0, 8(%r3)
	std	%r5, 16(%r3)
	std	%r2, 24(%r3)
	std	%r13, 32(%r3)
	std	%r14, 40(%r3)
	std	%r15, 48(%r3)
	std	%r16, 56(%r3)
	std	%r17, 64(%r3)
	std	%r18, 72(%r3)
	std	%r19, 80(%r3)
	std	%r20, 88(%r3)
	std	%r21, 96(%r3)
	std	%r22, 104(%r3)
	std	%r23, 112(%r3)
	std	%r24, 120(%r3)
	std	%r25, 128(%r3)
	std	%r26, 136(%r3)
	std	%r27, 144(%r3)
	std	%r28, 152(%r3)
	std	%r29, 160(%r3)
	std	%r30, 168(%r3)
	std	%r31, 176(%r3)
	stfd	%f14, 184(%r3)
	stfd	%f15, 192(%r3)
	stfd	%f16, 200(%r3)
	stfd	%f17, 208(%r3)
	stfd	%f18, 216(%r3)
	stfd	%f19, 224(%r3)
	stfd	%f20, 232(%r3)
	stfd	%f21, 240(%r3)
	stfd	%f22, 248(%r3)
	stfd	%f23, 256(%r3)
	stfd	%f24, 264(%r3)
	stfd	%f25, 272(%r3)
	stfd	%f26, 280(%r3)
	stfd	%f27, 288(%r3)
	stfd	%f28, 296(%r3)
	stfd	%f29, 304(%r3)
	stfd	%f30, 312(%r3)
	stfd	%f31, 320(%r3)
	mfspr	%r5, 256		/* vrsave */
	stw	%r5, 328(%r3)
	li	%r5, 336
	stvx	%v20, %r3, %r5
	li	%r5, 352
	stvx	%v21, %r3, %r5
	li	%r5, 368
	stvx	%v22, %r3, %r5
	li	%r5, 384
	stvx	%v23, %r3, %r5
	li	%r5, 400
	stvx	%v24, %r3, %r5
	li	%r5, 416
	stvx	%v25, %r3, %r5
	li	%r5, 432
	stvx	%v26, %r3, %r5
	li	%r5, 448
	stvx	%v27, %r3, %r5
	li	%r5, 464
	stvx	%v28, %r3, %r5
	li	%r5, 480
	stvx	%v29, %r3, %r5
	li	%r5, 496
	stvx	%v30, %r3, %r5
	li	%r5, 512
	stvx	%v31, %r3, %r5
	mffs	%f0
	stfd	%f0, 528(%r3)

	/* ---- restore *to ---- */
	lfd	%f0, 528(%r4)
	mtfsf	0xff, %f0
	li	%r5, 336
	lvx	%v20, %r4, %r5
	li	%r5, 352
	lvx	%v21, %r4, %r5
	li	%r5, 368
	lvx	%v22, %r4, %r5
	li	%r5, 384
	lvx	%v23, %r4, %r5
	li	%r5, 400
	lvx	%v24, %r4, %r5
	li	%r5, 416
	lvx	%v25, %r4, %r5
	li	%r5, 432
	lvx	%v26, %r4, %r5
	li	%r5, 448
	lvx	%v27, %r4, %r5
	li	%r5, 464
	lvx	%v28, %r4, %r5
	li	%r5, 480
	lvx	%v29, %r4, %r5
	li	%r5, 496
	lvx	%v30, %r4, %r5
	li	%r5, 512
	lvx	%v31, %r4, %r5
	lwz	%r5, 328(%r4)
	mtspr	256, %r5
	lfd	%f14, 184(%r4)
	lfd	%f15, 192(%r4)
	lfd	%f16, 200(%r4)
	lfd	%f17, 208(%r4)
	lfd	%f18, 216(%r4)
	lfd	%f19, 224(%r4)
	lfd	%f20, 232(%r4)
	lfd	%f21, 240(%r4)
	lfd	%f22, 248(%r4)
	lfd	%f23, 256(%r4)
	lfd	%f24, 264(%r4)
	lfd	%f25, 272(%r4)
	lfd	%f26, 280(%r4)
	lfd	%f27, 288(%r4)
	lfd	%f28, 296(%r4)
	lfd	%f29, 304(%r4)
	lfd	%f30, 312(%r4)
	lfd	%f31, 320(%r4)
	ld	%r13, 32(%r4)
	ld	%r14, 40(%r4)
	ld	%r15, 48(%r4)
	ld	%r16, 56(%r4)
	ld	%r17, 64(%r4)
	ld	%r18, 72(%r4)
	ld	%r19, 80(%r4)
	ld	%r20, 88(%r4)
	ld	%r21, 96(%r4)
	ld	%r22, 104(%r4)
	ld	%r23, 112(%r4)
	ld	%r24, 120(%r4)
	ld	%r25, 128(%r4)
	ld	%r26, 136(%r4)
	ld	%r27, 144(%r4)
	ld	%r28, 152(%r4)
	ld	%r29, 160(%r4)
	ld	%r30, 168(%r4)
	ld	%r31, 176(%r4)
	ld	%r2, 24(%r4)
	ld	%r5, 16(%r4)
	mtcrf	0xff, %r5
	ld	%r0, 8(%r4)
	mtlr	%r0
	ld	%r1, 0(%r4)
	blr
	.size xthread_switch, .-xthread_switch
