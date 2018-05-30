/*	$OpenBSD: cinfo.c,v 1.18 2015/03/19 21:22:15 bcallah Exp $	*/

/* This file is in the public domain. */

/*
 *		Character class tables.
 * Do it yourself character classification
 * macros, that understand the multinational character set,
 * and let me ask some questions the standard macros (in
 * ctype.h) don't let you ask.
 */

#include <ctype.h>
#include <sys/queue.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

#include "def.h"

/* Flag for treating '_' as word-like byte
 */
static int underscoreisword;

void
treatunderscoreasword(int i)
{
	underscoreisword = i;
}

/*
 * Find the name of a keystroke.  Needs to be changed to handle 8-bit printing
 * characters and function keys better.	 Returns a pointer to the terminating
 * '\0'.  Returns NULL on failure.
 */
char *
getkeyname(char *cp, size_t len, int k)
{
	const char	*np;
	size_t		 copied;

	if (k < 0)
		k = (unsigned char)(k);	/* sign extended char */
	switch (k) {
	case CCHR('@'):
		np = "C-SPC";
		break;
	case CCHR('I'):
		np = "TAB";
		break;
	case CCHR('M'):
		np = "RET";
		break;
	case CCHR('['):
		np = "ESC";
		break;
	case ' ':
		np = "SPC";
		break;		/* yuck again */
	case CCHR('?'):
		np = "DEL";
		break;
	default:
		if (k == 256) {
			np = NULL;
			break;
		} else if (k > CCHR('?')) {
			*cp++ = '0';
			*cp++ = ((k >> 6) & 7) + '0';
			*cp++ = ((k >> 3) & 7) + '0';
			*cp++ = (k & 7) + '0';
			*cp = '\0';
			return (cp);
		} else if (k < ' ') {
			*cp++ = 'C';
			*cp++ = '-';
			k = CCHR(k);
			if (isupper(k))
				k = tolower(k);
		}
		*cp++ = k;
		*cp = '\0';
		return (cp);
	}
	copied = strlcpy(cp, np, len);
	if (copied >= len)
		copied = len - 1;
	return (cp + copied);
}

/*
   Returns non-zero iff s[k] is a character (or the start of a
   multibyte character, we won't examine more than "len" bytes)
   which we consider to be "inside a word".  In practice, this means
   alphanumerics, "$", "%", "'". We'd also like to sometimes include
   "_", but that's rare enough that we'll handle it at the relevant
   call site.
 */
int
byteinword(const char *s, size_t k, size_t len) {
	if (s[k] == '$' || s[k] == '%' || s[k] == '\'') {
		return 1;
	} else if (s[k] == '_') {
		return underscoreisword;
	}

	mbstate_t mbs = { 0 };
	wchar_t wc = 0;
	size_t consumed = mbrtowc(&wc, &s[k], len, &mbs);

	if (consumed < (size_t) -2) {
		return iswalnum(wc);
	}

	/* If we're in byte-garbage: sure, whatever */
	return 1;
}
