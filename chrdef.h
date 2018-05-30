/*	$OpenBSD: chrdef.h,v 1.10 2015/03/25 12:29:03 bcallah Exp $	*/

/* This file is in the public domain. */

/*
 * sys/default/chardef.h: character set specific #defines for Mg 2a
 * Warning: System specific ones exist
 */

/*
 * Casting should be at least as efficient as anding with 0xff,
 * and won't have the size problems.
 */
#define	CHARMASK(c)	((unsigned char) (c))

/*
 * These flags, and the macros below them,
 * make up a do-it-yourself set of "ctype" macros that
 * understand the DEC multinational set, and let me ask
 * a slightly different set of questions.
 */
#define _MG_W	0x01		/* Word.			 */
#define _MG_U	0x02		/* Upper case letter.		 */
#define _MG_L	0x04		/* Lower case letter.		 */
#define _MG_C	0x08		/* Control.			 */
#define _MG_P	0x10		/* end of sentence punctuation	 */
#define	_MG_D	0x20		/* is decimal digit		 */

#define ISWORD(c)	((cinfo[CHARMASK(c)]&_MG_W)!=0)
#define ISCTRL(c)	((cinfo[CHARMASK(c)]&_MG_C)!=0)
#define ISUPPER(c)	((cinfo[CHARMASK(c)]&_MG_U)!=0)
#define ISLOWER(c)	((cinfo[CHARMASK(c)]&_MG_L)!=0)
#define ISEOSP(c)	((cinfo[CHARMASK(c)]&_MG_P)!=0)
#define	ISDIGIT(c)	((cinfo[CHARMASK(c)]&_MG_D)!=0)
#define TOUPPER(c)	((c)-0x20)
#define TOLOWER(c)	((c)+0x20)

/*
 * Generally useful thing for chars
 */
#define CCHR(x)		((x) ^ 0x40)	/* CCHR('?') == DEL */
