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
 * Proportionally rescale every window's column geometry to a new terminal
 * width using each band's STABLE logical weight (w_propw). Because propw
 * never changes during SIGWINCH-driven resizes, the result of "rescale
 * 80 -> 200 in one shot" equals the result of "80 -> 81 -> ... -> 200 in
 * many tiny steps". This matches how GNU Emacs distributes a frame resize:
 * each window's logical share of the parent stays fixed; only the pixel
 * projection of that share changes.
 *
 * Algorithm (largest-remainder / Hamilton method):
 *   1. Identify unique bands (windows sharing a w_leftcol). Sort left-to-
 *      right by w_leftcol.
 *   2. Total propw = sum of unique band weights. If any band has propw=0
 *      (legacy or unset), bootstrap from its current w_ncols.
 *   3. usable_new = newncol - (nbands - 1) divider columns.
 *   4. Floor allocation per band: floor(propw * usable_new / total_propw).
 *      Clamp to MIN_WCOLS.
 *   5. Distribute leftover columns to bands with the largest fractional
 *      remainders, breaking ties by giving extra to the band with the
 *      smallest current allocation (so growth is symmetric on round
 *      trips and not biased to one side).
 *   6. Lay out left-to-right with one-column dividers and propagate to
 *      every window in each band.
 *
 * If usable_new can't accommodate nbands * MIN_WCOLS, collapses all
 * vertical splits to a single full-width column.
 */
static void
rescale_columns(int oldncol, int newncol)
{
	struct mgwin	*wp;
	int		 band_leftcol_old[64];
	int		 band_propw[64];
	int		 band_oldw[64];
	int		 band_neww[64];
	int		 band_newleft[64];
	int		 order[64];
	long long	 frac[64];
	int		 nbands = 0;
	int		 i, j;
	int		 usable_new, total_propw, used, rem;

	(void)oldncol;

	/* Step 1: identify unique bands by w_leftcol. */
	for (wp = wheadp; wp != NULL; wp = wp->w_wndp) {
		int dup = FALSE;

		for (i = 0; i < nbands; i++)
			if (band_leftcol_old[i] == wp->w_leftcol) {
				dup = TRUE;
				break;
			}
		if (!dup && nbands < (int)(sizeof(band_leftcol_old) /
		    sizeof(band_leftcol_old[0]))) {
			band_leftcol_old[nbands] = wp->w_leftcol;
			band_propw[nbands] = wp->w_propw;
			band_oldw[nbands] = wp->w_ncols;
			nbands++;
		}
	}

	/* Sort bands left-to-right. */
	for (i = 1; i < nbands; i++) {
		int kl = band_leftcol_old[i];
		int kp = band_propw[i];
		int kw = band_oldw[i];

		for (j = i - 1; j >= 0 && band_leftcol_old[j] > kl; j--) {
			band_leftcol_old[j+1] = band_leftcol_old[j];
			band_propw[j+1] = band_propw[j];
			band_oldw[j+1] = band_oldw[j];
		}
		band_leftcol_old[j+1] = kl;
		band_propw[j+1] = kp;
		band_oldw[j+1] = kw;
	}

	if (nbands <= 1) {
		for (wp = wheadp; wp != NULL; wp = wp->w_wndp) {
			wp->w_leftcol = 0;
			wp->w_ncols = newncol;
			wp->w_lbound = 0;
		}
		return;
	}

	/*
	 * Step 2: bootstrap propw if missing. Pre-existing layouts and
	 * any band that lost its weight tracking get a weight derived
	 * from current pixel width (one-time migration).
	 */
	total_propw = 0;
	for (i = 0; i < nbands; i++) {
		if (band_propw[i] <= 0)
			band_propw[i] = band_oldw[i] > 0 ? band_oldw[i] : 1;
		total_propw += band_propw[i];
	}
	if (total_propw <= 0)
		total_propw = 1;

	usable_new = newncol - (nbands - 1);
	if (usable_new < nbands * MIN_WCOLS) {
		for (wp = wheadp; wp != NULL; wp = wp->w_wndp) {
			wp->w_leftcol = 0;
			wp->w_ncols = newncol;
			wp->w_lbound = 0;
		}
		return;
	}

	/* Step 4: floor allocation from propw. */
	used = 0;
	for (i = 0; i < nbands; i++) {
		long long scaled = (long long)band_propw[i] * usable_new;
		long long flr = scaled / total_propw;

		band_neww[i] = (int)flr;
		if (band_neww[i] < MIN_WCOLS)
			band_neww[i] = MIN_WCOLS;
		frac[i] = scaled - flr * total_propw;
		order[i] = i;
		used += band_neww[i];
	}

	/*
	 * Step 5: order bands for leftover distribution. Primary key:
	 * descending fractional remainder. Tie-break: ascending current
	 * allocation (smaller band wins, so growth is even on round trips).
	 */
	for (i = 1; i < nbands; i++) {
		int oi = order[i];

		for (j = i - 1; j >= 0; j--) {
			int oj = order[j];

			if (frac[oj] > frac[oi])
				break;
			if (frac[oj] == frac[oi] &&
			    band_neww[oj] <= band_neww[oi])
				break;
			order[j+1] = order[j];
		}
		order[j+1] = oi;
	}

	rem = usable_new - used;
	if (rem > 0) {
		for (i = 0; i < rem; i++)
			band_neww[order[i % nbands]]++;
	} else if (rem < 0) {
		int need = -rem;

		for (i = nbands - 1; i >= 0 && need > 0; i--) {
			int oi = order[i];
			int can = band_neww[oi] - MIN_WCOLS;

			if (can <= 0)
				continue;
			if (can > need)
				can = need;
			band_neww[oi] -= can;
			need -= can;
		}
		if (need > 0) {
			for (wp = wheadp; wp != NULL; wp = wp->w_wndp) {
				wp->w_leftcol = 0;
				wp->w_ncols = newncol;
				wp->w_lbound = 0;
			}
			return;
		}
	}

	/* Step 6: assign new leftcols left-to-right with divider columns. */
	{
		int cursor = 0;

		for (i = 0; i < nbands; i++) {
			band_newleft[i] = cursor;
			cursor += band_neww[i] + 1;
		}
	}

	for (wp = wheadp; wp != NULL; wp = wp->w_wndp) {
		for (i = 0; i < nbands; i++) {
			if (band_leftcol_old[i] == wp->w_leftcol) {
				wp->w_leftcol = band_newleft[i];
				wp->w_ncols = band_neww[i];
				wp->w_lbound = 0;
				/* keep propw in sync if it was bootstrapped */
				wp->w_propw = band_propw[i];
				break;
			}
		}
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
	curwp->w_propw = 10000;
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
	curwp->w_leftcol = 0;
	curwp->w_ncols = ncol;
	curwp->w_lbound = 0;
	curwp->w_propw = 10000;
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

	/* horizontal split: inherit column geometry and band weight */
	wp->w_leftcol = curwp->w_leftcol;
	wp->w_ncols = curwp->w_ncols;
	wp->w_lbound = 0;
	wp->w_propw = curwp->w_propw;

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

	/*
	 * Split the parent's logical band weight evenly between the two
	 * new bands. This weight is preserved across SIGWINCH resizes so
	 * the two bands always grow/shrink in equal proportion.
	 */
	{
		int parent_w = curwp->w_propw;
		int half;

		if (parent_w <= 0)
			parent_w = 10000;
		half = parent_w / 2;
		curwp->w_propw = half;
		wp->w_propw = parent_w - half;
	}

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
 * Shared band-resize worker for horizontal enlarge/shrink. A positive
 * delta grows curwp's column band by that many text columns; a negative
 * delta shrinks it. The adjacent band (right neighbor preferred, falling
 * back to left) gives up or gains the corresponding columns.
 *
 * All windows in curwp's band update uniformly, and likewise for the
 * neighbor band, so the layout stays consistent. Bands are keyed by
 * (w_leftcol, w_ncols). The deleted/granted column count is folded into
 * each band's w_propw so future SIGWINCH resizes keep the new
 * proportions.
 *
 * Refuses if either band would drop below MIN_WCOLS, or if the neighbor
 * candidates don't share a consistent (leftcol, ncols).
 */
static int
resize_horiz(int delta)
{
	struct mgwin	*iter;
	struct mgwin	*nbr[64];
	int		 n_nbr = 0;
	int		 max_nbr = (int)(sizeof(nbr) / sizeof(nbr[0]));
	int		 cur_left = curwp->w_leftcol;
	int		 cur_ncols = curwp->w_ncols;
	int		 cur_right = cur_left + cur_ncols;
	int		 nbr_left, nbr_ncols;
	int		 neighbor_on_right;
	int		 cur_propw_old, nbr_propw_old;
	int		 total_propw, total_cols;
	int		 new_cur_propw, new_nbr_propw;
	int		 j;

	if (delta == 0)
		return (TRUE);

	for (iter = wheadp; iter != NULL; iter = iter->w_wndp) {
		if (iter == curwp)
			continue;
		if (iter->w_leftcol == cur_right + 1 && n_nbr < max_nbr)
			nbr[n_nbr++] = iter;
	}
	neighbor_on_right = (n_nbr > 0);
	if (n_nbr == 0) {
		for (iter = wheadp; iter != NULL; iter = iter->w_wndp) {
			if (iter == curwp)
				continue;
			if (iter->w_leftcol + iter->w_ncols + 1 == cur_left &&
			    n_nbr < max_nbr)
				nbr[n_nbr++] = iter;
		}
	}
	if (n_nbr == 0) {
		dobeep();
		ewprintf("No adjacent column band");
		return (FALSE);
	}

	nbr_left = nbr[0]->w_leftcol;
	nbr_ncols = nbr[0]->w_ncols;
	for (j = 1; j < n_nbr; j++) {
		if (nbr[j]->w_leftcol != nbr_left ||
		    nbr[j]->w_ncols != nbr_ncols) {
			dobeep();
			ewprintf("Adjacent layout inconsistent");
			return (FALSE);
		}
	}

	if (delta > 0) {
		if (nbr_ncols - delta < MIN_WCOLS) {
			dobeep();
			ewprintf("Impossible change");
			return (FALSE);
		}
	} else {
		if (cur_ncols + delta < MIN_WCOLS) {
			dobeep();
			ewprintf("Impossible change");
			return (FALSE);
		}
	}

	cur_propw_old = curwp->w_propw;
	nbr_propw_old = nbr[0]->w_propw;
	total_propw = cur_propw_old + nbr_propw_old;
	total_cols = cur_ncols + nbr_ncols;
	if (total_cols < 1)
		total_cols = 1;
	new_cur_propw = (int)(((long long)total_propw *
	    (cur_ncols + delta) + total_cols / 2) / total_cols);
	new_nbr_propw = total_propw - new_cur_propw;
	if (new_cur_propw < 1)
		new_cur_propw = 1;
	if (new_nbr_propw < 1)
		new_nbr_propw = 1;

	for (iter = wheadp; iter != NULL; iter = iter->w_wndp) {
		if (iter->w_leftcol == cur_left &&
		    iter->w_ncols == cur_ncols) {
			iter->w_ncols += delta;
			iter->w_propw = new_cur_propw;
			if (!neighbor_on_right)
				iter->w_leftcol -= delta;
			iter->w_lbound = 0;
			iter->w_rflag |= WFMODE | WFFULL;
		}
	}
	for (iter = wheadp; iter != NULL; iter = iter->w_wndp) {
		if (iter->w_leftcol == nbr_left &&
		    iter->w_ncols == nbr_ncols) {
			iter->w_ncols -= delta;
			iter->w_propw = new_nbr_propw;
			if (neighbor_on_right)
				iter->w_leftcol += delta;
			iter->w_lbound = 0;
			iter->w_rflag |= WFMODE | WFFULL;
		}
	}

	sgarbf = TRUE;
	return (TRUE);
}

/*
 * enlarge-window-horizontally: grow the current window's column band by
 * n columns at the expense of an adjacent band. Defaults to growing
 * rightward; with no right neighbor, grows leftward.
 */
int
enlargewind_horiz(int f, int n)
{
	if (n < 0)
		return (shrinkwind_horiz(f, -n));
	return (resize_horiz(n));
}

/*
 * shrink-window-horizontally: shrink the current window's column band by
 * n columns, giving them to an adjacent band.
 */
int
shrinkwind_horiz(int f, int n)
{
	if (n < 0)
		return (enlargewind_horiz(f, -n));
	return (resize_horiz(-n));
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
 * Try to absorb wp into adjacent windows along one axis.
 *
 *   axis=COL: absorbers are left or right of wp. They must share a
 *             (leftcol, ncols) and their row spans must tile wp's
 *             row span exactly. Each absorber gains wp's columns +
 *             the divider column. Used for "vertical split" deletes.
 *
 *   axis=ROW: absorbers are above or below wp. They must share a
 *             (toprow, ntrows) and their column spans must tile wp's
 *             column span exactly (with divider columns between).
 *             Each absorber gains wp's rows + the modeline row.
 *
 * Returns the survivor on success, NULL if no valid absorber set
 * exists on this axis. Validation runs before any mutation, so a
 * NULL return leaves layout untouched.
 */
#define ABS_AXIS_COL	0
#define ABS_AXIS_ROW	1
static struct mgwin *
absorb_into(struct mgwin *wp, int axis)
{
	struct mgwin	*iter;
	struct mgwin	*set[64];
	struct line	*lp;
	int		 nset = 0;
	int		 max = (int)(sizeof(set) / sizeof(set[0]));
	int		 i, j;
	int		 forward;	/* forward = "absorbers come before wp" */
	int		 wpleft = wp->w_leftcol;
	int		 wpright = wp->w_leftcol + wp->w_ncols;
	int		 wptop = wp->w_toprow;
	int		 wpbot = wp->w_toprow + wp->w_ntrows;

	/* Gather candidates on the "near" side of wp. */
	if (axis == ABS_AXIS_COL) {
		for (iter = wheadp; iter != NULL; iter = iter->w_wndp) {
			if (iter == wp)
				continue;
			if (iter->w_leftcol + iter->w_ncols + 1 == wpleft &&
			    nset < max)
				set[nset++] = iter;
		}
		forward = (nset > 0);
		if (nset == 0) {
			for (iter = wheadp; iter != NULL; iter = iter->w_wndp) {
				if (iter == wp)
					continue;
				if (iter->w_leftcol == wpright + 1 &&
				    nset < max)
					set[nset++] = iter;
			}
		}
	} else {
		for (iter = wheadp; iter != NULL; iter = iter->w_wndp) {
			if (iter == wp)
				continue;
			if (iter->w_toprow + iter->w_ntrows + 1 == wptop &&
			    nset < max)
				set[nset++] = iter;
		}
		forward = (nset > 0);
		if (nset == 0) {
			for (iter = wheadp; iter != NULL; iter = iter->w_wndp) {
				if (iter == wp)
					continue;
				if (wpbot + 1 == iter->w_toprow && nset < max)
					set[nset++] = iter;
			}
		}
	}
	if (nset == 0)
		return (NULL);

	/* All absorbers must share the perpendicular geometry. */
	for (j = 1; j < nset; j++) {
		if (axis == ABS_AXIS_COL) {
			if (set[j]->w_leftcol != set[0]->w_leftcol ||
			    set[j]->w_ncols != set[0]->w_ncols)
				return (NULL);
		} else {
			if (set[j]->w_toprow != set[0]->w_toprow ||
			    set[j]->w_ntrows != set[0]->w_ntrows)
				return (NULL);
		}
	}

	/* Sort by the parallel axis. */
	for (i = 1; i < nset; i++) {
		struct mgwin *tmp = set[i];
		int key = (axis == ABS_AXIS_COL) ?
		    tmp->w_toprow : tmp->w_leftcol;

		for (j = i - 1; j >= 0; j--) {
			int prev = (axis == ABS_AXIS_COL) ?
			    set[j]->w_toprow : set[j]->w_leftcol;
			if (prev <= key)
				break;
			set[j+1] = set[j];
		}
		set[j+1] = tmp;
	}

	/* Validate tiling along the parallel axis. */
	if (axis == ABS_AXIS_COL) {
		if (set[0]->w_toprow != wptop)
			return (NULL);
		for (j = 0; j < nset; j++) {
			int abot = set[j]->w_toprow + set[j]->w_ntrows;

			if (j == nset - 1) {
				if (abot != wpbot)
					return (NULL);
			} else if (abot + 1 != set[j+1]->w_toprow) {
				return (NULL);
			}
		}
	} else {
		if (set[0]->w_leftcol != wpleft)
			return (NULL);
		for (j = 0; j < nset; j++) {
			int aright = set[j]->w_leftcol + set[j]->w_ncols;

			if (j == nset - 1) {
				if (aright != wpright)
					return (NULL);
			} else if (aright + 1 != set[j+1]->w_leftcol) {
				return (NULL);
			}
		}
	}

	/* Apply absorption. */
	if (axis == ABS_AXIS_COL) {
		for (j = 0; j < nset; j++) {
			set[j]->w_ncols += wp->w_ncols + 1;
			if (!forward)
				set[j]->w_leftcol = wpleft;
			set[j]->w_propw += wp->w_propw;
			set[j]->w_rflag |= WFMODE | WFFULL;
		}
	} else {
		int extra = wp->w_ntrows + 1;

		for (j = 0; j < nset; j++) {
			if (!forward) {
				set[j]->w_toprow = wptop;
				lp = set[j]->w_linep;
				for (i = 0; i < extra &&
				    lback(lp) != set[j]->w_bufp->b_headp; i++)
					lp = lback(lp);
				set[j]->w_linep = lp;
			}
			set[j]->w_ntrows += extra;
			set[j]->w_rflag |= WFMODE | WFFULL;
		}
	}
	return (set[0]);
}

/*
 * Delete current window. Geometry-aware:
 *
 *   1. Horizontal-split delete: if wp has a vertical neighbor in the
 *      same column band (same (w_leftcol, w_ncols), touching top-or-
 *      bottom edge), that neighbor absorbs wp's rows.
 *
 *   2. Column absorb: gather all windows whose right or left edge
 *      touches wp's opposite edge. They must share (w_leftcol, w_ncols)
 *      and tile wp's row span exactly. Each gains wp's columns + divider.
 *
 *   3. Row absorb: gather all windows whose bottom or top edge touches
 *      wp's opposite edge. They must share (w_toprow, w_ntrows) and
 *      tile wp's column span exactly. Each gains wp's rows + modeline.
 *
 *   4. If none work, refuse the delete and leave state untouched.
 *
 * A "column band" is keyed by (w_leftcol, w_ncols); a "row band" by
 * (w_toprow, w_ntrows). Two windows sharing only one of those values
 * belong to different bands.
 */
int
delwind(int f, int n)
{
	struct mgwin	*wp = curwp;
	struct mgwin	*band_mate = NULL;
	struct mgwin	*survivor = NULL;
	struct mgwin	*iter, *prev;
	struct line	*lp;
	int		 i;
	int		 total = 0;

	for (iter = wheadp; iter != NULL; iter = iter->w_wndp)
		total++;
	if (total <= 1) {
		dobeep();
		ewprintf("Only one window");
		return (FALSE);
	}

	/* Case 1: horizontal-split delete (same band, vertically adjacent). */
	for (iter = wheadp; iter != NULL; iter = iter->w_wndp) {
		if (iter == wp)
			continue;
		if (iter->w_leftcol != wp->w_leftcol)
			continue;
		if (iter->w_ncols != wp->w_ncols)
			continue;
		if (iter->w_toprow + iter->w_ntrows + 1 == wp->w_toprow ||
		    wp->w_toprow + wp->w_ntrows + 1 == iter->w_toprow) {
			band_mate = iter;
			break;
		}
	}

	if (band_mate != NULL) {
		int extra = wp->w_ntrows + 1;

		if (band_mate->w_toprow > wp->w_toprow) {
			band_mate->w_toprow = wp->w_toprow;
			lp = band_mate->w_linep;
			for (i = 0; i < extra &&
			    lback(lp) != band_mate->w_bufp->b_headp; i++)
				lp = lback(lp);
			band_mate->w_linep = lp;
		}
		band_mate->w_ntrows += extra;
		band_mate->w_rflag |= WFMODE | WFFULL;
		survivor = band_mate;
	}
	if (survivor == NULL)
		survivor = absorb_into(wp, ABS_AXIS_COL);
	if (survivor == NULL)
		survivor = absorb_into(wp, ABS_AXIS_ROW);
	if (survivor == NULL) {
		dobeep();
		ewprintf("No adjacent space to absorb");
		return (FALSE);
	}

	if (--wp->w_bufp->b_nwnd == 0) {
		wp->w_bufp->b_dotp = wp->w_dotp;
		wp->w_bufp->b_doto = wp->w_doto;
		wp->w_bufp->b_markp = wp->w_markp;
		wp->w_bufp->b_marko = wp->w_marko;
		wp->w_bufp->b_markp = wp->w_markp;
		wp->w_bufp->b_dotline = wp->w_dotline;
		wp->w_bufp->b_markline = wp->w_markline;
	}

	if (wp == wheadp) {
		wheadp = wp->w_wndp;
	} else {
		for (prev = wheadp; prev != NULL; prev = prev->w_wndp) {
			if (prev->w_wndp == wp) {
				prev->w_wndp = wp->w_wndp;
				break;
			}
		}
	}
	curwp = survivor;
	curbp = curwp->w_bufp;
	free(wp);
	sgarbf = TRUE;
	return (TRUE);
}
