	.text
	.file	"/usr/local/google/home/dschuff/s/wasm-waterfall/src/work/gcc/gcc/testsuite/gcc.c-torture/execute/pr58364.c"
	.section	.text.foo,"ax",@progbits
	.hidden	foo
	.globl	foo
	.type	foo,@function
foo:                                    # @foo
	.param  	i32
	.result 	i32
# BB#0:                                 # %entry
	i32.const	$push2=, 1
	i32.const	$push0=, 0
	i32.lt_s	$push1=, $0, $pop0
	i32.select	$push3=, $pop2, $0, $pop1
                                        # fallthrough-return: $pop3
	.endfunc
.Lfunc_end0:
	.size	foo, .Lfunc_end0-foo

	.section	.text.main,"ax",@progbits
	.hidden	main
	.globl	main
	.type	main,@function
main:                                   # @main
	.result 	i32
# BB#0:                                 # %entry
	i32.const	$push6=, 0
	i32.const	$push5=, 0
	i32.store	b($pop6), $pop5
	block   	
	i32.const	$push4=, 0
	i32.load	$push1=, a($pop4)
	i32.const	$push3=, 0
	i32.load	$push0=, c($pop3)
	i32.le_s	$push2=, $pop1, $pop0
	br_if   	0, $pop2        # 0: down to label0
# BB#1:                                 # %if.end
	i32.const	$push7=, 0
	return  	$pop7
.LBB1_2:                                # %if.then
	end_block                       # label0:
	call    	abort@FUNCTION
	unreachable
	.endfunc
.Lfunc_end1:
	.size	main, .Lfunc_end1-main

	.hidden	a                       # @a
	.type	a,@object
	.section	.data.a,"aw",@progbits
	.globl	a
	.p2align	2
a:
	.int32	1                       # 0x1
	.size	a, 4

	.hidden	c                       # @c
	.type	c,@object
	.section	.bss.c,"aw",@nobits
	.globl	c
	.p2align	2
c:
	.int32	0                       # 0x0
	.size	c, 4

	.hidden	b                       # @b
	.type	b,@object
	.section	.bss.b,"aw",@nobits
	.globl	b
	.p2align	2
b:
	.int32	0                       # 0x0
	.size	b, 4


	.ident	"clang version 4.0.0 (trunk 283460) (llvm/trunk 283507)"
	.functype	abort, void
