/*	$OpenBSD: window.c,v 1.37 2023/03/08 04:43:11 guenther Exp $	*/

/* This file is in the public domain. */

/*
 *		Window handling.
 */

#include <sys/queue.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#include "def.h"

struct mgwin *
new_window(struct buffer *bp)
{
	struct mgwin *wp;

	wp = calloc(1, sizeof(struct mgwin));
	if (wp == NULL)
		return (NULL);

	wp->w_bufp = bp;
	wp->w_dotp = NULL;
	wp->w_doto = 0;
	wp->w_markp = NULL;
	wp->w_marko = 0;
	wp->w_rflag = 0;
	wp->w_frame = 0;
	wp->w_leftcol = 0;
	wp->w_ncols = ncol;
	wp->w_lbound = 0;
	wp->w_wrapline = NULL;
	wp->w_dotline = wp->w_markline = 1;
	if (bp)
		bp->b_nwnd++;
	return (wp);
}

/*
 * Reposition dot in the current window to line "n".  If the argument is
 * positive, it is that line.  If it is negative it is that line from the
 * bottom.  If it is 0 the window is centered (this is what the standard
 * redisplay code does).
 */
int
reposition(int f, int n)
{
	curwp->w_frame = (f & FFARG) ? (n >= 0 ? n + 1 : n) : 0;
	curwp->w_rflag |= WFFRAME;
	sgarbf = TRUE;
	return (TRUE);
}

/*
 * Minimum text columns per window after a vertical (side-by-side) split.
 * The divider column itself is not counted, so two windows split from one
 * need a parent at least 2*MIN_WCOLS + 1 columns wide.
 */
#define MIN_WCOLS	8

/*
 * Proportionally rescale every window's column geometry when the terminal
 * width changes and vertical splits exist. Walks the unique divider
 * positions in the current layout, maps each one to a new column via
 * (D * newncol / oldncol), then re-derives every window's w_leftcol and
 * w_ncols from those mappings. This preserves the invariant that two
 * windows sharing an edge in the old layout still share the same edge
 * in the new layout (so the divider stays a single column).
 *
 * If a band would end up narrower than MIN_WCOLS or dividers would cross,
 * falls back to collapsing all vertical splits onto the full new width.
 */
static void
rescale_columns(int oldncol, int newncol)
{
	struct mgwin	*wp;
	int		 dividers[64], newdivs[64];
	int		 nd = 0;
	int		 i, j, key;

	/* Gather unique old divider column positions (w_leftcol - 1). */
	for (wp = wheadp; wp != NULL; wp = wp->w_wndp) {
		if (wp->w_leftcol > 0) {
			int dup = FALSE, d = wp->w_leftcol - 1;

			for (i = 0; i < nd; i++)
				if (dividers[i] == d) {
					dup = TRUE;
					break;
				}
			if (!dup && nd < (int)(sizeof(dividers) /
			    sizeof(dividers[0])))
				dividers[nd++] = d;
		}
	}

	/* Insertion sort — nd is tiny. */
	for (i = 1; i < nd; i++) {
		key = dividers[i];
		for (j = i - 1; j >= 0 && dividers[j] > key; j--)
			dividers[j+1] = dividers[j];
		dividers[j+1] = key;
	}

	/*
	 * Map each old divider position to a new one, proportionally with
	 * round-to-nearest. Truncation produces a 1-column drift on every
	 * round trip when the user drags the terminal back and forth.
	 */
	for (i = 0; i < nd; i++)
		newdivs[i] = (int)(((long long)dividers[i] * newncol +
		    oldncol / 2) / oldncol);

	/*
	 * Validate: every band on either side of every divider must have
	 * at least MIN_WCOLS columns of text. If not, the new width can't
	 * accommodate the layout — collapse all vertical splits cleanly
	 * to a single full-width column instead of producing tiny windows.
	 */
	for (i = 0; i < nd; i++) {
		int leftbound = (i == 0) ? 0 : newdivs[i-1] + 1;
		int rightbound = (i == nd - 1) ? newncol : newdivs[i+1];

		if (newdivs[i] - leftbound < MIN_WCOLS ||
		    rightbound - newdivs[i] - 1 < MIN_WCOLS) {
			for (wp = wheadp; wp != NULL; wp = wp->w_wndp) {
				wp->w_leftcol = 0;
				wp->w_ncols = newncol;
				wp->w_lbound = 0;
			}
			return;
		}
	}

	/* Apply the mapping to every window. */
	for (wp = wheadp; wp != NULL; wp = wp->w_wndp) {
		int new_left = 0;
		int new_right = newncol;
		int old_right = wp->w_leftcol + wp->w_ncols;

		if (wp->w_leftcol > 0) {
			for (i = 0; i < nd; i++) {
				if (dividers[i] == wp->w_leftcol - 1) {
					new_left = newdivs[i] + 1;
					break;
				}
			}
		}
		if (old_right < oldncol) {
			for (i = 0; i < nd; i++) {
				if (dividers[i] == old_right) {
					new_right = newdivs[i];
					break;
				}
			}
		}
		wp->w_leftcol = new_left;
		wp->w_ncols = new_right - new_left;
		wp->w_lbound = 0;
	}
}

/*
 * Refresh the display.  A call is made to the "ttresize" entry in the
 * terminal handler, which tries to reset "nrow" and "ncol".  They will,
 * however, never be set outside of the NROW or NCOL range.  If the display
 * changed size, arrange that everything is redone, then call "update" to
 * fix the display.  We do this so the new size can be displayed.  In the
 * normal case the call to "update" in "main.c" refreshes the screen, and
 * all of the windows need not be recomputed. This call includes a
 * 'force' parameter to ensure that the redraw is done, even after a
 * a suspend/continue (where the window size parameters will already
 * be updated). Note that when you get to the "display unusable"
 * message, the screen will be messed up. If you make the window bigger
 * again, and send another command, everything will get fixed!
 */
int
redraw(int f, int n)
{
	return (do_redraw(f, n, FALSE));
}

/*
 * Demote every window except curwp into oblivion, then resize curwp to
 * the full new (nrow, ncol). Used as the bailout path when the new
 * terminal geometry can no longer accommodate the current layout —
 * something like onlywind(), but safe to call from inside do_redraw
 * (does not consult anything beyond curwp / curbp).
 */
static void
collapse_to_curwp(void)
{
	struct mgwin	*wp;

	while (wheadp != curwp) {
		wp = wheadp;
		wheadp = wp->w_wndp;
		if (--wp->w_bufp->b_nwnd == 0) {
			wp->w_bufp->b_dotp = wp->w_dotp;
			wp->w_bufp->b_doto = wp->w_doto;
			wp->w_bufp->b_markp = wp->w_markp;
			wp->w_bufp->b_marko = wp->w_marko;
			wp->w_bufp->b_dotline = wp->w_dotline;
			wp->w_bufp->b_markline = wp->w_markline;
		}
		free(wp);
	}
	while (curwp->w_wndp != NULL) {
		wp = curwp->w_wndp;
		curwp->w_wndp = wp->w_wndp;
		if (--wp->w_bufp->b_nwnd == 0) {
			wp->w_bufp->b_dotp = wp->w_dotp;
			wp->w_bufp->b_doto = wp->w_doto;
			wp->w_bufp->b_markp = wp->w_markp;
			wp->w_bufp->b_marko = wp->w_marko;
			wp->w_bufp->b_dotline = wp->w_dotline;
			wp->w_bufp->b_markline = wp->w_markline;
		}
		free(wp);
	}
	curwp->w_toprow = 0;
	curwp->w_leftcol = 0;
	curwp->w_ncols = ncol;
	curwp->w_lbound = 0;
	curwp->w_ntrows = nrow - 2;
	if (curwp->w_ntrows < 1)
		curwp->w_ntrows = 1;
	curwp->w_rflag |= WFMODE | WFFULL;
}

/*
 * For each unique column band (windows sharing a w_leftcol),
 * proportionally rescale every window in the band so the band fills
 * from its first window's toprow down to row newnrow - 2 (just above
 * the echo line). Walks each band top-to-bottom, sorts windows by
 * w_toprow, recomputes each window's w_ntrows in proportion to its
 * old text-row share, and reassigns w_toprow to keep the band
 * contiguous.
 *
 * Returns FALSE if any band can't fit at least 1 text row + 1
 * modeline row per window. Caller is expected to collapse_to_curwp()
 * on failure.
 */
static int
adjust_row_per_band(int newnrow)
{
	struct mgwin	*wp, *o, *tmp, *band[64];
	int		 seen[64];
	int		 nseen = 0;
	int		 i, j, k, nband, dup;

	for (wp = wheadp; wp != NULL; wp = wp->w_wndp) {
		int my_leftcol = wp->w_leftcol;
		int oldbottom, oldfirsttop, oldbandheight, newbandheight;
		int old_total_text, cur_toprow;
		int newntrows;

		dup = FALSE;
		for (i = 0; i < nseen; i++) {
			if (seen[i] == my_leftcol) {
				dup = TRUE;
				break;
			}
		}
		if (dup)
			continue;
		if (nseen < (int)(sizeof(seen) / sizeof(seen[0])))
			seen[nseen++] = my_leftcol;

		/* Collect this band's windows. */
		nband = 0;
		for (o = wheadp; o != NULL; o = o->w_wndp) {
			if (o->w_leftcol == my_leftcol &&
			    nband < (int)(sizeof(band) / sizeof(band[0])))
				band[nband++] = o;
		}

		/* Insertion sort by w_toprow. */
		for (j = 1; j < nband; j++) {
			tmp = band[j];
			for (k = j - 1;
			    k >= 0 && band[k]->w_toprow > tmp->w_toprow; k--)
				band[k+1] = band[k];
			band[k+1] = tmp;
		}

		oldfirsttop = band[0]->w_toprow;
		oldbottom = band[nband-1]->w_toprow +
		    band[nband-1]->w_ntrows;
		/* +1 to include the bottom modeline row */
		oldbandheight = oldbottom - oldfirsttop + 1;

		/*
		 * The band keeps its top edge where it was (other bands
		 * may also start there; rearranging across bands is more
		 * than this function tackles). It must now end at row
		 * newnrow - 2 (modeline of last window), so the new
		 * band height is newnrow - 1 - oldfirsttop.
		 */
		newbandheight = newnrow - 1 - oldfirsttop;
		if (newbandheight < 2 * nband)
			return (FALSE);

		/*
		 * Each window contributes 1 modeline + at least 1 text row.
		 * Distribute the remaining text rows proportionally to the
		 * old text-row counts.
		 */
		old_total_text = oldbandheight - nband; /* subtract modelines */
		if (old_total_text < nband)
			old_total_text = nband;

		cur_toprow = oldfirsttop;
		for (j = 0; j < nband; j++) {
			if (j == nband - 1) {
				/* last window takes whatever's left */
				newntrows = newnrow - 1 - cur_toprow - 1;
			} else {
				newntrows = (int)(((long long)
				    band[j]->w_ntrows *
				    (newbandheight - nband) +
				    old_total_text / 2) / old_total_text);
				if (newntrows < 1)
					newntrows = 1;
			}
			if (newntrows < 1)
				return (FALSE);
			band[j]->w_toprow = cur_toprow;
			band[j]->w_ntrows = newntrows;
			band[j]->w_rflag |= WFMODE | WFFULL;
			cur_toprow += newntrows + 1; /* +1 for modeline */
		}
	}
	return (TRUE);
}

int
do_redraw(int f, int n, int force)
{
	struct mgwin	*wp;
	int		 oldnrow, oldncol;
	int		 has_vsplit;

	oldnrow = nrow;
	oldncol = ncol;
	ttresize();
	if (nrow == oldnrow && ncol == oldncol && !force) {
		sgarbf = TRUE;
		return (TRUE);
	}

	/* If the new screen is entirely unusable, collapse and beep. */
	if (nrow < 3) {
		collapse_to_curwp();
		dobeep();
		ewprintf("Display unusable");
		sgarbf = TRUE;
		update(CMODE);
		return (FALSE);
	}

	has_vsplit = FALSE;
	for (wp = wheadp; wp != NULL; wp = wp->w_wndp) {
		if (wp->w_leftcol > 0 || wp->w_ncols != oldncol) {
			has_vsplit = TRUE;
			break;
		}
	}

	/* Column-axis adjustment. */
	if (!has_vsplit) {
		/* No vertical splits: every window owns the full width. */
		for (wp = wheadp; wp != NULL; wp = wp->w_wndp) {
			wp->w_leftcol = 0;
			wp->w_ncols = ncol;
			wp->w_lbound = 0;
		}
	} else if (ncol != oldncol) {
		/* Have vsplits and width changed: rescale proportionally. */
		rescale_columns(oldncol, ncol);
	}
	/* Have vsplits, width unchanged: leave column geometry alone. */

	/*
	 * Row-axis adjustment. With vertical splits a flat last-window
	 * resize would leave parallel bands hanging off the screen, so
	 * grow/shrink each column band's bottommost window instead.
	 */
	if (!adjust_row_per_band(nrow))
		collapse_to_curwp();

	sgarbf = TRUE;
	update(CMODE);
	return (TRUE);
}

/*
 * The command to make the next window (next => down the screen) the current
 * window. There are no real errors, although the command does nothing if
 * there is only 1 window on the screen.
 */
int
nextwind(int f, int n)
{
	struct mgwin	*wp;

	if ((wp = curwp->w_wndp) == NULL)
		wp = wheadp;
	curwp = wp;
	curbp = wp->w_bufp;
	return (TRUE);
}

/* not in GNU Emacs */
/*
 * This command makes the previous window (previous => up the screen) the
 * current window. There are no errors, although the command does not do
 * a lot if there is only 1 window.
 */
int
prevwind(int f, int n)
{
	struct mgwin	*wp1, *wp2;

	wp1 = wheadp;
	wp2 = curwp;
	if (wp1 == wp2)
		wp2 = NULL;
	while (wp1->w_wndp != wp2)
		wp1 = wp1->w_wndp;
	curwp = wp1;
	curbp = wp1->w_bufp;
	return (TRUE);
}

/*
 * This command makes the current window the only window on the screen.  Try
 * to set the framing so that "." does not have to move on the display.  Some
 * care has to be taken to keep the values of dot and mark in the buffer
 * structures right if the destruction of a window makes a buffer become
 * undisplayed.
 */
int
onlywind(int f, int n)
{
	struct mgwin	*wp;
	struct line	*lp;
	int		 i;

	while (wheadp != curwp) {
		wp = wheadp;
		wheadp = wp->w_wndp;
		if (--wp->w_bufp->b_nwnd == 0) {
			wp->w_bufp->b_dotp = wp->w_dotp;
			wp->w_bufp->b_doto = wp->w_doto;
			wp->w_bufp->b_markp = wp->w_markp;
			wp->w_bufp->b_marko = wp->w_marko;
			wp->w_bufp->b_dotline = wp->w_dotline;
			wp->w_bufp->b_markline = wp->w_markline;
		}
		free(wp);
	}
	while (curwp->w_wndp != NULL) {
		wp = curwp->w_wndp;
		curwp->w_wndp = wp->w_wndp;
		if (--wp->w_bufp->b_nwnd == 0) {
			wp->w_bufp->b_dotp = wp->w_dotp;
			wp->w_bufp->b_doto = wp->w_doto;
			wp->w_bufp->b_markp = wp->w_markp;
			wp->w_bufp->b_marko = wp->w_marko;
			wp->w_bufp->b_dotline = wp->w_dotline;
			wp->w_bufp->b_markline = wp->w_markline;
		}
		free(wp);
	}
	lp = curwp->w_linep;
	i = curwp->w_toprow;
	while (i != 0 && lback(lp) != curbp->b_headp) {
		--i;
		lp = lback(lp);
	}
	curwp->w_toprow = 0;

	/* 2 = mode, echo */
	curwp->w_ntrows = nrow - 2;
	curwp->w_linep = lp;
	curwp->w_rflag |= WFMODE | WFFULL;
	return (TRUE);
}

/*
 * Split the current window.  A window smaller than 3 lines cannot be split.
 * The only other error that is possible is a "malloc" failure allocating the
 * structure for the new window.
 * If called with a FFOTHARG, flags on the new window are set to 'n'.
 */
int
splitwind(int f, int n)
{
	struct mgwin	*wp, *wp1, *wp2;
	struct line	*lp;
	int		 ntru, ntrd, ntrl;

	if (curwp->w_ntrows < 3) {
		dobeep();
		ewprintf("Cannot split a %d line window", curwp->w_ntrows);
		return (FALSE);
	}
	wp = new_window(curbp);
	if (wp == NULL) {
		dobeep();
		ewprintf("Unable to create a window");
		return (FALSE);
	}

	/* use the current dot and mark */
	wp->w_dotp = curwp->w_dotp;
	wp->w_doto = curwp->w_doto;
	wp->w_markp = curwp->w_markp;
	wp->w_marko = curwp->w_marko;
	wp->w_dotline = curwp->w_dotline;
	wp->w_markline = curwp->w_markline;

	/* horizontal split: inherit column geometry from the parent */
	wp->w_leftcol = curwp->w_leftcol;
	wp->w_ncols = curwp->w_ncols;
	wp->w_lbound = 0;

	/* figure out which half of the screen we're in */
	ntru = (curwp->w_ntrows - 1) / 2;	/* Upper size */
	ntrl = (curwp->w_ntrows - 1) - ntru;	/* Lower size */

	for (lp = curwp->w_linep, ntrd = 0; lp != curwp->w_dotp;
	    lp = lforw(lp))
		ntrd++;

	lp = curwp->w_linep;

	/* old is upper window */
	if (ntrd <= ntru) {
		/* hit mode line */
		if (ntrd == ntru)
			lp = lforw(lp);
		curwp->w_ntrows = ntru;
		wp->w_wndp = curwp->w_wndp;
		curwp->w_wndp = wp;
		wp->w_toprow = curwp->w_toprow + ntru + 1;
		wp->w_ntrows = ntrl;
	/* old is lower window */
	} else {
		wp1 = NULL;
		wp2 = wheadp;
		while (wp2 != curwp) {
			wp1 = wp2;
			wp2 = wp2->w_wndp;
		}
		if (wp1 == NULL)
			wheadp = wp;
		else
			wp1->w_wndp = wp;
		wp->w_wndp = curwp;
		wp->w_toprow = curwp->w_toprow;
		wp->w_ntrows = ntru;

		/* mode line */
		++ntru;
		curwp->w_toprow += ntru;
		curwp->w_ntrows = ntrl;
		while (ntru--)
			lp = lforw(lp);
	}

	/* adjust the top lines if necessary */
	curwp->w_linep = lp;
	wp->w_linep = lp;

	curwp->w_rflag |= WFMODE | WFFULL;
	wp->w_rflag |= WFMODE | WFFULL;
	/* if FFOTHARG, set flags) */
	if (f & FFOTHARG)
		wp->w_flag = n;

	return (TRUE);
}

/*
 * Split the current window vertically (i.e. produce a left/right pair with
 * a one-column "|" divider in between). Mirrors splitwind() but acts on the
 * column axis. The new window is inserted in the window list immediately
 * after the current one and shows the same buffer at the same dot/mark; the
 * original window remains current and keeps the left half.
 */
int
splitwind_horiz(int f, int n)
{
	struct mgwin	*wp;
	int		 ntrleft, ntrright;

	if (curwp->w_ncols < 2 * MIN_WCOLS + 1) {
		dobeep();
		ewprintf("Cannot split a %d column window", curwp->w_ncols);
		return (FALSE);
	}
	wp = new_window(curbp);
	if (wp == NULL) {
		dobeep();
		ewprintf("Unable to create a window");
		return (FALSE);
	}

	/* share dot and mark with the original window */
	wp->w_dotp = curwp->w_dotp;
	wp->w_doto = curwp->w_doto;
	wp->w_markp = curwp->w_markp;
	wp->w_marko = curwp->w_marko;
	wp->w_dotline = curwp->w_dotline;
	wp->w_markline = curwp->w_markline;
	wp->w_linep = curwp->w_linep;

	/*
	 * Carve out columns: subtract one for the divider, then split what
	 * is left as evenly as possible, giving any odd column to the left
	 * (original) half.
	 */
	ntrleft = (curwp->w_ncols - 1) / 2 + ((curwp->w_ncols - 1) % 2);
	ntrright = curwp->w_ncols - 1 - ntrleft;

	/* new window owns the right half */
	wp->w_toprow = curwp->w_toprow;
	wp->w_ntrows = curwp->w_ntrows;
	wp->w_leftcol = curwp->w_leftcol + ntrleft + 1;
	wp->w_ncols = ntrright;
	wp->w_lbound = 0;

	/* original window keeps the left half */
	curwp->w_ncols = ntrleft;
	curwp->w_lbound = 0;

	/* insert the new window into the list directly after curwp */
	wp->w_wndp = curwp->w_wndp;
	curwp->w_wndp = wp;

	/* both halves need a full redraw including mode line */
	curwp->w_rflag |= WFMODE | WFFULL;
	wp->w_rflag |= WFMODE | WFFULL;

	/* if FFOTHARG, set flags on the new window */
	if (f & FFOTHARG)
		wp->w_flag = n;

	/* force a hard refresh so the divider column is painted cleanly */
	sgarbf = TRUE;

	return (TRUE);
}

/*
 * Enlarge the current window.  Find the window that loses space.  Make sure
 * it is big enough.  If so, hack the window descriptions, and ask redisplay
 * to do all the hard work.  You don't just set "force reframe" because dot
 * would move.
 */
int
enlargewind(int f, int n)
{
	struct mgwin	*adjwp;
	struct line	*lp;
	int		 i;

	if (n < 0)
		return (shrinkwind(f, -n));
	if (wheadp->w_wndp == NULL) {
		dobeep();
		ewprintf("Only one window");
		return (FALSE);
	}
	if ((adjwp = curwp->w_wndp) == NULL) {
		adjwp = wheadp;
		while (adjwp->w_wndp != curwp)
			adjwp = adjwp->w_wndp;
	}
	if (adjwp->w_ntrows <= n) {
		dobeep();
		ewprintf("Impossible change");
		return (FALSE);
	}

	/* shrink below */
	if (curwp->w_wndp == adjwp) {
		lp = adjwp->w_linep;
		for (i = 0; i < n && lp != adjwp->w_bufp->b_headp; ++i)
			lp = lforw(lp);
		adjwp->w_linep = lp;
		adjwp->w_toprow += n;
	/* shrink above */
	} else {
		lp = curwp->w_linep;
		for (i = 0; i < n && lback(lp) != curbp->b_headp; ++i)
			lp = lback(lp);
		curwp->w_linep = lp;
		curwp->w_toprow -= n;
	}
	curwp->w_ntrows += n;
	adjwp->w_ntrows -= n;
	curwp->w_rflag |= WFMODE | WFFULL;
	adjwp->w_rflag |= WFMODE | WFFULL;
	return (TRUE);
}

/*
 * Shrink the current window.  Find the window that gains space.  Hack at the
 * window descriptions. Ask the redisplay to do all the hard work.
 */
int
shrinkwind(int f, int n)
{
	struct mgwin	*adjwp;
	struct line	*lp;
	int		 i;

	if (n < 0)
		return (enlargewind(f, -n));
	if (wheadp->w_wndp == NULL) {
		dobeep();
		ewprintf("Only one window");
		return (FALSE);
	}
	/*
	 * Bit of flakiness - FFRAND means it was an internal call, and
	 * to be trusted implicitly about sizes.
	 */
	if (!(f & FFRAND) && curwp->w_ntrows <= n) {
		dobeep();
		ewprintf("Impossible change");
		return (FALSE);
	}
	if ((adjwp = curwp->w_wndp) == NULL) {
		adjwp = wheadp;
		while (adjwp->w_wndp != curwp)
			adjwp = adjwp->w_wndp;
	}

	/* grow below */
	if (curwp->w_wndp == adjwp) {
		lp = adjwp->w_linep;
		for (i = 0; i < n && lback(lp) != adjwp->w_bufp->b_headp; ++i)
			lp = lback(lp);
		adjwp->w_linep = lp;
		adjwp->w_toprow -= n;
	/* grow above */
	} else {
		lp = curwp->w_linep;
		for (i = 0; i < n && lp != curbp->b_headp; ++i)
			lp = lforw(lp);
		curwp->w_linep = lp;
		curwp->w_toprow += n;
	}
	curwp->w_ntrows -= n;
	adjwp->w_ntrows += n;
	curwp->w_rflag |= WFMODE | WFFULL;
	adjwp->w_rflag |= WFMODE | WFFULL;
	return (TRUE);
}

/*
 * Delete current window. Call shrink-window to do the screen updating, then
 * throw away the window.
 */
int
delwind(int f, int n)
{
	struct mgwin	*wp, *nwp;

	wp = curwp;		/* Cheap...		 */

	/* shrinkwind returning false means only one window... */
	if (shrinkwind(FFRAND, wp->w_ntrows + 1) == FALSE)
		return (FALSE);
	if (--wp->w_bufp->b_nwnd == 0) {
		wp->w_bufp->b_dotp = wp->w_dotp;
		wp->w_bufp->b_doto = wp->w_doto;
		wp->w_bufp->b_markp = wp->w_markp;
		wp->w_bufp->b_marko = wp->w_marko;
		wp->w_bufp->b_dotline = wp->w_dotline;
		wp->w_bufp->b_markline = wp->w_markline;
	}

	/* since shrinkwind did't crap out, we know we have a second window */
	if (wp == wheadp)
		wheadp = curwp = wp->w_wndp;
	else if ((curwp = wp->w_wndp) == NULL)
		curwp = wheadp;
	curbp = curwp->w_bufp;
	for (nwp = wheadp; nwp != NULL; nwp = nwp->w_wndp)
		if (nwp->w_wndp == wp) {
			nwp->w_wndp = wp->w_wndp;
			break;
		}
	free(wp);
	return (TRUE);
}
