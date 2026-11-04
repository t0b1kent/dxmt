/* ARM64EC stack probe used by the c17 graphics build. x15 holds 16-byte units.
 * This leaf does not change sp or x18. Its 28 text bytes are checked against
 * the accepted c17 helper before the release build. */
.text
.p2align 2
.globl "#__chkstk_arm64ec"
"#__chkstk_arm64ec":
    lsl x16, x15, #4
    mov x17, sp
1:
    sub x17, x17, #1, lsl #12
    subs x16, x16, #1, lsl #12
    ldr xzr, [x17]
    b.gt 1b
    ret
