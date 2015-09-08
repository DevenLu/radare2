/* radare - LGPL - Copyright 2014-2015 - pancake, condret */

#include <r_anal.h>
#include <r_types.h>
#include <r_util.h>
#include <r_db.h>

#define IFDBG if (esil->debug)
#define FLG(x) R_ANAL_ESIL_FLAG_##x
#define cpuflag(x,y) if (y) { R_BIT_SET (&esil->flags, FLG(x)); } else { R_BIT_UNSET (&esil->flags, FLG(x)); }

/* internal helper functions */

/* Returns the number that has bits+1 least significant bits set. */
static inline ut64 mask (int bits) {
	return (ut64)((((st64)2) << bits) - 1)>>1;
}

static int isnum (RAnalEsil *esil, const char *str, ut64 *num) {
	if (*str >= '0' && *str <= '9') {
		if (num)
			*num = r_num_get (NULL, str);
		return 1;
	}
	if (num)
		*num = 0;
	return 0;
}

static int isregornum(RAnalEsil *esil, const char *str, ut64 *num) {
	if (!r_anal_esil_reg_read (esil, str, num))
		if (!isnum (esil, str, num))
			return 0;
	return 1;
}

/* pop Register or Number */
int popRN(RAnalEsil *esil, ut64 *n) {
	char *str = r_anal_esil_pop (esil);
	if (str) {
		int ret = isregornum (esil, str, n);
		free (str);
		return ret;
	}
	return 0;
}

/* R_ANAL_ESIL API */

R_API RAnalEsil *r_anal_esil_new() {
	RAnalEsil *esil = R_NEW0 (RAnalEsil);
	if (!esil) return NULL;
	esil->parse_goto_count = R_ANAL_ESIL_GOTO_LIMIT;
	esil->ops = sdb_new0 ();
	esil->interrupts = sdb_new0 ();
	return esil;
}

R_API int r_anal_esil_set_op (RAnalEsil *esil, const char *op, RAnalEsilOp code) {
	char t[128];
	char *h;
	if (!code || !op || !strlen(op) || !esil || !esil->ops)
		return R_FALSE;
	h = sdb_itoa (sdb_hash (op), t, 16);
	sdb_num_set (esil->ops, h, (ut64)(size_t)code, 0);
	if (!sdb_num_exists (esil->ops, h)) {
		eprintf ("can't set esil-op %s\n", op);
		return R_FALSE;
	}
	return R_TRUE;
}

R_API int r_anal_esil_set_interrupt (RAnalEsil *esil, int interrupt, RAnalEsilInterruptCB interruptcb) {
	char t[128];
	char *i;
	if (!interruptcb || !esil || !esil->interrupts)
		return R_FALSE;
	i = sdb_itoa ((ut64) interrupt, t, 16);
	sdb_num_set (esil->interrupts, i, (ut64)(size_t)interruptcb, 0);
	if (!sdb_num_exists (esil->interrupts, i)) {
		eprintf ("can't set interrupt-handler for interrupt %d\n", interrupt);
		return R_FALSE;
	}
	return R_TRUE;
}

R_API int r_anal_esil_fire_interrupt (RAnalEsil *esil, int interrupt) {
	char t[128];
	char *i;
	RAnalEsilInterruptCB icb;
	if (!esil)
		return R_FALSE;
	if (esil->cmd) {
		if (esil->cmd (esil, esil->cmd_intr, interrupt)) {
			return R_TRUE;
		}
	}
	if (esil->anal) {
		RAnalPlugin *ap = esil->anal->cur;
		if (ap && ap->esil_intr) {
			if (ap->esil_intr (esil, interrupt))
				return R_TRUE;
		}
	}
	if (!esil->interrupts)
		return R_FALSE;
	i = sdb_itoa ((ut64) interrupt, t, 16);
	if (!sdb_num_exists (esil->interrupts, i)) {
		eprintf ("Cannot find interrupt-handler for interrupt %d\n", interrupt);
		return R_FALSE;
	}
	icb = (RAnalEsilInterruptCB)(size_t)sdb_num_get (esil->interrupts, i, 0);
	return icb (esil, interrupt);
}

R_API int r_anal_esil_set_offset(RAnalEsil *esil, ut64 off) {
	if (esil) {
		esil->offset = off;
		return R_TRUE;
	}
	return R_FALSE;
}

R_API void r_anal_esil_free (RAnalEsil *esil) {
	if (!esil)
		return;
	sdb_free (esil->ops);
	esil->ops = NULL;
	sdb_free (esil->interrupts);
	esil->interrupts = NULL;
	sdb_free (esil->stats);
	esil->stats = NULL;
	r_anal_esil_stack_free (esil);
	if (esil->anal && esil->anal->cur && esil->anal->cur->esil_fini)
		esil->anal->cur->esil_fini (esil);
	free (esil);
}

static ut8 esil_internal_sizeof_reg (RAnalEsil *esil, const char *r) {
	RRegItem *i;
	if (!esil || !esil->anal || !esil->anal->reg || !r)
		return R_FALSE;
	i = r_reg_get (esil->anal->reg, r, -1);
	if (!i)
		return R_FALSE;
	return (ut8)i->size;
}

static int internal_esil_mem_read(RAnalEsil *esil, ut64 addr, ut8 *buf, int len) {
	if (!esil || !esil->anal || !esil->anal->iob.io)
		return 0;
	return esil->anal->iob.read_at (esil->anal->iob.io, addr, buf, len);
}

R_API int r_anal_esil_mem_read(RAnalEsil *esil, ut64 addr, ut8 *buf, int len) {
	int i, ret = 0;
	if (!buf || !esil)
		return 0;
	if (esil->cb.hook_mem_read) {
		ret = esil->cb.hook_mem_read (esil, addr, buf, len);
	}
	if (!ret && esil->cb.mem_read) {
		ret = esil->cb.mem_read (esil, addr, buf, len);
	}
	IFDBG {
		eprintf ("0x%08"PFMT64x" R> ", addr);
		for (i=0; i<len; i++)
			eprintf ("%02x", buf[i]);
		eprintf ("\n");
	}
	return ret;
}

static int internal_esil_mem_write (RAnalEsil *esil, ut64 addr, const ut8 *buf, int len) {
	if (!esil || !esil->anal || !esil->anal->iob.io)
		return 0;
	return esil->anal->iob.write_at (esil->anal->iob.io, addr, buf, len);
}

R_API int r_anal_esil_mem_write (RAnalEsil *esil, ut64 addr, const ut8 *buf, int len) {
	int i, ret = 0;
	if (!buf || !esil)
		return 0;
	IFDBG {
		eprintf ("0x%08"PFMT64x" <W ", addr);
		for (i=0;i<len;i++)
			eprintf ("%02x", buf[i]);
		eprintf ("\n");
	}
	if (esil->cb.hook_mem_write) {
		ret = esil->cb.hook_mem_write (esil, addr, buf, len);
	}
	if (!ret && esil->cb.mem_write) {
		ret = esil->cb.mem_write (esil, addr, buf, len);
	}
	return ret;
}

static int internal_esil_reg_read(RAnalEsil *esil, const char *regname, ut64 *num) {
	RRegItem *reg = r_reg_get (esil->anal->reg, regname, -1);
	if (reg) {
		if (num)
			*num = r_reg_get_value (esil->anal->reg, reg);
		return 1;
	}
	return 0;
}

static int internal_esil_reg_write(RAnalEsil *esil, const char *regname, ut64 num) {
	RRegItem *reg = r_reg_get (esil->anal->reg, regname, -1);
	if (reg) {
		r_reg_set_value (esil->anal->reg, reg, num);
		return 1;
	}
	return 0;
}

static int esil_internal_borrow_check (RAnalEsil *esil, ut8 bit) {
	bit = ((bit & 0x3f) + 0x3f) & 0x3f;	//safer-sex version of -1
	return ((esil->old & mask(bit)) < (esil->cur & mask(bit)));
}

static int esil_internal_carry_check (RAnalEsil *esil, ut8 bit) {
	bit &= 0x3f;				//say no to weird bitshifts
	return ((esil->cur & mask(bit)) < (esil->old & mask(bit)));
}

static int esil_internal_parity_check (RAnalEsil *esil) {
	// Set if the number of set bits in the least significant byte is not a multiple of 2.
	int i, bits = esil->cur & 1;
	for (i = 1; i < 8; i++)
		bits ^= ((esil->cur & (1 << i)) >> i);
	return bits;
}

static int esil_internal_sign_check (RAnalEsil *esil) {
	if (!esil || !esil->lastsz)						//XXX we must rethink of how we set esil->lastsz (check the src) (a,a,^=,%%z,z,= esil->lastsz will be 1 here not sizeof(a))
		return R_FALSE;
	return !!((esil->cur & (0x1 << (esil->lastsz - 1))) >> (esil->lastsz - 1));
}

static int esil_internal_overflow_check (RAnalEsil *esil) {
	if (!esil ||  (esil->lastsz < 2))
		return R_FALSE;
	return (esil_internal_carry_check (esil, esil->lastsz-1) ^ esil_internal_carry_check (esil, esil->lastsz-2));	//according to wikipedia this should work
}															//cannot imagine any case wher both happens, maybe it's not that simple

R_API int r_anal_esil_pushnum(RAnalEsil *esil, ut64 num) {
	char str[64];
	snprintf (str, sizeof (str)-1, "0x%"PFMT64x, num);
	return r_anal_esil_push (esil, str);
}

R_API int r_anal_esil_push(RAnalEsil *esil, const char *str) {
	if (!str || !esil || !*str || esil->stackptr>30)
		return 0;
	esil->stack[esil->stackptr++] = strdup (str);
	return 1;
}

R_API char *r_anal_esil_pop(RAnalEsil *esil) {
	if (!esil || esil->stackptr<1)
		return NULL;
	return esil->stack[--esil->stackptr];
}

R_API int r_anal_esil_get_parm_type (RAnalEsil *esil, const char *str) {
	int len, i;
	if (!str || !(len=strlen(str)))
		return R_ANAL_ESIL_PARM_INVALID;
	if (str[0] == ESIL_INTERNAL_PREFIX) {
		if (len > 1)
			return R_ANAL_ESIL_PARM_INTERNAL;
		return R_ANAL_ESIL_PARM_INVALID;
	}
	if (!strncmp (str, "0x", 2))
		return R_ANAL_ESIL_PARM_NUM;
	if (!((str[0] >= '0' && str[0] <= '9')||str[0]=='-'))
		goto not_a_number;
	for (i = 1; i < len; i++)
		if (!(str[i] >= '0' && str[i] <= '9'))
			goto not_a_number;
	return R_ANAL_ESIL_PARM_NUM;
	not_a_number:
	if (r_reg_get (esil->anal->reg, str, -1))
		return R_ANAL_ESIL_PARM_REG;
	return R_ANAL_ESIL_PARM_INVALID;
}

static int esil_internal_read (RAnalEsil *esil, const char *str, ut64 *num) {
	ut8 bit;
	if (!str)
		return R_FALSE;
	if (esil->cb.hook_flag_read) {
		int ret = esil->cb.hook_flag_read (esil, str+1, num);
		if (ret)
			return R_TRUE;
	}
	switch (str[1]) {
	case '%':
		*num = esil->offset;
		break;
	case 'z':						//zero-flag
		*num = (esil->cur == 0);
		break;
	case 'b':						//borrow
		bit = (ut8) r_num_get (NULL, &str[2]);
		*num = esil_internal_borrow_check (esil, bit);
		break;
	case 'c':						//carry
		bit = (ut8) r_num_get (NULL, &str[2]);
		*num = esil_internal_carry_check (esil, bit);
		break;
	case 'o':						//overflow
		*num = esil_internal_overflow_check (esil);
		break;
	case 'p':						//parity
		*num = esil_internal_parity_check (esil);
		break;
	case 'r':						//regsize in 8-bit-bytes
		*num = esil->anal->bits/8;
		break;
	case 's':						//sign
		*num = esil_internal_sign_check (esil);
		break;
	default:
		return R_FALSE;
	}
	return R_TRUE;
}

R_API int r_anal_esil_get_parm (RAnalEsil *esil, const char *str, ut64 *num) {
	int parm_type = r_anal_esil_get_parm_type (esil, str);
	if (!num || !esil) return R_FALSE;
	switch (parm_type) {
	case R_ANAL_ESIL_PARM_INTERNAL:
		// *num = esil_internal_read (esil, str, num);
		return esil_internal_read (esil, str, num);
	case R_ANAL_ESIL_PARM_NUM:
		*num = r_num_get (NULL, str);
		return R_TRUE;
	case R_ANAL_ESIL_PARM_REG:
		if (!r_anal_esil_reg_read (esil, str, num))
			break;
		return R_TRUE;
	default:
		IFDBG eprintf ("Invalid arg (%s)\n", str);
		esil->parse_stop = 1;
		break;
	}
	return R_FALSE;
}

R_API int r_anal_esil_reg_write (RAnalEsil *esil, const char *dst, ut64 num) {
	int ret = 0;
	IFDBG {
		eprintf ("%s=0x%"PFMT64x"\n", dst, num);
	}
	if (esil->cb.hook_reg_write) {
		ret = esil->cb.hook_reg_write (esil, dst, num);
	}
	if (!ret && esil->cb.reg_write) {
		ret = esil->cb.reg_write (esil, dst, num);
	}
	return ret;
}

R_API int r_anal_esil_reg_read (RAnalEsil *esil, const char *regname, ut64 *num) {
	int ret = 0;
	ut64 localnum;
	if (!num)
		num = &localnum;
	*num = 0LL;
#if 0
// we have no hook for numeric reads.. so maybe this check must be removed to allow hooks work with numbers too
	if (regname[0]>='0' && regname[0]<='9') {
		// invalid register name
		return 0;
	}
#endif
	if (esil->cb.hook_reg_read) {
		ret = esil->cb.hook_reg_read (esil, regname, num);
	}
	if (!ret && esil->cb.reg_read) {
		ret = esil->cb.reg_read (esil, regname, num);
	}
	if (ret) {
		IFDBG {
			eprintf ("%s=0x%"PFMT64x"\n", regname, *num);
		}
	}
	return ret;
}

static int esil_eq (RAnalEsil *esil) {
	int ret = 0;
	ut64 num, num2;
	char *dst = r_anal_esil_pop (esil);
	char *src = r_anal_esil_pop (esil);
	if (src && dst && r_anal_esil_reg_read (esil, dst, &num)) {
		if (r_anal_esil_get_parm (esil, src, &num2)) {
			if (r_anal_esil_get_parm_type (esil, src) != R_ANAL_ESIL_PARM_INTERNAL) {	//necessary for some flag-things
				esil->cur = num2;
				esil->old = num;
				esil->lastsz = esil_internal_sizeof_reg (esil, dst);
			}
			ret = r_anal_esil_reg_write (esil, dst, num2);
		} else {
			IFDBG eprintf ("esil_eq: invalid src\n");
		}
	} else {
		IFDBG eprintf ("esil_eq: invalid parameters\n");
	}
	free (src);
	free (dst);
	return ret;
}

static int esil_neg(RAnalEsil *esil) {
	int ret = 0;
	ut64 num;
	char *src = r_anal_esil_pop (esil);
	if (src) {
		if (r_anal_esil_get_parm (esil, src, &num)) {
			r_anal_esil_pushnum (esil, !!!num);
			ret = 1;
		} else {
			if (isregornum (esil, src, &num)) {
				ret = 1;
				r_anal_esil_pushnum (esil, !!!num);
			} else {
				eprintf ("esil_neg: unknown reg %s\n", src);
			}
		}
	} else {
		eprintf ("esil_neg: empty stack\n");
	}
	free (src);
	return ret;
}

static int esil_negeq(RAnalEsil *esil) {
	int ret = 0;
	ut64 num;
	char *src = r_anal_esil_pop (esil);
	if (src && r_anal_esil_reg_read (esil, src, &num)) {
		num = !num;
		r_anal_esil_reg_write (esil, src, num);
		ret = 1;
	} else {
		eprintf ("esil_negeq: empty stack\n");
	}
	free (src);
	//r_anal_esil_pushnum (esil, ret);
	return ret;
}

static int esil_nop(RAnalEsil *esil) {
	return 0;
}

static int esil_andeq(RAnalEsil *esil) {
	int ret = 0;
	ut64 num, num2;
	char *dst = r_anal_esil_pop (esil);
	char *src = r_anal_esil_pop (esil);
	if (dst && r_anal_esil_reg_read (esil, dst, &num)) {
		if (src && r_anal_esil_get_parm (esil, src, &num2)) {
			if (r_anal_esil_get_parm_type (esil, src) != R_ANAL_ESIL_PARM_INTERNAL) {
				esil->old = num;
				esil->cur = num & num2;
				esil->lastsz = esil_internal_sizeof_reg (esil, dst);
			}
			r_anal_esil_reg_write (esil, dst, num & num2);
			ret = 1;
		} else {
			eprintf ("esil_andeq: empty stack\n");
		}
	}
	free (src);
	free (dst);
	return ret;
}

static int esil_oreq(RAnalEsil *esil) {
	int ret = 0;
	ut64 num, num2;
	char *dst = r_anal_esil_pop (esil);
	char *src = r_anal_esil_pop (esil);
	if (dst && r_anal_esil_reg_read (esil, dst, &num)) {
		if (src && r_anal_esil_get_parm (esil, src, &num2)) {
			if (r_anal_esil_get_parm_type (esil, src) != R_ANAL_ESIL_PARM_INTERNAL) {
				esil->old = num;
				esil->cur = num | num2;
				esil->lastsz = esil_internal_sizeof_reg (esil, dst);
			}
			r_anal_esil_reg_write (esil, dst, num | num2);
			ret = 1;
		} else {
			eprintf ("esil_ordeq: empty stack\n");
		}
	}
	free (src);
	free (dst);
	return ret;
}

static int esil_xoreq(RAnalEsil *esil) {
	int ret = 0;
	ut64 num, num2;
	char *dst = r_anal_esil_pop (esil);
	char *src = r_anal_esil_pop (esil);
	if (dst && r_anal_esil_reg_read (esil, dst, &num)) {
		if (src && r_anal_esil_get_parm (esil, src, &num2)) {
			if (r_anal_esil_get_parm_type (esil, src) != R_ANAL_ESIL_PARM_INTERNAL) {
				esil->old = num;
				esil->cur = num ^ num2;
				esil->lastsz = esil_internal_sizeof_reg (esil, dst);
			}
			r_anal_esil_reg_write (esil, dst, num ^ num2);
			ret = 1;
		} else {
			eprintf ("esil_xoreq: empty stack\n");
		}
	}
	free (src);
	free (dst);
	return ret;
}

#if 0
static int esil_interrupt_linux_i386(RAnalEsil *esil) {		//move this into a plugin
	ut32 sn, ret = 0;
	char *usn = r_anal_esil_pop (esil);
	if (usn) {
		sn = (ut32) r_num_get (NULL, usn);
	} else sn = 0x80;

	if (sn == 3) {
		// trap
		esil->trap = R_ANAL_TRAP_BREAKPOINT;
		esil->trap_code = 3;
		return -1;
	}

	if (sn != 0x80) {
		eprintf ("Interrupt 0x%x not handled.", sn);
		esil->trap = R_ANAL_TRAP_UNHANDLED;
		esil->trap_code = sn;
		return -1;
	}
#undef r
#define r(x) r_reg_getv(esil->anal->reg, "##x##")
#undef rs
#define rs(x,y) r_reg_setv(esil->anal->reg, "##x##",y)
	switch (r(eax)) {
	case 1:
		printf ("exit(%d)\n", (int)r(ebx));
		rs(eax, -1);
		// never return. stop execution somehow, throw an exception
		break;
	case 3:
		ret = r(edx);
		printf ("ret:%d = read(fd:%"PFMT64d", ptr:0x%08"PFMT64x", len:%"PFMT64d")\n",
			(int)ret, r(ebx), r(ecx), r(edx));
		rs(eax, ret);
		break;
	case 4:
		ret = r(edx);
		printf ("ret:%d = write(fd:%"PFMT64d", ptr:0x%08"PFMT64x", len:%"PFMT64d")\n",
			(int)ret, r(ebx), r(ecx), r(edx));
		rs(eax, ret);
		break;
	case 5:
		ret = -1;
		printf ("fd:%d = open(file:0x%08"PFMT64x", mode:%"PFMT64d", perm:%"PFMT64d")\n",
			(int)ret, r(ebx), r(ecx), r(edx));
		rs(eax, ret);
		break;
	}
#undef r
#undef rs
	return 0;
}
#endif

static int esil_trap(RAnalEsil *esil) {
	ut64 s, d;
	if (popRN (esil, &s) && popRN (esil, &d)) {
		esil->trap = s;
		esil->trap_code = d;
		return R_TRUE;
	}
	eprintf ("esil_trap: missing parameters in stack\n");
	return R_FALSE;
}

static int esil_interrupt(RAnalEsil *esil) {
	ut64 interrupt;
	if (popRN (esil, &interrupt)) {
		return r_anal_esil_fire_interrupt (esil, (int)interrupt);
	}
	return R_FALSE;
}

static int esil_cmp(RAnalEsil *esil) {
	ut64 num, num2;
	int ret = 0;
	char *dst = r_anal_esil_pop (esil);
	char *src = r_anal_esil_pop (esil);
	if (dst && r_anal_esil_get_parm (esil, dst, &num)) {
		if (src && r_anal_esil_get_parm (esil, src, &num2)) {
			esil->old = num;
			esil->cur = num - num2;
			ret = 1;
			if (r_reg_get (esil->anal->reg, dst, -1)) {
				esil->lastsz = esil_internal_sizeof_reg (esil, dst);
			} else if (r_reg_get (esil->anal->reg, src, -1)) {
				esil->lastsz = esil_internal_sizeof_reg (esil, src);
			}
		}
	}
	free (dst);
	free (src);

	//r_anal_esil_pushnum (esil, ret);
	return ret;
}

#if 0
x86 documentation:
CF - carry flag -- Set on high-order bit carry or borrow; cleared otherwise
	num>>63
PF - parity flag
	(num&0xff)
    Set if low-order eight bits of result contain an even number of "1" bits; cleared otherwise
ZF - zero flags
    Set if result is zero; cleared otherwise
	zf = num?0:1;
SF - sign flag
    Set equal to high-order bit of result (0 if positive 1 if negative)
	sf = ((st64)num)<0)?1:0;
OF - overflow flag
	if (a>0&&b>0 && (a+b)<0)
    Set if result is too large a positive number or too small a negative number (excluding sign bit) to fit in destination operand; cleared otherwise 

JBE : CF = 1 || ZF = 1

#endif

/*
 * Expects a string in the stack. Each char of the string represents a CPU flag.
 * Those relations are associated by the CPU itself and are used to move values
 * from the internal ESIL into the RReg instance.
 *
 * For example:
 *   zco,?=     # update zf, cf and of
 *
 * If we want to update the esil value of a specific flag we use the =? command
 *
 *    zf,z,=?    # esil[zf] = r_reg[zf]
 *
 * Defining new cpu flags
 */
#if 0
static int esil_ifset(RAnalEsil *esil) {
	char *s, *src = r_anal_esil_pop (esil);
	for (s=src; *s; s++) {
		switch (*s) {
		case 'z':
			r_anal_esil_reg_write (esil, "zf", R_BIT_CHK(&esil->flags, FLG(ZERO)));
			break;
		case 'c':
			r_anal_esil_reg_write (esil, "cf", R_BIT_CHK(&esil->flags, FLG(CARRY)));
			break;
		case 'o':
			r_anal_esil_reg_write (esil, "of", R_BIT_CHK(&esil->flags, FLG(OVERFLOW)));
			break;
		case 'p':
			r_anal_esil_reg_write (esil, "pf", R_BIT_CHK(&esil->flags, FLG(PARITY)));
			break;
		}
	}
	free (src);
	return 0;
}
#endif

static int esil_if(RAnalEsil *esil) {
	ut64 num;
	char *src = r_anal_esil_pop (esil);
	if (src) {
		// TODO: check return value
		(void)r_anal_esil_get_parm (esil, src, &num);
			// condition not matching, skipping until }
		if (!num)
			esil->skip = R_TRUE;
		return R_TRUE;
	}
	return R_FALSE;
}

static int esil_lsl(RAnalEsil *esil) {
	int ret = 0;
	ut64 num, num2;
	char *dst = r_anal_esil_pop (esil);
	char *src = r_anal_esil_pop (esil);
	if (dst && r_anal_esil_get_parm (esil, dst, &num)) {
		if (src && r_anal_esil_get_parm (esil, src, &num2)) {
			ut64 res = num << num2;
			r_anal_esil_pushnum (esil, res);
			ret = 1;
		} else {
			eprintf ("esil_lsl: empty stack\n");
		}
	}
	free (src);
	free (dst);
	return ret;
}

static int esil_lsleq(RAnalEsil *esil) {
	int ret = 0;
	ut64 num, num2;
	char *dst = r_anal_esil_pop (esil);
	char *src = r_anal_esil_pop (esil);
	if (dst && r_anal_esil_reg_read (esil, dst, &num)) {
		if (src && r_anal_esil_get_parm (esil, src, &num2)) {
			esil->old = num;
			num <<= num2;
			esil->cur = num;
			esil->lastsz = esil_internal_sizeof_reg (esil, dst);
			r_anal_esil_reg_write (esil, dst, num);
			ret = 1;
		} else {
			eprintf ("esil_lsleq: empty stack\n");
		}
	}
	free (src);
	free (dst);
	return ret;
}

static int esil_lsr(RAnalEsil *esil) {
	int ret = 0;
	ut64 num, num2;
	char *dst = r_anal_esil_pop (esil);
	char *src = r_anal_esil_pop (esil);
	if (dst && r_anal_esil_get_parm (esil, dst, &num)) {
		if (src && r_anal_esil_get_parm (esil, src, &num2)) {
			ut64 res = num >> num2;
			r_anal_esil_pushnum (esil, res);
			ret = 1;
		} else {
			eprintf ("esil_lsr: empty stack\n");
		}
	}
	free (src);
	free (dst);
	return ret;
}

static int esil_lsreq(RAnalEsil *esil) {
	int ret = 0;
	ut64 num, num2;
	char *dst = r_anal_esil_pop (esil);
	char *src = r_anal_esil_pop (esil);
	if (dst && r_anal_esil_reg_read (esil, dst, &num)) {
		if (src && r_anal_esil_get_parm (esil, src, &num2)) {
			esil->old = num;
			num >>= num2;
			esil->cur = num;
			esil->lastsz = esil_internal_sizeof_reg (esil, dst);
			r_anal_esil_reg_write (esil, dst, num);
			ret = 1;
		} else {
			eprintf ("esil_lsreq: empty stack\n");
		}
	}
	free (src);
	free (dst);
	return ret;
}

static int esil_and(RAnalEsil *esil) {
	int ret = 0;
	ut64 num, num2;
	char *dst = r_anal_esil_pop (esil);
	char *src = r_anal_esil_pop (esil);
	if (dst && r_anal_esil_get_parm (esil, dst, &num)) {
		if (src && r_anal_esil_get_parm (esil, src, &num2)) {
			num &= num2;
			r_anal_esil_pushnum (esil, num);
			ret = 1;
		} else {
			eprintf ("esil_and: empty stack\n");
		}
	}
	free (src);
	free (dst);
	return ret;
}

static int esil_xor(RAnalEsil *esil) {
	int ret = 0;
	ut64 num, num2;
	char *dst = r_anal_esil_pop (esil);
	char *src = r_anal_esil_pop (esil);
	if (dst && r_anal_esil_get_parm (esil, dst, &num)) {
		if (src && r_anal_esil_get_parm (esil, src, &num2)) {
			num ^= num2;
			r_anal_esil_pushnum (esil, num);
			ret = 1;
		} else {
			eprintf ("esil_xor: empty stack\n");
		}
	}
	free (src);
	free (dst);
	return ret;
}

static int esil_or(RAnalEsil *esil) {
	int ret = 0;
	ut64 num, num2;
	char *dst = r_anal_esil_pop (esil);
	char *src = r_anal_esil_pop (esil);
	if (dst && r_anal_esil_get_parm (esil, dst, &num)) {
		if (src && r_anal_esil_get_parm (esil, src, &num2)) {
			num |= num2;
			r_anal_esil_pushnum (esil, num);
			ret = 1;
		} else {
			eprintf ("esil_xor: empty stack\n");
		}
	}
	free (src);
	free (dst);
	return ret;
}

R_API int r_anal_esil_dumpstack (RAnalEsil *esil) {
	int i;
	if (!esil)
		return 0;
	if (esil->trap) {
		eprintf ("ESIL TRAP type %d 0x%x\n",
			esil->trap, esil->trap_code);
	}
	if (esil->stackptr<1) 
		return 0;
	//eprintf ("StackDump:\n");
	for (i=esil->stackptr-1; i>=0; i--) {
		esil->anal->cb_printf ("%s\n", esil->stack[i]);
	}
	return 1;
}

static int esil_break(RAnalEsil *esil) {
	esil->parse_stop = 1;
	return 1;
}

static int esil_clear(RAnalEsil *esil) {
	char *r;
	while ((r = r_anal_esil_pop (esil)))
		free (r);
	return 1;
}

static int esil_todo(RAnalEsil *esil) {
	esil->parse_stop = 2;
	return 1;
}

static int esil_goto(RAnalEsil *esil) {
	ut64 num = 0;
	char *src = r_anal_esil_pop (esil);
	if (src && *src && r_anal_esil_get_parm (esil, src, &num)) {
		esil->parse_goto = num;
	}
	free (src);
	return 1;
}

static int esil_pop(RAnalEsil *esil) {
	char *dst = r_anal_esil_pop (esil);
	free (dst);
	return 1;
}

static int esil_mod(RAnalEsil *esil) {
	int ret = 0;
	ut64 s, d;
	char *dst = r_anal_esil_pop (esil);
	char *src = r_anal_esil_pop (esil);
	if (src && r_anal_esil_get_parm (esil, src, &s)) {
		if (dst && r_anal_esil_get_parm (esil, dst, &d)) {
			if (s == 0) {
				eprintf ("esil_mod: Division by zero!\n");
				esil->trap = R_ANAL_TRAP_DIVBYZERO;
				esil->trap_code = 0;
			} else {
				r_anal_esil_pushnum (esil, d%s);
			}
			ret = 1;
		}
	} else {
		eprintf ("esil_mod: invalid parameters");
	}
	free (dst);
	free (src);
	return ret;
}

static int esil_modeq(RAnalEsil *esil) {
	int ret = 0;
	ut64 s, d;
	char *dst = r_anal_esil_pop (esil);
	char *src = r_anal_esil_pop (esil);
	if (src && r_anal_esil_get_parm (esil, src, &s)) {
		if (dst && r_anal_esil_reg_read (esil, dst, &d)) {
			if (s) {
				if (r_anal_esil_get_parm_type (esil, src) != R_ANAL_ESIL_PARM_INTERNAL) {
					esil->old = d;
					esil->cur = d%s;
					esil->lastsz = esil_internal_sizeof_reg (esil, dst);
				}
				r_anal_esil_reg_write (esil, dst, d%s);
			} else {
				eprintf ("esil_modeq: Division by zero!\n");
				esil->trap = R_ANAL_TRAP_DIVBYZERO;
				esil->trap_code = 0;
			}
			ret = 1;
		} else {
			eprintf ("esil_modeq: empty stack\n");
		}
	} else {
		eprintf ("esil_modeq: invalid parameters");
	}
	free (src);
	free (dst);
	return ret;
}

static int esil_div(RAnalEsil *esil) {
	int ret = 0;
	ut64 s, d;
	char *dst = r_anal_esil_pop (esil);
	char *src = r_anal_esil_pop (esil);
	if (src && r_anal_esil_get_parm (esil, src, &s)) {
		if (dst && r_anal_esil_get_parm (esil, dst, &d)) {
			if (s == 0) {
				eprintf ("esil_div: Division by zero!\n");
				esil->trap = R_ANAL_TRAP_DIVBYZERO;
				esil->trap_code = 0;
			} else  {
				r_anal_esil_pushnum (esil, d/s);
			}
			ret = 1;
		}
	} else {
		eprintf ("esil_div: invalid parameters");
	}
	free (src);
	free (dst);
	return ret;
}

static int esil_diveq (RAnalEsil *esil) {
	int ret = 0;
	ut64 s, d;
	char *dst = r_anal_esil_pop (esil);
	char *src = r_anal_esil_pop (esil);
	if (src && r_anal_esil_get_parm (esil, src, &s)) {
		if (dst && r_anal_esil_reg_read (esil, dst, &d)) {
			if (s) {
				if (r_anal_esil_get_parm_type (esil, src) != R_ANAL_ESIL_PARM_INTERNAL) {
					esil->old = d;
					esil->cur = d/s;
					esil->lastsz = esil_internal_sizeof_reg (esil, dst);
				}
				r_anal_esil_reg_write (esil, dst, d/s);
			} else {
				eprintf ("esil_diveq: Division by zero!\n");
				esil->trap = R_ANAL_TRAP_DIVBYZERO;
				esil->trap_code = 0;
			}
			ret = 1;
		} else {
			eprintf ("esil_diveq: empty stack\n");
		}
	} else {
		eprintf ("esil_diveq: invalid parameters");
	}
	free (src);
	free (dst);
	return ret;
}

static int esil_mul(RAnalEsil *esil) {
	int ret = 0;
	ut64 s, d;
	char *dst = r_anal_esil_pop (esil);
	char *src = r_anal_esil_pop (esil);
	if (src && r_anal_esil_get_parm (esil, src, &s)) {
		if (dst && r_anal_esil_get_parm (esil, dst, &d)) {
			r_anal_esil_pushnum (esil, d*s);
			ret = 1;
		} else {
			eprintf ("esil_mul: empty stack\n");
		}
	} else {
		eprintf ("esil_mul: invalid parameters");
	}
	free (src);
	free (dst);
	return ret;
}

static int esil_muleq (RAnalEsil *esil) {
	int ret = 0;
	ut64 s, d;
	char *dst = r_anal_esil_pop (esil);
	char *src = r_anal_esil_pop (esil);
	if (src && r_anal_esil_get_parm (esil, src, &s)) {
		if (dst && r_anal_esil_reg_read (esil, dst, &d)) {
			if (r_anal_esil_get_parm_type (esil, src) != R_ANAL_ESIL_PARM_INTERNAL) {
				esil->old = d;
				esil->cur = d*s;
				esil->lastsz = esil_internal_sizeof_reg (esil, dst);
			}
			r_anal_esil_reg_write (esil, dst, s*d);
			ret = R_TRUE;
		} else {
			eprintf ("esil_muleq: empty stack\n");
		}
	} else {
		eprintf ("esil_muleq: invalid parameters\n");
	}
	free (dst);
	free (src);
	return ret;
}

static int esil_add (RAnalEsil *esil) {
	int ret = 0;
	ut64 s, d;
	char *dst = r_anal_esil_pop (esil);
	char *src = r_anal_esil_pop (esil);
	if (src && r_anal_esil_get_parm (esil, src, &s)) {
		if (dst && r_anal_esil_get_parm (esil, dst, &d)) {
			r_anal_esil_pushnum (esil, s+d);
			ret = R_TRUE;
		}
	} else {
		eprintf ("esil_add: invalid parameters\n");
	}
	free (src);
	free (dst);
	return ret;
}

static int esil_addeq (RAnalEsil *esil) {
	int ret = 0;
	ut64 s, d;
	char *dst = r_anal_esil_pop (esil);
	char *src = r_anal_esil_pop (esil);
	if (src && r_anal_esil_get_parm (esil, src, &s)) {
		if (dst && r_anal_esil_reg_read (esil, dst, &d)) {
			if (r_anal_esil_get_parm_type (esil, src) != R_ANAL_ESIL_PARM_INTERNAL) {
				esil->old = d;
				esil->cur = d+s;
				esil->lastsz = esil_internal_sizeof_reg (esil, dst);
			}
			r_anal_esil_reg_write (esil, dst, s+d);
			ret = R_TRUE;
		}
	} else {
		eprintf ("esil_addeq: invalid parameters\n");
	}
	free (src);
	free (dst);
	return ret;
}

static int esil_inc (RAnalEsil *esil) {
	int ret = 0;
	ut64 s;
	char *src = r_anal_esil_pop (esil);
	if (src && r_anal_esil_get_parm (esil, src, &s)) {
		s++;
		r_anal_esil_pushnum (esil, s);
		ret = R_TRUE;
	} else {
		eprintf ("esil_inc: invalid parameters\n");
	}
	free (src);
	return ret;
}

static int esil_inceq (RAnalEsil *esil) {
	int ret = 0;
	ut64 sd;
	char *src_dst = r_anal_esil_pop (esil);
	if (src_dst && (r_anal_esil_get_parm_type (esil, src_dst) == R_ANAL_ESIL_PARM_REG) && r_anal_esil_get_parm (esil, src_dst, &sd)) {
		esil->old = sd;
		sd++;
		esil->cur = sd;
		r_anal_esil_reg_write (esil, src_dst, sd);
		esil->lastsz = esil_internal_sizeof_reg (esil, src_dst);
		ret = R_TRUE;
	} else {
		eprintf ("esil_inceq: invalid parameters\n");
	}
	free (src_dst);
	return ret;
}

static int esil_sub (RAnalEsil *esil) {
	int ret = 0;
	ut64 s = 0, d = 0;
	char *dst = r_anal_esil_pop (esil);
	char *src = r_anal_esil_pop (esil);
	if (src && r_anal_esil_get_parm (esil, src, &s)) {
		if (dst && r_anal_esil_get_parm (esil, dst, &d)) {
			r_anal_esil_pushnum (esil, d-s);
			ret = R_TRUE;
		} else {
			eprintf ("esil_sub: invalid parameters");
		}
	} else {
		eprintf ("esil_sub: invalid parameters\n");
	}
	free (src);
	free (dst);
	return ret;
}

static int esil_subeq (RAnalEsil *esil) {
	int ret = 0;
	ut64 s, d;
	char *dst = r_anal_esil_pop (esil);
	char *src = r_anal_esil_pop (esil);
	if (src && r_anal_esil_get_parm (esil, src, &s)) {
		if (dst && r_anal_esil_reg_read (esil, dst, &d)) {
			if (r_anal_esil_get_parm_type (esil, src) != R_ANAL_ESIL_PARM_INTERNAL) {
				esil->old = d;
				esil->cur = d-s;
				esil->lastsz = esil_internal_sizeof_reg (esil, dst);
			}
			r_anal_esil_reg_write (esil, dst, d-s);
			ret = R_TRUE;
		}
	} else {
		eprintf ("esil_subeq: invalid parameters\n");
	}
	free (src);
	free (dst);
	return ret;
}

static int esil_dec (RAnalEsil *esil) {
	int ret = 0;
	ut64 s;
	char *src = r_anal_esil_pop (esil);
	if (src && r_anal_esil_get_parm (esil, src, &s)) {
		s--;
		r_anal_esil_pushnum (esil, s);
		ret = R_TRUE;
	} else {
		eprintf ("esil_dec: invalid parameters\n");
	}
	free (src);
	return ret;
}

static int esil_deceq (RAnalEsil *esil) {
	int ret = 0;
	ut64 sd;
	char *src_dst = r_anal_esil_pop (esil);
	if (src_dst && (r_anal_esil_get_parm_type (esil, src_dst) == R_ANAL_ESIL_PARM_REG) && r_anal_esil_get_parm (esil, src_dst, &sd)) {
		esil->old = sd;
		sd--;
		esil->cur = sd;
		r_anal_esil_reg_write (esil, src_dst, sd);
		esil->lastsz = esil_internal_sizeof_reg (esil, src_dst);
		ret = R_TRUE;
	} else {
		eprintf ("esil_deceq: invalid parameters\n");
	}
	free (src_dst);
	return ret;
}

/* POKE */
static int esil_poke_n(RAnalEsil *esil, int bits) {
	int bitmask = mask (bits);
	ut64 num, addr;
	union {
		ut8 byte;
		ut16 word;
		ut32 dword;
		ut64 qword;
	} n, n2;
	char *dst = r_anal_esil_pop (esil);
	char *src = r_anal_esil_pop (esil);
	int bytes = bits / 8, ret = 0;
	if (bits%8) {
		free (src);
		free (dst);
		return 0;
	}
	//eprintf ("GONA POKE %d src:%s dst:%s\n", bits, src, dst);
	if (src && r_anal_esil_get_parm (esil, src, &num)) {
		if (dst && r_anal_esil_get_parm (esil, dst, &addr)) {
			int type = r_anal_esil_get_parm_type (esil, src);
			if (type != R_ANAL_ESIL_PARM_INTERNAL) {
				n.qword = n2.qword = 0;
				r_anal_esil_mem_read (esil, addr, (ut8 *)&n, bytes);
				r_mem_copyendian ((ut8 *)&n2, (ut8 *)&n, bytes, !esil->anal->big_endian);
				esil->old = n2.qword;
				esil->cur = (num & bitmask);
				esil->lastsz = bits;
				num = num & bitmask;
			}
			r_mem_copyendian ((ut8 *)&n, (ut8 *)&num, bytes, !esil->anal->big_endian);
			ret = r_anal_esil_mem_write (esil, addr, (const ut8*)&n, bytes);
		}
	}
	return ret;
}

static int esil_poke1(RAnalEsil *esil) { return esil_poke_n (esil, 8); }
static int esil_poke2(RAnalEsil *esil) { return esil_poke_n (esil, 16); }
static int esil_poke4(RAnalEsil *esil) { return esil_poke_n (esil, 32); }
static int esil_poke8(RAnalEsil *esil) { return esil_poke_n (esil, 64); }
static int esil_poke(RAnalEsil *esil) {
	switch (esil->anal->bits) {
	case 64: return esil_poke8 (esil);
	case 32: return esil_poke4 (esil);
	case 16: return esil_poke2 (esil);
	case 8: return esil_poke1 (esil);
	default: return 0;
	}
}

static int esil_poke_some(RAnalEsil *esil) {
	int i, ret = 0;
	ut64 ptr, regs;
	char *count, *dst = r_anal_esil_pop (esil);
	if (dst) {
		// reg
		isregornum (esil, dst, &ptr);
		count = r_anal_esil_pop (esil);
		if (count) {
			isregornum (esil, count, &regs);
			if (regs>0) {
				ut64 num64;
				ut32 num32;
				for (i=0; i<regs; i++) {
					char *foo = r_anal_esil_pop (esil);
					isregornum (esil, foo, &num64);
					/* TODO : implement peek here */
					// read from $dst 
					ret = r_anal_esil_mem_write (esil, ptr,
						(const ut8*)&num32, sizeof (num32));
					if (ret != sizeof (num32)) {
						eprintf ("Cannot write at 0x%08"PFMT64x"\n", ptr);
					}
					ptr += 4;
					free (foo);
				}
			}
			free (dst);
			free (count);
			return 1;
		}
		free (dst);
	}
	return 0;
}

/* PEEK */

static int esil_peek_n(RAnalEsil *esil, int bits) {
	char res[32];
	ut64 addr;
	int ret = 0, bytes = bits / 8;
	char *dst = r_anal_esil_pop (esil);
	if (bits % 8) {
		free (dst);
		return 0;
	}
	if (dst && isregornum (esil, dst, &addr)) {
		ut64 a, b, bitmask = mask (bits);
		ret = r_anal_esil_mem_read (esil, addr, (ut8*)&a, bytes);
		r_mem_copyendian ((ut8 *)&b, (const ut8*)&a, bytes ,!esil->anal->big_endian);
		snprintf (res, sizeof (res), "0x%"PFMT64x, b & bitmask);
		r_anal_esil_push (esil, res);
		esil->lastsz = bits;
	}
	free (dst);
	return ret;
}

static int esil_peek1(RAnalEsil *esil) { return esil_peek_n(esil, 8); }
static int esil_peek2(RAnalEsil *esil) { return esil_peek_n(esil, 16); }
static int esil_peek4(RAnalEsil *esil) { return esil_peek_n(esil, 32); }
static int esil_peek8(RAnalEsil *esil) { return esil_peek_n(esil, 64); }

static int esil_peek_some(RAnalEsil *esil) {
	int i, ret = 0;
	ut64 ptr, regs;
	// pop ptr
	char *count, *dst = r_anal_esil_pop (esil);
	if (dst) {
		// reg
		isregornum (esil, dst, &ptr);
		count = r_anal_esil_pop (esil);
		if (count) {
			isregornum (esil, count, &regs);
			if (regs>0) {
				ut32 num32;
				for (i=0; i<regs; i++) {
					char *foo = r_anal_esil_pop (esil);
					if (!foo) {
						eprintf ("Cannot pop in peek\n");
						return 0;
					}
					ret = r_anal_esil_mem_read (esil, ptr,
						(ut8*)&num32, sizeof (num32));
					if (ret == sizeof (num32)) {
						r_anal_esil_reg_write (esil, foo, num32);
					} else {
						eprintf ("Cannot peek from 0x%08"PFMT64x"\n", ptr);
					}
					ptr += 4;
					free (foo);
				}
			}
			free (dst);
			free (count);
			return 1;
		}
		free (dst);
	}
	return 0;
}

static int esil_peek(RAnalEsil *esil) {
	switch (esil->anal->bits) {
	case 64: return esil_peek8 (esil);
	case 32: return esil_peek4 (esil);
	case 16: return esil_peek2 (esil);
	case 8: return esil_peek1 (esil);
	}
	return 0;
}

/* OREQ */

static int esil_mem_oreq_n (RAnalEsil *esil, int bits) {
	int ret = 0;
	ut64 s, d;
	char *dst = r_anal_esil_pop (esil);				//save the dst-addr
	char *src0 = r_anal_esil_pop (esil);				//get the src
	char *src1 = NULL;
	if (src0 && r_anal_esil_get_parm (esil, src0, &s)) {		//get the src
		ret = 1;
		r_anal_esil_push (esil, dst);				//push the dst-addr
		ret &= esil_peek_n (esil, bits);			//read
		src1 = r_anal_esil_pop (esil);				//get the old dst-value
		if (src1 && r_anal_esil_get_parm (esil, src1, &d)) {	//get the old dst-value
			d |= s;						//calculate the new dst-value
			r_anal_esil_pushnum (esil, d);			//push the new dst-value
			r_anal_esil_push (esil, dst);			//push the dst-addr
			ret &= esil_poke_n (esil, bits);		//write
		} else	ret = 0;
	}
	if (!ret)
		eprintf ("esil_mem_oreq1: invalid parameters\n");
	free (dst);
	free (src0);
	free (src1);
	return ret;
}

static int esil_mem_oreq1 (RAnalEsil *esil) { return esil_mem_oreq_n (esil, 8); }
static int esil_mem_oreq2 (RAnalEsil *esil) { return esil_mem_oreq_n (esil, 16); }
static int esil_mem_oreq4 (RAnalEsil *esil) { return esil_mem_oreq_n (esil, 32); }
static int esil_mem_oreq8 (RAnalEsil *esil) { return esil_mem_oreq_n (esil, 64); }

static int esil_mem_oreq(RAnalEsil *esil) {
	switch (esil->anal->bits) {
	case 64: return esil_mem_oreq8 (esil);
	case 32: return esil_mem_oreq4 (esil);
	case 16: return esil_mem_oreq2 (esil);
	case 8: return esil_mem_oreq1 (esil);
	}
	return 0;
}

/* XOREQ */

static int esil_mem_xoreq_n (RAnalEsil *esil, int bits) {
	int ret = 0;
	ut64 s, d;
	char *dst = r_anal_esil_pop (esil);
	char *src0 = r_anal_esil_pop (esil);
	char *src1 = NULL;
	if (src0 && r_anal_esil_get_parm (esil, src0, &s)) {
		ret = 1;
		r_anal_esil_push (esil, dst);
		ret &= esil_peek_n (esil, bits);
		src1 = r_anal_esil_pop (esil);
		if (src1 && r_anal_esil_get_parm (esil, src1, &d)) {
			d ^= s;
			r_anal_esil_pushnum (esil, d);
			r_anal_esil_push (esil, dst);
			ret &= esil_poke_n (esil, bits);
		} else	ret = 0;
	}
	if (!ret)
		eprintf ("esil_mem_xoreq8: invalid parameters\n");
	free (dst);
	free (src0);
	free (src1);
	return ret;
}

static int esil_mem_xoreq1 (RAnalEsil *esil) { return esil_mem_xoreq_n (esil, 8); }
static int esil_mem_xoreq2 (RAnalEsil *esil) { return esil_mem_xoreq_n (esil, 16); }
static int esil_mem_xoreq4 (RAnalEsil *esil) { return esil_mem_xoreq_n (esil, 32); }
static int esil_mem_xoreq8 (RAnalEsil *esil) { return esil_mem_xoreq_n (esil, 64); }

static int esil_mem_xoreq(RAnalEsil *esil) {
	switch (esil->anal->bits) {
	case 64: return esil_mem_xoreq8 (esil);
	case 32: return esil_mem_xoreq4 (esil);
	case 16: return esil_mem_xoreq2 (esil);
	case 8: return esil_mem_xoreq1 (esil);
	}
	return 0;
}

/* ANDEQ */

static int esil_mem_andeq_n (RAnalEsil *esil, int bits) {
	int ret = 0;
	ut64 s, d;
	char *dst = r_anal_esil_pop (esil);
	char *src0 = r_anal_esil_pop (esil);
	char *src1 = NULL;
	if (src0 && r_anal_esil_get_parm (esil, src0, &s)) {
		ret = 1;
		r_anal_esil_push (esil, dst);
		ret &= esil_peek_n (esil, bits);
		src1 = r_anal_esil_pop (esil);
		if (src1 && r_anal_esil_get_parm (esil, src1, &d)) {
			d &= s;
			r_anal_esil_pushnum (esil, d);
			r_anal_esil_push (esil, dst);
			ret &= esil_poke_n (esil, bits);
		} else	ret = 0;
	}
	if (!ret)
		eprintf ("esil_mem_andeq1: invalid parameters\n");
	free (dst);
	free (src0);
	free (src1);
	return ret;
}

static int esil_mem_andeq1 (RAnalEsil *esil) { return esil_mem_andeq_n (esil, 8); }
static int esil_mem_andeq2 (RAnalEsil *esil) { return esil_mem_andeq_n (esil, 16); }
static int esil_mem_andeq4 (RAnalEsil *esil) { return esil_mem_andeq_n (esil, 32); }
static int esil_mem_andeq8 (RAnalEsil *esil) { return esil_mem_andeq_n (esil, 64); }

static int esil_mem_andeq(RAnalEsil *esil) {
	switch (esil->anal->bits) {
	case 64: return esil_mem_andeq8 (esil);
	case 32: return esil_mem_andeq4 (esil);
	case 16: return esil_mem_andeq2 (esil);
	case 8: return esil_mem_andeq1 (esil);
	}
	return 0;
}

/* ADDEQ */

static int esil_mem_addeq_n (RAnalEsil *esil, int bits) {
	int ret = 0;
	ut64 s, d;
	char *dst = r_anal_esil_pop (esil);
	char *src0 = r_anal_esil_pop (esil);
	char *src1 = NULL;
	if (src0 && r_anal_esil_get_parm (esil, src0, &s)) {
		ret = 1;
		r_anal_esil_push (esil, dst);
		ret &= esil_peek_n (esil, bits);
		src1 = r_anal_esil_pop (esil);
		if (src1 && r_anal_esil_get_parm (esil, src1, &d)) {
			d += s;
			r_anal_esil_pushnum (esil, d);
			r_anal_esil_push (esil, dst);
			ret &= esil_poke_n (esil, bits);
		} else	ret = 0;
	}
	if (!ret)
		eprintf ("esil_mem_addeq8: invalid parameters\n");
	free (dst);
	free (src0);
	free (src1);
	return ret;
}

static int esil_mem_addeq1 (RAnalEsil *esil) { return esil_mem_addeq_n (esil, 8); }
static int esil_mem_addeq2 (RAnalEsil *esil) { return esil_mem_addeq_n (esil, 16); }
static int esil_mem_addeq4 (RAnalEsil *esil) { return esil_mem_addeq_n (esil, 32); }
static int esil_mem_addeq8 (RAnalEsil *esil) { return esil_mem_addeq_n (esil, 64); }

static int esil_mem_addeq(RAnalEsil *esil) {
	switch (esil->anal->bits) {
	case 64: return esil_mem_addeq8 (esil);
	case 32: return esil_mem_addeq4 (esil);
	case 16: return esil_mem_addeq2 (esil);
	case 8: return esil_mem_addeq1 (esil);
	}
	return 0;
}

/* SUBEQ */

static int esil_mem_subeq_n (RAnalEsil *esil, int bits) {
	int ret = 0;
	ut64 s, d;
	char *dst = r_anal_esil_pop (esil);
	char *src0 = r_anal_esil_pop (esil);
	char *src1 = NULL;
	if (src0 && r_anal_esil_get_parm (esil, src0, &s)) {
		ret = 1;
		r_anal_esil_push (esil, dst);
		ret &= esil_peek_n (esil, bits);
		src1 = r_anal_esil_pop (esil);
		if (src1 && r_anal_esil_get_parm (esil, src1, &d)) {
			d -= s;
			r_anal_esil_pushnum (esil, d);
			r_anal_esil_push (esil, dst);
			ret &= esil_poke_n (esil, bits);
		} else	ret = 0;
	}
	if (!ret)
		eprintf ("esil_mem_subeq1: invalid parameters\n");
	free (dst);
	free (src0);
	free (src1);
	return ret;
}

static int esil_mem_subeq1 (RAnalEsil *esil) { return esil_mem_subeq_n (esil, 8); }
static int esil_mem_subeq2 (RAnalEsil *esil) { return esil_mem_subeq_n (esil, 16); }
static int esil_mem_subeq4 (RAnalEsil *esil) { return esil_mem_subeq_n (esil, 32); }
static int esil_mem_subeq8 (RAnalEsil *esil) { return esil_mem_subeq_n (esil, 64); }

static int esil_mem_subeq(RAnalEsil *esil) {
	switch (esil->anal->bits) {
	case 64: return esil_mem_subeq8 (esil);
	case 32: return esil_mem_subeq4 (esil);
	case 16: return esil_mem_subeq2 (esil);
	case 8: return esil_mem_subeq1 (esil);
	}
	return 0;
}

/* MODEQ */

static int esil_mem_modeq_n (RAnalEsil *esil, int bits) {
	int ret = 0;
	ut64 s, d;
	char *dst = r_anal_esil_pop (esil);
	char *src0 = r_anal_esil_pop (esil);
	char *src1 = NULL;
	if (src0 && r_anal_esil_get_parm (esil, src0, &s)) {
		if (s == 0) {
			eprintf ("esil_mem_modeq4: Division by zero!\n");
			esil->trap = R_ANAL_TRAP_DIVBYZERO;
			esil->trap_code = 0;
		} else {
			ret = 1;
			r_anal_esil_push (esil, dst);
			ret &= esil_peek_n (esil, bits);
			src1 = r_anal_esil_pop (esil);
			if (src1 && r_anal_esil_get_parm (esil, src1, &d)) {
				d = d % s;
				r_anal_esil_pushnum (esil, d);
				r_anal_esil_push (esil, dst);
				ret &= esil_poke_n (esil, bits);
			} else	ret = 0;
		}
	}
	if (!ret)
		eprintf ("esil_mem_modeq4: invalid parameters\n");
	free (dst);
	free (src0);
	free (src1);
	return ret;
}

static int esil_mem_modeq1 (RAnalEsil *esil) { return esil_mem_modeq_n (esil, 8); }
static int esil_mem_modeq2 (RAnalEsil *esil) { return esil_mem_modeq_n (esil, 16); }
static int esil_mem_modeq4 (RAnalEsil *esil) { return esil_mem_modeq_n (esil, 32); }
static int esil_mem_modeq8 (RAnalEsil *esil) { return esil_mem_modeq_n (esil, 64); }

static int esil_mem_modeq(RAnalEsil *esil) {
	switch (esil->anal->bits) {
	case 64: return esil_mem_modeq8 (esil);
	case 32: return esil_mem_modeq4 (esil);
	case 16: return esil_mem_modeq2 (esil);
	case 8: return esil_mem_modeq1 (esil);
	}
	return 0;
}

/* DIVEQ */

static int esil_mem_diveq_n (RAnalEsil *esil, int bits) {
	int ret = 0;
	ut64 s, d;
	char *dst = r_anal_esil_pop (esil);
	char *src0 = r_anal_esil_pop (esil);
	char *src1 = NULL;
	if (src0 && r_anal_esil_get_parm (esil, src0, &s)) {
		if (s == 0) {
			eprintf ("esil_mem_diveq8: Division by zero!\n");
			esil->trap = R_ANAL_TRAP_DIVBYZERO;
			esil->trap_code = 0;
		} else {
			ret = 1;
			r_anal_esil_push (esil, dst);
			ret &= esil_peek_n (esil, bits);
			src1 = r_anal_esil_pop (esil);
			if (src1 && r_anal_esil_get_parm (esil, src1, &d)) {
				d = d / s;
				r_anal_esil_pushnum (esil, d);
				r_anal_esil_push (esil, dst);
				ret &= esil_poke_n (esil, bits);
			} else	ret = 0;
		}
	}
	if (!ret)
		eprintf ("esil_mem_diveq8: invalid parameters\n");
	free (dst);
	free (src0);
	free (src1);
	return ret;
}

static int esil_mem_diveq1 (RAnalEsil *esil) { return esil_mem_diveq_n (esil, 8); }
static int esil_mem_diveq2 (RAnalEsil *esil) { return esil_mem_diveq_n (esil, 16); }
static int esil_mem_diveq4 (RAnalEsil *esil) { return esil_mem_diveq_n (esil, 32); }
static int esil_mem_diveq8 (RAnalEsil *esil) { return esil_mem_diveq_n (esil, 64); }

static int esil_mem_diveq(RAnalEsil *esil) {
	switch (esil->anal->bits) {
	case 64: return esil_mem_diveq8 (esil);
	case 32: return esil_mem_diveq4 (esil);
	case 16: return esil_mem_diveq2 (esil);
	case 8: return esil_mem_diveq1 (esil);
	}
	return 0;
}

/* MULEQ */

static int esil_mem_muleq_n (RAnalEsil *esil, int bits, ut64 bitmask) {
	int ret = 0;
	ut64 s, d;
	char *dst = r_anal_esil_pop (esil);
	char *src0 = r_anal_esil_pop (esil);
	char *src1 = NULL;
	if (src0 && r_anal_esil_get_parm (esil, src0, &s)) {
		ret = 1;
		r_anal_esil_push (esil, dst);
		ret &= esil_peek_n (esil, bits);
		src1 = r_anal_esil_pop (esil);
		if (src1 && r_anal_esil_get_parm (esil, src1, &d)) {
			d *= s;
			r_anal_esil_pushnum (esil, d);
			r_anal_esil_push (esil, dst);
			ret &= esil_poke_n (esil, bits);
		} else	ret = 0;
	}
	if (!ret)
		eprintf ("esil_mem_muleq1: invalid parameters\n");
	free (dst);
	free (src0);
	free (src1);
	return ret;
}

static int esil_mem_muleq1 (RAnalEsil *esil) { return esil_mem_muleq_n (esil, 8, UT8_MAX); }
static int esil_mem_muleq2 (RAnalEsil *esil) { return esil_mem_muleq_n (esil, 16, UT16_MAX); }
static int esil_mem_muleq4 (RAnalEsil *esil) { return esil_mem_muleq_n (esil, 32, UT32_MAX); }
static int esil_mem_muleq8 (RAnalEsil *esil) { return esil_mem_muleq_n (esil, 64, UT64_MAX); }

static int esil_mem_muleq(RAnalEsil *esil) {
	switch (esil->anal->bits) {
		case 64: return esil_mem_muleq8 (esil);
		case 32: return esil_mem_muleq4 (esil);
		case 16: return esil_mem_muleq2 (esil);
		case 8: return esil_mem_muleq1 (esil);
	}
	return 0;
}

/* INCEQ */

static int esil_mem_inceq_n (RAnalEsil *esil, int bits) {
	int ret = 0;
	ut64 s;
	char *off = r_anal_esil_pop (esil);
	char *src = NULL;
	if (off) {
		ret = 1;
		r_anal_esil_push (esil, off);
		ret &= esil_peek_n (esil, bits);
		src = r_anal_esil_pop (esil);
		if (src && r_anal_esil_get_parm (esil, src, &s)) {
			s++;
			r_anal_esil_pushnum (esil, s);
			r_anal_esil_push (esil, off);
			ret &= esil_poke_n (esil, bits);
		} else	ret = 0;
	}
	if (!ret)
		eprintf ("esil_mem_inceq8: invalid parameters\n");
	free (src);
	free (off);
	return ret;
}

static int esil_mem_inceq1 (RAnalEsil *esil) { return esil_mem_inceq_n (esil, 8); }
static int esil_mem_inceq2 (RAnalEsil *esil) { return esil_mem_inceq_n (esil, 16); }
static int esil_mem_inceq4 (RAnalEsil *esil) { return esil_mem_inceq_n (esil, 32); }
static int esil_mem_inceq8 (RAnalEsil *esil) { return esil_mem_inceq_n (esil, 64); }

static int esil_mem_inceq(RAnalEsil *esil) {
	switch (esil->anal->bits) {
		case 64: return esil_mem_inceq8 (esil);
		case 32: return esil_mem_inceq4 (esil);
		case 16: return esil_mem_inceq2 (esil);
		case 8: return esil_mem_inceq1 (esil);
	}
	return 0;
}

/* DECEQ */

static int esil_mem_deceq_n(RAnalEsil *esil, int bits) {
	int ret = 0;
	ut64 s;
	char *off = r_anal_esil_pop (esil);
	char *src = NULL;
	if (off) {
		ret = 1;
		r_anal_esil_push (esil, off);
		ret &= esil_peek_n (esil, bits);
		src = r_anal_esil_pop (esil);
		if (src && r_anal_esil_get_parm (esil, src, &s)) {
			s--;
			r_anal_esil_pushnum (esil, s);
			r_anal_esil_push (esil, off);
			ret &= esil_poke_n (esil, bits);
		} else ret = 0;
	}
	if (!ret)
		eprintf ("esil_mem_deceq8: invalid parameters\n");
	free (src);
	free (off);
	return ret;
}

static int esil_mem_deceq1 (RAnalEsil *esil) { return esil_mem_deceq_n (esil, 8); }
static int esil_mem_deceq2 (RAnalEsil *esil) { return esil_mem_deceq_n (esil, 16); }
static int esil_mem_deceq4 (RAnalEsil *esil) { return esil_mem_deceq_n (esil, 32); }
static int esil_mem_deceq8 (RAnalEsil *esil) { return esil_mem_deceq_n (esil, 64); }

static int esil_mem_deceq(RAnalEsil *esil) {
	switch (esil->anal->bits) {
	case 64: return esil_mem_deceq8 (esil);
	case 32: return esil_mem_deceq4 (esil);
	case 16: return esil_mem_deceq2 (esil);
	case 8: return esil_mem_deceq1 (esil);
	}
	return 0;
}

static int esil_dup (RAnalEsil *esil) {
	char *dup_me;
	ut64 dup;
	if (!esil)
		return R_FALSE;
	dup_me = r_anal_esil_pop (esil);
	if (!r_anal_esil_get_parm (esil, dup_me, &dup))
		return R_FALSE;
	free (dup_me);
	return r_anal_esil_pushnum (esil, dup);
}

/* in case of fail, we must set some var */
static int esil_smaller(RAnalEsil *esil) {		// 'src < dst' => 'src,dst,<'
	ut64 s, d;
	if (!popRN (esil, &s)) {
		eprintf ("esil_smaller: src is broken\n");
		return R_FALSE;
	}
	if (!popRN (esil, &d)) {
		eprintf ("esil_smaller: dst is broken\n");
		return R_FALSE;
	}
	r_anal_esil_pushnum (esil, (s < d));
	return R_TRUE;
}

// TODO: 
// sign is not handled
// ESIL flags not updated?
static int esil_bigger(RAnalEsil *esil) {		// 'src > dst' => 'src,dst,>'
	int ret = 0;
	ut64 s, d;
	char *dst = r_anal_esil_pop (esil);
	char *src = r_anal_esil_pop (esil);
	if (src && isregornum (esil, src, &s)) {
		if (dst && isregornum (esil, dst, &d)) {
			r_anal_esil_pushnum(esil, (s > d));
			ret = 1;
		} else {
			eprintf ("esil_bigger: dst is broken\n");
		}
	} else {
		eprintf ("esil_bigger: src is broken\n");
	}
	free (src);
	free (dst);
	return ret;
}

static int esil_smaller_equal(RAnalEsil *esil) {		// 'src <= dst' => 'src,dst,<='
	int ret = 0;
	ut64 s, d;
	char *dst = r_anal_esil_pop (esil);
	char *src = r_anal_esil_pop (esil);
	if (src && isregornum (esil, src, &s)) {
		if (dst && isregornum (esil, dst, &d)) {
			r_anal_esil_pushnum(esil, (s <= d));
			ret = 1;
		} else {
			eprintf ("esil_smaller_equal: dst is broken\n");
		}
	} else {
		eprintf ("esil_smaller_equal: src is broken\n");
	}
	free (src);
	free (dst);
	return ret;
}

static int esil_bigger_equal(RAnalEsil *esil) {		// 'src >= dst' => 'src,dst,>='
	int ret = 0;
	ut64 s, d;
	char *dst = r_anal_esil_pop (esil);
	char *src = r_anal_esil_pop (esil);
	if (src && isregornum (esil, src, &s)) {
		if (dst && isregornum (esil, dst, &d)) {
			r_anal_esil_pushnum(esil, (s >= d));
			ret = 1;
		} else {
			eprintf ("esil_bigger_equal: dst is broken\n");
		}
	} else {
		eprintf ("esil_bigger_equal: src is broken\n");
	}
	free (src);
	free (dst);
	return ret;
}

static int iscommand (RAnalEsil *esil, const char *word, RAnalEsilOp *op) {
	char t[128];
	char *h;
	h = sdb_itoa (sdb_hash (word), t, 16);
	if (sdb_num_exists (esil->ops, h)) {
		*op = (RAnalEsilOp) (size_t)sdb_num_get (esil->ops, h, 0);
		return R_TRUE;
	}
	return R_FALSE;
}

static int runword (RAnalEsil *esil, const char *word) {
	RAnalEsilOp op = NULL;
	esil->parse_goto_count--;
	if (esil->parse_goto_count<1) {
		eprintf ("ESIL infinite loop detected\n");
		esil->trap = 1; // INTERNAL ERROR
		esil->parse_stop = 1; // INTERNAL ERROR
		return 0;
	}

	// Don't push anything onto stack when processing if statements
	if (!strcmp (word, "?{") && esil->Reil) {
		esil->Reil->skip = esil->Reil->skip? 0:1;
		if (esil->Reil->skip) {
			esil->Reil->cmd_count = 0;
			memset (esil->Reil->if_buf, 0, sizeof(esil->Reil->if_buf));
		}
	}

	if (esil->Reil && esil->Reil->skip) {
		int tmp_len = strlen (esil->Reil->if_buf);
		strncat (esil->Reil->if_buf, word, sizeof(esil->Reil->if_buf) - tmp_len - 2);
		strncat (esil->Reil->if_buf, ",", 1);
		if (!strcmp (word, "}"))  {
			r_anal_esil_pushnum (esil, esil->Reil->addr + esil->Reil->cmd_count + 1);
			r_anal_esil_parse (esil, esil->Reil->if_buf);
			return 1;
		}
		if (iscommand (esil, word, &op)) esil->Reil->cmd_count++;
		return 1;
	}

//eprintf ("WORD (%d) (%s)\n", esil->skip, word);
	if (!strcmp (word, "}{")) {
		esil->skip = esil->skip? 0: 1;
		return 1;
	} else if (!strcmp (word, "}")) {
		esil->skip = 0;
		return 1;
	}
	if (esil->skip) {
		return 1;
	}

	if (iscommand (esil, word, &op)) {
		// run action
		if (op) {
			if (esil->cb.hook_command) {
				if (esil->cb.hook_command (esil, word))
					return 1; // XXX cannot return != 1
			}
			return op (esil);
		}
	}
	if (!*word || *word==',') {
		// skip empty words
		return 1;
	}

	// push value
	if (!r_anal_esil_push (esil, word)) {
		eprintf ("ESIL stack is full\n");
		esil->trap = 1;
		esil->trap_code = 1;
	}
	return 1;
}

static const char *gotoWord(const char *str, int n) {
	const char *ostr = str;
	int count = 0;
	while (*str) {
		if (count == n)
			return ostr;
		str++;
		if (*str == ',') {
			ostr = str+1;
			count++;
		}
	}
	return NULL;
}

/** evaluate an esil word and return the action to perform
 * TODO: Use `enum` here
 * 0: continue running the
 * 1: stop execution
 * 2: continue in loop
 * 3: normal continuation
 */
static int evalWord (RAnalEsil *esil, const char *ostr, const char **str) {
	if ((*str)[0] && (*str)[1]==',')
		return 2;
	if (esil->repeat)
		return 0;
	if (esil->parse_goto != -1) {
		// TODO: detect infinite loop??? how??
		*str = gotoWord (ostr, esil->parse_goto);
		if (*str) {
			esil->parse_goto = -1;
			return 2;
		}
		eprintf ("Cannot find word %d\n", esil->parse_goto);
		return 1;
	}
	if (esil->parse_stop) {
		if (esil->parse_stop == 2) {
			eprintf ("ESIL TODO: %s\n", *str+1);
		}
		return 1;
	}
	return 3;
}

R_API int r_anal_esil_parse(RAnalEsil *esil, const char *str) {
	int wordi = 0;
	int dorunword;
	char word[64];
	const char *ostr = str;
	if (!esil)
		return 0;
	esil->trap = 0;
loop:
	esil->repeat = 0;
	esil->skip = 0;
	esil->parse_goto = -1;
	esil->parse_stop = 0;
	if (esil->anal) {
		esil->parse_goto_count = esil->anal->esil_goto_limit;
	} else {
		esil->parse_goto_count = R_ANAL_ESIL_GOTO_LIMIT;
	}
	str = ostr;
repeat:
	wordi = 0;
	while (*str) {
		if (wordi>62) {
			eprintf ("Invalid esil string\n");
			return -1;
		}
		dorunword = 0;
		if (*str == ';') {
			word[wordi] = 0;
			dorunword = 1;
		}
		if (*str == ',') {
			word[wordi] = 0;
			dorunword = 2;
		}

		if (dorunword) {
			if (!runword (esil, word)) {
				return 0;
			}
			word[wordi] = ',';
			wordi = 0;
			switch (evalWord (esil, ostr, &str)) {
			case 0: goto loop;
			case 1: return 0;
			case 2: continue;
			}
			if (dorunword==1)
				return 0;
			str++;
		}
		word[wordi++] = *str;
		str++;
	}
	word[wordi] = 0;
	if (*word) {
		if (!runword (esil, word))
			return 0;
		switch (evalWord (esil, ostr, &str)) {
		case 0: goto loop;
		case 1: return 0;
		case 2: goto repeat;
		}
	}
	return 1;
}

R_API void  r_anal_esil_stack_free (RAnalEsil *esil) {
	int i;
	if (esil) {
		for (i=0; i<esil->stackptr; i++)
			R_FREE (esil->stack[i]);
		esil->stackptr = 0;
	}
}

R_API int r_anal_esil_condition(RAnalEsil *esil, const char *str) {
	char *popped;
	int ret;
	if (!esil)
		return R_FALSE;
	while (*str==' ') str++; // use proper string chop?
	ret = r_anal_esil_parse (esil, str);
	popped = r_anal_esil_pop (esil);
	if (popped) {
		ut64 num;
		if (isregornum (esil, popped, &num)) {
			ret = !! num;
		} else ret = 0;
		free (popped);
	} else {
		eprintf ("ESIL stack is empty\n");
		return -1;
	}
	return ret;
}

static void r_anal_esil_setup_ops(RAnalEsil *esil) {
#define OP(x,y) r_anal_esil_set_op(esil,x,y)
	OP ("$", esil_interrupt);
	OP ("$$", esil_trap);
	OP ("==", esil_cmp);
	OP ("<", esil_smaller);
	OP (">", esil_bigger);
	OP ("<=", esil_smaller_equal);
	OP (">=", esil_bigger_equal);
	OP ("?{", esil_if);
	OP ("<<", esil_lsl);
	OP ("<<=", esil_lsleq);
	OP (">>", esil_lsr);
	OP (">>=", esil_lsreq);
	OP ("&", esil_and);
	OP ("&=", esil_andeq);
	OP ("}", esil_nop); // just to avoid push
	OP ("|", esil_or);
	OP ("|=", esil_oreq);
	OP ("!", esil_neg);
	OP ("!=", esil_negeq);
	OP ("=", esil_eq);
	OP ("*", esil_mul);
	OP ("*=", esil_muleq);
	OP ("^", esil_xor);
	OP ("^=", esil_xoreq);
	OP ("+", esil_add);
	OP ("+=", esil_addeq);
	OP ("++", esil_inc);
	OP ("++=", esil_inceq);
	OP ("-", esil_sub);
	OP ("-=", esil_subeq);
	OP ("--", esil_dec);
	OP ("--=", esil_deceq);
	OP ("/", esil_div);
	OP ("/=", esil_diveq);
	OP ("%", esil_mod);
	OP ("%=", esil_modeq);
	OP ("=[]", esil_poke);
	OP ("=[1]", esil_poke1);
	OP ("=[2]", esil_poke2);
	OP ("=[4]", esil_poke4);
	OP ("=[8]", esil_poke8);
	OP ("|=[]", esil_mem_oreq);
	OP ("|=[1]", esil_mem_oreq1);
	OP ("|=[2]", esil_mem_oreq2);
	OP ("|=[4]", esil_mem_oreq4);
	OP ("|=[8]", esil_mem_oreq8);
	OP ("^=[]", esil_mem_xoreq);
	OP ("^=[1]", esil_mem_xoreq1);
	OP ("^=[2]", esil_mem_xoreq2);
	OP ("^=[4]", esil_mem_xoreq4);
	OP ("^=[8]", esil_mem_xoreq8);
	OP ("&=[]", esil_mem_andeq);
	OP ("&=[1]", esil_mem_andeq1);
	OP ("&=[2]", esil_mem_andeq2);
	OP ("&=[4]", esil_mem_andeq4);
	OP ("&=[8]", esil_mem_andeq8);
	OP ("+=[]", esil_mem_addeq);
	OP ("+=[1]", esil_mem_addeq1);
	OP ("+=[2]", esil_mem_addeq2);
	OP ("+=[4]", esil_mem_addeq4);
	OP ("+=[8]", esil_mem_addeq8);
	OP ("-=[]", esil_mem_subeq);
	OP ("-=[1]", esil_mem_subeq1);
	OP ("-=[2]", esil_mem_subeq2);
	OP ("-=[4]", esil_mem_subeq4);
	OP ("-=[8]", esil_mem_subeq8);
	OP ("%=[]", esil_mem_modeq);
	OP ("%=[1]", esil_mem_modeq1);
	OP ("%=[2]", esil_mem_modeq2);
	OP ("%=[4]", esil_mem_modeq4);
	OP ("%=[8]", esil_mem_modeq8);
	OP ("/=[]", esil_mem_diveq);
	OP ("/=[1]", esil_mem_diveq1);
	OP ("/=[2]", esil_mem_diveq2);
	OP ("/=[4]", esil_mem_diveq4);
	OP ("/=[8]", esil_mem_diveq8);
	OP ("*=[]", esil_mem_muleq);
	OP ("*=[1]", esil_mem_muleq1);
	OP ("*=[2]", esil_mem_muleq2);
	OP ("*=[4]", esil_mem_muleq4);
	OP ("*=[8]", esil_mem_muleq8);
	OP ("++=[]", esil_mem_inceq);
	OP ("++=[1]", esil_mem_inceq1);
	OP ("++=[2]", esil_mem_inceq2);
	OP ("++=[4]", esil_mem_inceq4);
	OP ("++=[8]", esil_mem_inceq8);
	OP ("--=[]", esil_mem_deceq);
	OP ("--=[1]", esil_mem_deceq1);
	OP ("--=[2]", esil_mem_deceq2);
	OP ("--=[4]", esil_mem_deceq4);
	OP ("--=[8]", esil_mem_deceq8);
	OP ("[]", esil_peek);
	OP ("[*]", esil_peek_some);
	OP ("=[*]", esil_poke_some);
	OP ("[1]", esil_peek1);
	OP ("[2]", esil_peek2);
	OP ("[4]", esil_peek4);
	OP ("[8]", esil_peek8);
	OP ("STACK", r_anal_esil_dumpstack);
	OP ("POP", esil_pop);
	OP ("TODO", esil_todo);
	OP ("GOTO", esil_goto);
	OP ("BREAK", esil_break);
	OP ("CLEAR", esil_clear);
	OP ("DUP", esil_dup);
}

/* register callbacks using this anal module. */
R_API int r_anal_esil_setup (RAnalEsil *esil, RAnal *anal, int romem, int stats) {
	if (!esil) return R_FALSE;
	//esil->debug = 0;
	esil->anal = anal;
	esil->parse_goto_count = anal->esil_goto_limit;
	esil->trap = 0;
	esil->trap_code = 0;
	//esil->user = NULL;

	esil->cb.reg_read = internal_esil_reg_read;
	esil->cb.reg_write = internal_esil_reg_write;
	esil->cb.mem_read = internal_esil_mem_read;
	esil->cb.mem_write = internal_esil_mem_write;

	r_anal_esil_mem_ro (esil, romem);
	r_anal_esil_stats (esil, stats);
	r_anal_esil_setup_ops (esil);

	if (anal->cur && anal->cur->esil_init && anal->cur->esil_fini) {
		return anal->cur->esil_init (esil);
	}
	return R_TRUE;
}
