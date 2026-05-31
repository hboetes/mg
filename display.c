/*	$OpenBSD: display.c,v 1.52 2023/04/21 13:39:37 op Exp $	*/

/* This file is in the public domain. */

/*
 * The functions in this file handle redisplay. The
 * redisplay system knows almost nothing about the editing
 * process; the editing functions do, however, set some
 * hints to eliminate a lot of the grinding. There is more
 * that can be done; the "vtputc" interface is a real
 * pig.
 */

#include <sys/queue.h>
#include <ctype.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <term.h>

#include "def.h"
#include "kbd.h"

/*
 * A video structure always holds
 * an array of characters whose length is equal to
 * the longest line possible. v_text is allocated
 * dynamically to fit the screen width.
 */
struct video {
	short	v_hash;		/* Hash code, for compares.	 */
	short	v_flag;		/* Flag word.			 */
	short	v_color;	/* Color of [v_lend, ncol), or whole row */
	short	v_lcolor;	/* Color of [0, v_lend) when v_lend > 0 */
	short	v_lend;		/* Split column; 0 = single-color row	 */
	int	v_cost;		/* Cost of display.		 */
	char	*v_text;	/* The actual characters.	 */
	char	*v_attr;	/* Per-column color attr (CTEXT/CHILIGHT) */
};

#define VFCHG	0x0001			/* Changed.			 */
#define VFHBAD	0x0002			/* Hash and cost are bad.	 */
#define VFEXT	0x0004			/* extended line (beyond ncol)	 */

/*
 * SCORE structures hold the optimal
 * trace trajectory, and the cost of redisplay, when
 * the dynamic programming redisplay code is used.
 */
struct score {
	int	s_itrace;	/* "i" index for track back.	 */
	int	s_jtrace;	/* "j" index for trace back.	 */
	int	s_cost;		/* Display cost.		 */
};

void	vtmove(int, int);
void	vtputc(int, struct mgwin *);
void	vtpute(int, struct mgwin *);
int	vtputs(const char *, struct mgwin *);
void	vteeol(struct mgwin *);
void	updext(int, int, struct mgwin *);
void	modeline(struct mgwin *, int);
void	paint_divider(struct mgwin *);
void	setscores(int, int);
void	traceback(int, int, int, int);
void	ucopy(struct video *, struct video *);
void	uline(int, struct video *, struct video *);
void	hash(struct video *);

static void	reset_row_attr(int);
static void	paint_region(struct mgwin *);
static int	dispcol(struct line *, int, int);
static int	effcolor(struct video *, int);

int	sgarbf = TRUE;		/* TRUE if screen is garbage.	 */
int	vtrow = HUGE;		/* Virtual cursor row.		 */
int	vtcol = HUGE;		/* Virtual cursor column.	 */
static int vtattr = CTEXT;	/* Current paint attribute for vtputc.	*/
int	tthue = CNONE;		/* Current color.		 */
int	ttrow = HUGE;		/* Physical cursor row.		 */
int	ttcol = HUGE;		/* Physical cursor column.	 */
int	tttop = HUGE;		/* Top of scroll region.	 */
int	ttbot = HUGE;		/* Bottom of scroll region.	 */

struct video	**vscreen;		/* Edge vector, virtual.	 */
struct video	**pscreen;		/* Edge vector, physical.	 */
struct video	 *video;		/* Actual screen data.		 */
struct video	  blanks;		/* Blank line image.		 */

/*
 * This matrix is written as an array because
 * we do funny things in the "setscores" routine, which
 * is very compute intensive, to make the subscripts go away.
 * It would be "SCORE	score[NROW][NROW]" in old speak.
 * Look at "setscores" to understand what is up.
 */
struct score *score;			/* [NROW * NROW] */

static int	 linenos = TRUE;
static int	 colnos = FALSE;

/* Is macro recording enabled? */
extern int macrodef;
/* Is working directory global? */
extern int globalwd;

/*
 * Since we don't have variables (we probably should) these are command
 * processors for changing the values of mode flags.
 */
int
linenotoggle(int f, int n)
{
	if (f & FFARG)
		linenos = n > 0;
	else
		linenos = !linenos;

	sgarbf = TRUE;

	return (TRUE);
}

int
colnotoggle(int f, int n)
{
	if (f & FFARG)
		colnos = n > 0;
	else
		colnos = !colnos;

	sgarbf = TRUE;

	return (TRUE);
}

/*
 * Reinit the display data structures, this is called when the terminal
 * size changes.
 */
int
vtresize(int force, int newrow, int newcol)
{
	int	 i;
	int	 rowchanged, colchanged;
	static	 int first_run = 1;
	struct video	*vp;

	if (newrow < 1 || newcol < 1)
		return (FALSE);

	rowchanged = (newrow != nrow);
	colchanged = (newcol != ncol);

#define TRYREALLOC(a, n) do {					\
		void *tmp;					\
		if ((tmp = realloc((a), (n))) == NULL) {	\
			panic("out of memory in display code");	\
		}						\
		(a) = tmp;					\
	} while (0)

#define TRYREALLOCARRAY(a, n, m) do {				\
		void *tmp;					\
		if ((tmp = reallocarray((a), (n), (m))) == NULL) {\
			panic("out of memory in display code");	\
		}						\
		(a) = tmp;					\
	} while (0)

	/* No update needed */
	if (!first_run && !force && !rowchanged && !colchanged)
		return (TRUE);

	if (first_run)
		memset(&blanks, 0, sizeof(blanks));

	if (rowchanged || first_run) {
		int vidstart;

		/*
		 * This is not pretty.
		 */
		if (nrow == 0)
			vidstart = 0;
		else
			vidstart = 2 * (nrow - 1);

		/*
		 * We're shrinking, free some internal data.
		 */
		if (newrow < nrow) {
			for (i = 2 * (newrow - 1); i < 2 * (nrow - 1); i++) {
				free(video[i].v_text);
				video[i].v_text = NULL;
				free(video[i].v_attr);
				video[i].v_attr = NULL;
			}
		}

		TRYREALLOCARRAY(score, newrow, newrow * sizeof(struct score));
		TRYREALLOCARRAY(vscreen, (newrow - 1), sizeof(struct video *));
		TRYREALLOCARRAY(pscreen, (newrow - 1), sizeof(struct video *));
		TRYREALLOCARRAY(video, (newrow - 1), 2 * sizeof(struct video));

		/*
		 * Zero-out the entries we just allocated.
		 */
		for (i = vidstart; i < 2 * (newrow - 1); i++)
			memset(&video[i], 0, sizeof(struct video));

		/*
		 * Reinitialize vscreen and pscreen arrays completely.
		 */
		vp = &video[0];
		for (i = 0; i < newrow - 1; ++i) {
			vscreen[i] = vp;
			++vp;
			pscreen[i] = vp;
			++vp;
		}
	}
	if (rowchanged || colchanged || first_run) {
		for (i = 0; i < 2 * (newrow - 1); i++) {
			TRYREALLOC(video[i].v_text, newcol);
			TRYREALLOC(video[i].v_attr, newcol);
			memset(video[i].v_attr, CTEXT, newcol);
		}
		TRYREALLOC(blanks.v_text, newcol);
		TRYREALLOC(blanks.v_attr, newcol);
		memset(blanks.v_attr, CTEXT, newcol);
	}

	nrow = newrow;
	ncol = newcol;

	if (ttrow > nrow)
		ttrow = nrow;
	if (ttcol > ncol)
		ttcol = ncol;

	first_run = 0;
	return (TRUE);
}

#undef TRYREALLOC
#undef TRYREALLOCARRAY

/*
 * Initialize the data structures used
 * by the display code. The edge vectors used
 * to access the screens are set up. The operating
 * system's terminal I/O channel is set up. Fill the
 * "blanks" array with ASCII blanks. The rest is done
 * at compile time. The original window is marked
 * as needing full update, and the physical screen
 * is marked as garbage, so all the right stuff happens
 * on the first call to redisplay.
 */
void
vtinit(void)
{
	int	i;

	ttopen();
	ttinit();

	/*
	 * ttinit called ttresize(), which called vtresize(), so our data
	 * structures are setup correctly.
	 */

	blanks.v_color = CTEXT;
	for (i = 0; i < ncol; ++i)
		blanks.v_text[i] = ' ';
}

/*
 * Tidy up the virtual display system
 * in anticipation of a return back to the host
 * operating system. Right now all we do is position
 * the cursor to the last line, erase the line, and
 * close the terminal channel.
 */
void
vttidy(void)
{
	ttcolor(CTEXT);
	ttnowindow();		/* No scroll window.	 */
	ttmove(nrow - 1, 0);	/* Echo line.		 */
	tteeol();
	tttidy();
	ttflush();
	ttclose();
}

/*
 * Move the virtual cursor to an origin
 * 0 spot on the virtual display screen. I could
 * store the column as a character pointer to the spot
 * on the line, which would make "vtputc" a little bit
 * more efficient. No checking for errors.
 */
void
vtmove(int row, int col)
{
	vtrow = row;
	vtcol = col;
}

/*
 * Write a character to the virtual display,
 * dealing with long lines and the display of unprintable
 * things like control characters. Also expand tabs every 8
 * columns. This code only puts printing characters into
 * the virtual display image. Special care must be taken when
 * expanding tabs. On a screen whose width is not a multiple
 * of 8, it is possible for the virtual cursor to hit the
 * right margin before the next tab stop is reached. This
 * makes the tab code loop if you are not careful.
 * Three guesses how we found this.
 */
void
vtputc(int c, struct mgwin *wp)
{
	struct video	*vp;
	int		 target;
	int		 right = WRIGHT(wp);

	c &= 0xff;

	vp = vscreen[vtrow];
	if (vtcol >= right) {
		vp->v_text[right - 1] = '$';
		if (vp->v_attr != NULL)
			vp->v_attr[right - 1] = vtattr;
	} else if (c == '\t') {
		target = ntabstop(vtcol - wp->w_leftcol, wp->w_bufp->b_tabw);
		do {
			vtputc(' ', wp);
		} while (vtcol < right && (vtcol - wp->w_leftcol) < target);
	} else if (ISCTRL(c)) {
		vtputc('^', wp);
		vtputc(CCHR(c), wp);
	} else if (isprint(c)) {
		if (vp->v_attr != NULL)
			vp->v_attr[vtcol] = vtattr;
		vp->v_text[vtcol++] = c;
	} else {
		char bf[5];

		snprintf(bf, sizeof(bf), "\\%o", c);
		vtputs(bf, wp);
	}
}

/*
 * Put a character to the virtual screen in an extended line.  If we are not
 * yet on left edge, don't print it yet.  Check for overflow on the right
 * margin.
 */
void
vtpute(int c, struct mgwin *wp)
{
	struct video *vp;
	int target;
	int right = WRIGHT(wp);
	int textcol = vtcol - wp->w_leftcol + wp->w_lbound;

	c &= 0xff;

	vp = vscreen[vtrow];
	if (vtcol >= right) {
		vp->v_text[right - 1] = '$';
		if (vp->v_attr != NULL)
			vp->v_attr[right - 1] = vtattr;
	} else if (c == '\t') {
		target = ntabstop(textcol, wp->w_bufp->b_tabw);
		do {
			vtpute(' ', wp);
		} while (((vtcol - wp->w_leftcol + wp->w_lbound) < target)
		    && vtcol < right);
	} else if (ISCTRL(c) != FALSE) {
		vtpute('^', wp);
		vtpute(CCHR(c), wp);
	} else if (isprint(c)) {
		if (vtcol >= wp->w_leftcol) {
			vp->v_text[vtcol] = c;
			if (vp->v_attr != NULL)
				vp->v_attr[vtcol] = vtattr;
		}
		++vtcol;
	} else {
		char bf[5], *cp;

		snprintf(bf, sizeof(bf), "\\%o", c);
		for (cp = bf; *cp != '\0'; cp++)
			vtpute(*cp, wp);
	}
}

/*
 * Erase from the end of the software cursor to the end of the line on which
 * the software cursor is located. The display routines will decide if a
 * hardware erase to end of line command should be used to display this.
 */
void
vteeol(struct mgwin *wp)
{
	struct video *vp;
	int right = WRIGHT(wp);

	vp = vscreen[vtrow];
	while (vtcol < right) {
		if (vp->v_attr != NULL)
			vp->v_attr[vtcol] = vtattr;
		vp->v_text[vtcol++] = ' ';
	}
}

/*
 * Reset a row's per-column attribute buffer to CTEXT.  Called before
 * laying out a row's text so paint_region can overlay highlight cleanly.
 */
static void
reset_row_attr(int row)
{
	if (vscreen[row]->v_attr != NULL)
		memset(vscreen[row]->v_attr, CTEXT, ncol);
}

/*
 * Compute the display column corresponding to buffer offset `off` on `lp`.
 * Mirrors the tab / control-char / unprintable expansion used by vtputc
 * and getcolpos.
 */
static int
dispcol(struct line *lp, int off, int tabw)
{
	int	col = 0, i, c;
	char	tmp[5];

	if (off > llength(lp))
		off = llength(lp);
	for (i = 0; i < off; i++) {
		c = lgetc(lp, i);
		if (c == '\t')
			col = ntabstop(col, tabw);
		else if (ISCTRL(c) != FALSE)
			col += 2;
		else if (isprint(c))
			col++;
		else
			col += snprintf(tmp, sizeof(tmp), "\\%o", c);
	}
	return (col);
}

/*
 * Paint the active region in window `wp` by overlaying CHILIGHT into
 * v_attr for cells inside the region.  Called per window per redisplay
 * after the row text has been laid out by update().
 */
static void
paint_region(struct mgwin *wp)
{
	struct line	*lp, *sl, *el;
	int		 slo, elo, sln, eln;
	int		 i, srow, in_region, tabw;
	int		 lcol, lbnd, right;
	int		 rstart, rend, c;
	int		 sc_abs, ec_abs;
	int		 llen_col;

	if (wp->w_markp == NULL)
		return;

	/* Empty region: nothing to highlight. */
	if (wp->w_markp == wp->w_dotp && wp->w_marko == wp->w_doto)
		return;

	tabw = wp->w_bufp->b_tabw;
	lcol = wp->w_leftcol;
	lbnd = wp->w_lbound;
	right = WRIGHT(wp);

	/* Normalize so (sl,slo,sln) is start <= (el,elo,eln) is end. */
	if (wp->w_markline < wp->w_dotline ||
	    (wp->w_markline == wp->w_dotline &&
	     wp->w_marko < wp->w_doto)) {
		sl = wp->w_markp; slo = wp->w_marko; sln = wp->w_markline;
		el = wp->w_dotp;  elo = wp->w_doto;  eln = wp->w_dotline;
	} else {
		sl = wp->w_dotp;  slo = wp->w_doto;  sln = wp->w_dotline;
		el = wp->w_markp; elo = wp->w_marko; eln = wp->w_markline;
	}
	(void)sln; (void)eln;

	lp = wp->w_linep;
	in_region = 0;
	for (i = 0; i < wp->w_ntrows; i++) {
		srow = wp->w_toprow + i;
		if (lp == wp->w_bufp->b_headp)
			break;
		/* Skip rows that aren't plain text (modeline shouldn't be
		 * inside the text-row range, but stay safe). */
		if (vscreen[srow]->v_color != CTEXT ||
		    vscreen[srow]->v_lend != 0)
			goto next;

		if (!in_region && lp == sl)
			in_region = 1;

		if (in_region) {
			llen_col = dispcol(lp, llength(lp), tabw);
			if (lp == sl && lp == el) {
				rstart = dispcol(lp, slo, tabw);
				rend   = dispcol(lp, elo, tabw);
			} else if (lp == sl) {
				rstart = dispcol(lp, slo, tabw);
				rend   = llen_col + 1; /* incl newline cell */
			} else if (lp == el) {
				rstart = 0;
				rend   = dispcol(lp, elo, tabw);
			} else {
				rstart = 0;
				rend   = llen_col + 1;
			}

			sc_abs = lcol + rstart - lbnd;
			ec_abs = lcol + rend   - lbnd;
			if (sc_abs < lcol)
				sc_abs = lcol;
			if (ec_abs > right)
				ec_abs = right;
			if (vscreen[srow]->v_attr != NULL) {
				for (c = sc_abs; c < ec_abs; c++)
					vscreen[srow]->v_attr[c] = CHILIGHT;
			}
		}

		if (lp == el)
			in_region = 0;
next:
		lp = lforw(lp);
	}
}

/*
 * Effective per-column color for `vp` at `col`.  Honors split-color rows
 * (v_lend), row-level color (v_color), or falls through to per-cell v_attr.
 */
static int
effcolor(struct video *vp, int col)
{
	if (vp->v_lend > 0)
		return (col < vp->v_lend ? vp->v_lcolor : vp->v_color);
	if (vp->v_color != CTEXT)
		return (vp->v_color);
	if (vp->v_attr == NULL)
		return (CTEXT);
	return (vp->v_attr[col]);
}

/*
 * Make sure that the display is
 * right. This is a three part process. First,
 * scan through all of the windows looking for dirty
 * ones. Check the framing, and refresh the screen.
 * Second, make sure that "currow" and "curcol" are
 * correct for the current window. Third, make the
 * virtual and physical screens the same.
 */
void
update(int modelinecolor)
{
	struct line	*lp;
	struct mgwin	*wp;
	struct video	*vp1;
	struct video	*vp2;
	int	 c, i, j;
	int	 hflag;
	int	 currow, curcol;
	int	 offs, size;

	if (charswaiting())
		return;

	/*
	 * Safety net: if the terminal is too small to draw anything,
	 * skip the paint entirely. vscreen is sized nrow-1; any row
	 * index >= nrow-1 would dereference out of bounds.
	 */
	if (nrow < 3)
		return;
	for (wp = wheadp; wp != NULL; wp = wp->w_wndp) {
		if (wp->w_toprow < 0 ||
		    wp->w_toprow + wp->w_ntrows + 1 > nrow - 1 ||
		    wp->w_leftcol < 0 ||
		    wp->w_leftcol + wp->w_ncols > ncol)
			return;
	}
	if (sgarbf) {		/* must update everything */
		wp = wheadp;
		while (wp != NULL) {
			wp->w_rflag |= WFMODE | WFFULL;
			wp = wp->w_wndp;
		}
	}
	if (linenos || colnos) {
		wp = wheadp;
		while (wp != NULL) {
			wp->w_rflag |= WFMODE;
			wp = wp->w_wndp;
		}
	}
	hflag = FALSE;			/* Not hard. */
	for (wp = wheadp; wp != NULL; wp = wp->w_wndp) {
		/*
		 * Active region: in-line moves (backchar/forwchar/gotobol/
		 * gotoeol) set no rflag, so dot can shift without us being
		 * told. Force WFFULL whenever mark is set so reset_row_attr
		 * clears stale attrs from cells that left the region before
		 * paint_region re-stamps the current region.
		 */
		if (wp->w_markp != NULL)
			wp->w_rflag |= WFFULL;

		/*
		 * Nothing to be done.
		 */
		if (wp->w_rflag == 0)
			continue;

		if ((wp->w_rflag & WFFRAME) == 0) {
			lp = wp->w_linep;
			for (i = 0; i < wp->w_ntrows; ++i) {
				if (lp == wp->w_dotp)
					goto out;
				if (lp == wp->w_bufp->b_headp)
					break;
				lp = lforw(lp);
			}
		}
		/*
		 * Put the middle-line in place.
		 */
		i = wp->w_frame;
		if (i > 0) {
			--i;
			if (i >= wp->w_ntrows)
				i = wp->w_ntrows - 1;
		} else if (i < 0) {
			i += wp->w_ntrows;
			if (i < 0)
				i = 0;
		} else
			i = wp->w_ntrows / 2; /* current center, no change */

		/*
		 * Find the line.
		 */
		lp = wp->w_dotp;
		while (i != 0 && lback(lp) != wp->w_bufp->b_headp) {
			--i;
			lp = lback(lp);
		}
		wp->w_linep = lp;
		wp->w_rflag |= WFFULL;	/* Force full.		 */
	out:
		lp = wp->w_linep;	/* Try reduced update.	 */
		i = wp->w_toprow;
		if ((wp->w_rflag & ~WFMODE) == WFEDIT) {
			while (lp != wp->w_dotp) {
				++i;
				lp = lforw(lp);
			}
			vscreen[i]->v_color = CTEXT;
			vscreen[i]->v_lend = 0;
			vscreen[i]->v_flag |= (VFCHG | VFHBAD);
			reset_row_attr(i);
			vtattr = CTEXT;
			vtmove(i, wp->w_leftcol);
			for (j = 0; j < llength(lp); ++j)
				vtputc(lgetc(lp, j), wp);
			vteeol(wp);
		} else if ((wp->w_rflag & (WFEDIT | WFFULL)) != 0) {
			hflag = TRUE;
			while (i < wp->w_toprow + wp->w_ntrows) {
				vscreen[i]->v_color = CTEXT;
				vscreen[i]->v_lend = 0;
				vscreen[i]->v_flag |= (VFCHG | VFHBAD);
				reset_row_attr(i);
				vtattr = CTEXT;
				vtmove(i, wp->w_leftcol);
				if (lp != wp->w_bufp->b_headp) {
					for (j = 0; j < llength(lp); ++j)
						vtputc(lgetc(lp, j), wp);
					lp = lforw(lp);
				}
				vteeol(wp);
				++i;
			}
		}
	}
	/*
	 * Modeline pass.  Runs after every window's text paint so a
	 * later window's WFFULL loop can't clobber the split-color state
	 * (v_lend / v_lcolor / v_color) that modeline() installs on a
	 * row that is simultaneously a modeline row for one window and
	 * a text row for another.
	 */
	for (wp = wheadp; wp != NULL; wp = wp->w_wndp) {
		if (wp->w_rflag == 0)
			continue;
		if ((wp->w_rflag & WFMODE) != 0)
			modeline(wp, modelinecolor);
		paint_divider(wp);
		wp->w_rflag = 0;
		wp->w_frame = 0;
	}
	lp = curwp->w_linep;	/* Cursor location. */
	currow = curwp->w_toprow;
	while (lp != curwp->w_dotp) {
		++currow;
		lp = lforw(lp);
	}
	curcol = 0;
	i = 0;
	while (i < curwp->w_doto) {
		c = lgetc(lp, i++);
		if (c == '\t') {
			curcol = ntabstop(curcol, curwp->w_bufp->b_tabw);
		} else if (ISCTRL(c) != FALSE)
			curcol += 2;
		else if (isprint(c))
			curcol++;
		else {
			char bf[5];

			snprintf(bf, sizeof(bf), "\\%o", c);
			curcol += strlen(bf);
		}
	}
	if (curcol >= curwp->w_ncols - 1) {	/* extended line. */
		/* flag we are extended and changed */
		vscreen[currow]->v_flag |= VFEXT | VFCHG;
		updext(currow, curcol, curwp); /* and output extended line */
	} else
		curwp->w_lbound = 0;	/* not extended line */

	/*
	 * Make sure no lines need to be de-extended because the cursor is no
	 * longer on them.
	 */
	wp = wheadp;
	while (wp != NULL) {
		lp = wp->w_linep;
		i = wp->w_toprow;
		while (i < wp->w_toprow + wp->w_ntrows) {
			if (vscreen[i]->v_flag & VFEXT) {
				/* always flag extended lines as changed */
				vscreen[i]->v_flag |= VFCHG;
				if ((wp != curwp) || (lp != wp->w_dotp) ||
				    (curcol < curwp->w_ncols - 1)) {
					vtmove(i, wp->w_leftcol);
					for (j = 0; j < llength(lp); ++j)
						vtputc(lgetc(lp, j), wp);
					vteeol(wp);
					/* this line no longer is extended */
					vscreen[i]->v_flag &= ~VFEXT;
				}
			}
			lp = lforw(lp);
			++i;
		}
		/* if garbaged then fix up mode lines */
		if (sgarbf != FALSE)
			vscreen[i]->v_flag |= VFCHG;
		/* and onward to the next window */
		wp = wp->w_wndp;
	}

	/*
	 * Overlay region highlight AFTER updext / de-extend so those paths
	 * don't clobber v_attr.  Force VFCHG and recompute hash on any row
	 * we touched so the screen-paint engine emits the changes.
	 */
	for (wp = wheadp; wp != NULL; wp = wp->w_wndp) {
		int rmin, rmax;
		paint_region(wp);
		if (wp->w_markp == NULL)
			continue;
		rmin = wp->w_toprow;
		rmax = wp->w_toprow + wp->w_ntrows;
		for (i = rmin; i < rmax && i < nrow - 1; i++) {
			vscreen[i]->v_flag |= (VFCHG | VFHBAD);
		}
	}

	if (sgarbf != FALSE) {	/* Screen is garbage.	 */
		sgarbf = FALSE;	/* Erase-page clears.	 */
		epresf = FALSE;	/* The message area.	 */
		tttop = HUGE;	/* Forget where you set. */
		ttbot = HUGE;	/* scroll region.	 */
		tthue = CNONE;	/* Color unknown.	 */
		ttmove(0, 0);
		tteeop();
		for (i = 0; i < nrow - 1; ++i) {
			uline(i, vscreen[i], &blanks);
			ucopy(vscreen[i], pscreen[i]);
		}
		ttmove(currow, curwp->w_leftcol + curcol - curwp->w_lbound);
		ttflush();
		return;
	}
	if (hflag != FALSE) {			/* Hard update?		*/
		for (i = 0; i < nrow - 1; ++i) {/* Compute hash data.	*/
			hash(vscreen[i]);
			hash(pscreen[i]);
		}
		offs = 0;			/* Get top match.	*/
		while (offs != nrow - 1) {
			vp1 = vscreen[offs];
			vp2 = pscreen[offs];
			if (vp1->v_color != vp2->v_color
			    || vp1->v_hash != vp2->v_hash)
				break;
			uline(offs, vp1, vp2);
			ucopy(vp1, vp2);
			++offs;
		}
		if (offs == nrow - 1) {		/* Might get it all.	*/
			ttmove(currow, curwp->w_leftcol + curcol - curwp->w_lbound);
			ttflush();
			return;
		}
		size = nrow - 1;		/* Get bottom match.	*/
		while (size != offs) {
			vp1 = vscreen[size - 1];
			vp2 = pscreen[size - 1];
			if (vp1->v_color != vp2->v_color
			    || vp1->v_hash != vp2->v_hash)
				break;
			uline(size - 1, vp1, vp2);
			ucopy(vp1, vp2);
			--size;
		}
		if ((size -= offs) == 0)	/* Get screen size.	*/
			panic("Illegal screen size in update");
		setscores(offs, size);		/* Do hard update.	*/
		traceback(offs, size, size, size);
		for (i = 0; i < size; ++i)
			ucopy(vscreen[offs + i], pscreen[offs + i]);
		ttmove(currow, curwp->w_leftcol + curcol - curwp->w_lbound);
		ttflush();
		return;
	}
	for (i = 0; i < nrow - 1; ++i) {	/* Easy update.		*/
		vp1 = vscreen[i];
		vp2 = pscreen[i];
		if ((vp1->v_flag & VFCHG) != 0) {
			uline(i, vp1, vp2);
			ucopy(vp1, vp2);
		}
	}
	ttmove(currow, curwp->w_leftcol + curcol - curwp->w_lbound);
	ttflush();
}

/*
 * Update a saved copy of a line,
 * kept in a video structure. The "vvp" is
 * the one in the "vscreen". The "pvp" is the one
 * in the "pscreen". This is called to make the
 * virtual and physical screens the same when
 * display has done an update.
 */
void
ucopy(struct video *vvp, struct video *pvp)
{
	vvp->v_flag &= ~VFCHG;		/* Changes done.	 */
	pvp->v_flag = vvp->v_flag;	/* Update model.	 */
	pvp->v_hash = vvp->v_hash;
	pvp->v_cost = vvp->v_cost;
	pvp->v_color = vvp->v_color;
	pvp->v_lcolor = vvp->v_lcolor;
	pvp->v_lend = vvp->v_lend;
	bcopy(vvp->v_text, pvp->v_text, ncol);
	if (vvp->v_attr != NULL && pvp->v_attr != NULL)
		bcopy(vvp->v_attr, pvp->v_attr, ncol);
}

/*
 * updext: update the extended line which the cursor is currently on at a
 * column greater than the terminal width. The line will be scrolled right or
 * left to let the user see where the cursor is.
 */
void
updext(int currow, int curcol, struct mgwin *wp)
{
	struct line	*lp;			/* pointer to current line */
	int	 j;			/* index into line */
	int	 wncol = wp->w_ncols;

	if (wncol < 2)
		return;

	/*
	 * calculate what column the left bound should be
	 * (force cursor into middle half of the window)
	 */
	wp->w_lbound = curcol - (curcol % (wncol >> 1)) - (wncol >> 2);

	/*
	 * scan through the line outputting characters to the virtual screen
	 * once we reach the left edge of this window
	 */
	vtmove(currow, wp->w_leftcol - wp->w_lbound);	/* may be off-window */
	lp = wp->w_dotp;			/* line to output */
	for (j = 0; j < llength(lp); ++j)	/* until the end-of-line */
		vtpute(lgetc(lp, j), wp);
	vteeol(wp);				/* truncate the virtual line */
	vscreen[currow]->v_text[wp->w_leftcol] = '$';	/* '$' at left edge */
}

/*
 * Update a single line. This routine only
 * uses basic functionality (no insert and delete character,
 * but erase to end of line). The "vvp" points at the video
 * structure for the line on the virtual screen, and the "pvp"
 * is the same for the physical screen. Avoid erase to end of
 * line when updating CMODE color lines, because of the way that
 * reverse video works on most terminals.
 */
void
uline(int row, struct video *vvp, struct video *pvp)
{
	int	left, right, i, ec, cur_color;

	/*
	 * Split-color row (vertical-split modeline): paint two color regions
	 * unconditionally — the diff engine doesn't model color transitions.
	 */
	if (vvp->v_lend > 0) {
		ttmove(row, 0);
		ttcolor(vvp->v_lcolor);
		for (i = 0; i < vvp->v_lend; i++) {
			ttputc(vvp->v_text[i]);
			++ttcol;
		}
		ttcolor(vvp->v_color);
		for (i = vvp->v_lend; i < ncol; i++) {
			ttputc(vvp->v_text[i]);
			++ttcol;
		}
		ttcolor(CTEXT);
		return;
	}

	/*
	 * Attr-aware incremental paint.  Match left and right edges by text
	 * AND effective per-column color; repaint the changed middle while
	 * emitting ttcolor() transitions on color boundaries.
	 */
	left = 0;
	while (left < ncol &&
	    vvp->v_text[left] == pvp->v_text[left] &&
	    effcolor(vvp, left) == effcolor(pvp, left))
		left++;
	if (left == ncol)
		return;

	right = ncol;
	while (right > left &&
	    vvp->v_text[right - 1] == pvp->v_text[right - 1] &&
	    effcolor(vvp, right - 1) == effcolor(pvp, right - 1))
		right--;

	ttmove(row, left);
	cur_color = tthue;
	for (i = left; i < right; i++) {
		ec = effcolor(vvp, i);
		if (ec != cur_color) {
			ttcolor(ec);
			cur_color = ec;
		}
		ttputc(vvp->v_text[i]);
		++ttcol;
	}
	ttcolor(CTEXT);
}

/*
 * Paint the vertical divider column that sits at wp->w_leftcol - 1
 * for any window whose left edge is not the screen's left edge.
 * Writes the divider glyph into every row owned by the window
 * (text rows plus the mode-line row) and marks those rows changed
 * so the diff engine emits them to the terminal.
 *
 * No-op for windows flush against the left of the screen.
 */
void
paint_divider(struct mgwin *wp)
{
	int row, col, lastrow;

	if (wp->w_leftcol <= 0)
		return;

	col = wp->w_leftcol - 1;
	lastrow = wp->w_toprow + wp->w_ntrows;	/* mode-line row */
	for (row = wp->w_toprow; row <= lastrow; row++) {
		vscreen[row]->v_text[col] = '|';
		vscreen[row]->v_flag |= VFCHG;
	}
}

/*
 * Redisplay the mode line for the window pointed to by the "wp".
 * This is the only routine that has any idea of how the mode line is
 * formatted. You can change the modeline format by hacking at this
 * routine. Called by "update" any time there is a dirty window.  Note
 * that if STANDOUT_GLITCH is defined, first and last magic_cookie_glitch
 * characters may never be seen.
 */
void
modeline(struct mgwin *wp, int modelinecolor)
{
	int	n, md;
	struct buffer *bp;
	char sl[21];		/* Overkill. Space for 2^64 in base 10. */
	int len;

	n = wp->w_toprow + wp->w_ntrows;	/* Location.		 */
	/*
	 * Compute the row's color regions by walking all windows. Each
	 * window at row n is either contributing a modeline (its
	 * w_toprow + w_ntrows == n) or text (n in its text-row range).
	 * We merge all modeline columns into one extent [mode_l, mode_r),
	 * and all text columns into [text_l, text_r). If those extents
	 * don't interleave (one is strictly left of the other), we paint
	 * a two-region row; otherwise we degrade to plain CTEXT.
	 */
	{
		struct mgwin	*other;
		int		 mode_l, mode_r;
		int		 text_l = -1, text_r = -1;

		mode_l = wp->w_leftcol;
		mode_r = WRIGHT(wp);
		for (other = wheadp; other != NULL; other = other->w_wndp) {
			int other_modeline = other->w_toprow + other->w_ntrows;

			if (other == wp)
				continue;
			if (other_modeline == n) {
				if (other->w_leftcol < mode_l)
					mode_l = other->w_leftcol;
				if (WRIGHT(other) > mode_r)
					mode_r = WRIGHT(other);
			} else if (n >= other->w_toprow &&
			    n <  other_modeline) {
				if (text_l == -1 ||
				    other->w_leftcol < text_l)
					text_l = other->w_leftcol;
				if (WRIGHT(other) > text_r)
					text_r = WRIGHT(other);
			}
		}

		if (text_l == -1) {
			/* No text on this row: full-row modeline. */
			vscreen[n]->v_color = modelinecolor;
			vscreen[n]->v_lend = 0;
		} else if (mode_r <= text_l) {
			/* Modelines on the left, text on the right. */
			vscreen[n]->v_lcolor = modelinecolor;
			vscreen[n]->v_lend = mode_r;
			vscreen[n]->v_color = CTEXT;
		} else if (text_r <= mode_l) {
			/* Text on the left, modelines on the right. */
			vscreen[n]->v_lcolor = CTEXT;
			vscreen[n]->v_lend = mode_l;
			vscreen[n]->v_color = modelinecolor;
		} else {
			/*
			 * Interleaved / >2 regions (e.g. TEXT-MODE-TEXT when
			 * a middle-column window's modeline sits on a row
			 * whose flanking columns belong to text-row windows).
			 * The 2-region (v_color / v_lcolor / v_lend) model
			 * can't express that, so fall back to per-column
			 * v_attr.  v_color=CTEXT + v_lend=0 makes effcolor()
			 * read v_attr; vtattr below stamps modelinecolor onto
			 * the cells this window writes.
			 */
			vscreen[n]->v_color = CTEXT;
			vscreen[n]->v_lend = 0;
		}
	}
	vscreen[n]->v_flag |= (VFCHG | VFHBAD);	/* Recompute, display.	 */
	vtattr = modelinecolor;			/* Stamp v_attr for cells we write. */
	vtmove(n, wp->w_leftcol);		/* Seek to right line.	 */
	bp = wp->w_bufp;
	vtputc('-', wp);
	vtputc('-', wp);
	if ((bp->b_flag & BFREADONLY) != 0) {
		vtputc('%', wp);
		if ((bp->b_flag & BFCHG) != 0)
			vtputc('*', wp);
		else
			vtputc('%', wp);
	} else if ((bp->b_flag & BFCHG) != 0) {	/* "*" if changed.	 */
		vtputc('*', wp);
		vtputc('*', wp);
	} else {
		vtputc('-', wp);
		vtputc('-', wp);
	}
	vtputc('-', wp);
	n = 5;
	n += vtputs("Mg: ", wp);
	if (bp->b_bname[0] != '\0')
		n += vtputs(&(bp->b_bname[0]), wp);
	while (n < 42) {			/* Pad out with blanks.	 */
		vtputc(' ', wp);
		++n;
	}
	vtputc('(', wp);
	++n;
	for (md = 0; ; ) {
		n += vtputs(bp->b_modes[md]->p_name, wp);
		if (++md > bp->b_nmodes)
			break;
		vtputc('-', wp);
		++n;
	}
	/* XXX These should eventually move to a real mode */
	if (macrodef == TRUE)
		n += vtputs("-def", wp);
	if (globalwd == TRUE)
		n += vtputs("-gwd", wp);
	vtputc(')', wp);
	++n;

	if (linenos && colnos)
		len = snprintf(sl, sizeof(sl), "--L%d--C%d", wp->w_dotline,
		    getcolpos(wp));
	else if (linenos)
		len = snprintf(sl, sizeof(sl), "--L%d", wp->w_dotline);
	else if (colnos)
		len = snprintf(sl, sizeof(sl), "--C%d", getcolpos(wp));
	if ((linenos || colnos) && len < sizeof(sl) && len != -1)
		n += vtputs(sl, wp);

	while (n < wp->w_ncols) {		/* Pad out.		 */
		vtputc('-', wp);
		++n;
	}
	vtattr = CTEXT;				/* Restore default. */
}

/*
 * Output a string to the mode line, report how long it was.
 */
int
vtputs(const char *s, struct mgwin *wp)
{
	int n = 0;

	while (*s != '\0') {
		vtputc(*s++, wp);
		++n;
	}
	return (n);
}

/*
 * Compute the hash code for the line pointed to by the "vp".
 * Recompute it if necessary. Also set the approximate redisplay
 * cost. The validity of the hash code is marked by a flag bit.
 * The cost understand the advantages of erase to end of line.
 * Tuned for the VAX by Bob McNamara; better than it used to be on
 * just about any machine.
 */
void
hash(struct video *vp)
{
	int	i, n;
	char   *s;

	if ((vp->v_flag & VFHBAD) != 0) {	/* Hash bad.		 */
		s = &vp->v_text[ncol - 1];
		for (i = ncol; i != 0; --i, --s)
			if (*s != ' ')
				break;
		n = ncol - i;			/* Erase cheaper?	 */
		if (n > tceeol)
			n = tceeol;
		vp->v_cost = i + n;		/* Bytes + blanks.	 */
		for (n = 0; i != 0; --i, --s)
			n = (n << 5) + n + *s;
		n = (n << 5) + n + vp->v_lend;	/* Fold in color-split	 */
		n = (n << 5) + n + vp->v_lcolor;
		if (vp->v_attr != NULL) {	/* Fold in per-col attrs */
			for (i = 0; i < ncol; i++)
				n = (n << 5) + n + vp->v_attr[i];
		}
		vp->v_hash = n;			/* Hash code.		 */
		vp->v_flag &= ~VFHBAD;		/* Flag as all done.	 */
	}
}

/*
 * Compute the Insert-Delete
 * cost matrix. The dynamic programming algorithm
 * described by James Gosling is used. This code assumes
 * that the line above the echo line is the last line involved
 * in the scroll region. This is easy to arrange on the VT100
 * because of the scrolling region. The "offs" is the origin 0
 * offset of the first row in the virtual/physical screen that
 * is being updated; the "size" is the length of the chunk of
 * screen being updated. For a full screen update, use offs=0
 * and size=nrow-1.
 *
 * Older versions of this code implemented the score matrix by
 * a two dimensional array of SCORE nodes. This put all kinds of
 * multiply instructions in the code! This version is written to
 * use a linear array and pointers, and contains no multiplication
 * at all. The code has been carefully looked at on the VAX, with
 * only marginal checking on other machines for efficiency. In
 * fact, this has been tuned twice! Bob McNamara tuned it even
 * more for the VAX, which is a big issue for him because of
 * the 66 line X displays.
 *
 * On some machines, replacing the "for (i=1; i<=size; ++i)" with
 * i = 1; do { } while (++i <=size)" will make the code quite a
 * bit better; but it looks ugly.
 */
void
setscores(int offs, int size)
{
	struct score	 *sp;
	struct score	 *sp1;
	struct video	**vp, **pp;
	struct video	**vbase, **pbase;
	int	  tempcost;
	int	  bestcost;
	int	  j, i;

	vbase = &vscreen[offs - 1];	/* By hand CSE's.	 */
	pbase = &pscreen[offs - 1];
	score[0].s_itrace = 0;		/* [0, 0]		 */
	score[0].s_jtrace = 0;
	score[0].s_cost = 0;
	sp = &score[1];			/* Row 0, inserts.	 */
	tempcost = 0;
	vp = &vbase[1];
	for (j = 1; j <= size; ++j) {
		sp->s_itrace = 0;
		sp->s_jtrace = j - 1;
		tempcost += tcinsl;
		tempcost += (*vp)->v_cost;
		sp->s_cost = tempcost;
		++vp;
		++sp;
	}
	sp = &score[nrow];		/* Column 0, deletes.	 */
	tempcost = 0;
	for (i = 1; i <= size; ++i) {
		sp->s_itrace = i - 1;
		sp->s_jtrace = 0;
		tempcost += tcdell;
		sp->s_cost = tempcost;
		sp += nrow;
	}
	sp1 = &score[nrow + 1];		/* [1, 1].		 */
	pp = &pbase[1];
	for (i = 1; i <= size; ++i) {
		sp = sp1;
		vp = &vbase[1];
		for (j = 1; j <= size; ++j) {
			sp->s_itrace = i - 1;
			sp->s_jtrace = j;
			bestcost = (sp - nrow)->s_cost;
			if (j != size)	/* Cd(A[i])=0 @ Dis.	 */
				bestcost += tcdell;
			tempcost = (sp - 1)->s_cost;
			tempcost += (*vp)->v_cost;
			if (i != size)	/* Ci(B[j])=0 @ Dsj.	 */
				tempcost += tcinsl;
			if (tempcost < bestcost) {
				sp->s_itrace = i;
				sp->s_jtrace = j - 1;
				bestcost = tempcost;
			}
			tempcost = (sp - nrow - 1)->s_cost;
			if ((*pp)->v_color != (*vp)->v_color
			    || (*pp)->v_hash != (*vp)->v_hash)
				tempcost += (*vp)->v_cost;
			if (tempcost < bestcost) {
				sp->s_itrace = i - 1;
				sp->s_jtrace = j - 1;
				bestcost = tempcost;
			}
			sp->s_cost = bestcost;
			++sp;		/* Next column.		 */
			++vp;
		}
		++pp;
		sp1 += nrow;		/* Next row.		 */
	}
}

/*
 * Trace back through the dynamic programming cost
 * matrix, and update the screen using an optimal sequence
 * of redraws, insert lines, and delete lines. The "offs" is
 * the origin 0 offset of the chunk of the screen we are about to
 * update. The "i" and "j" are always started in the lower right
 * corner of the matrix, and imply the size of the screen.
 * A full screen traceback is called with offs=0 and i=j=nrow-1.
 * There is some do-it-yourself double subscripting here,
 * which is acceptable because this routine is much less compute
 * intensive then the code that builds the score matrix!
 */
void
traceback(int offs, int size, int i, int j)
{
	int	itrace, jtrace;
	int	k;
	int	ninsl, ndraw, ndell;

	if (i == 0 && j == 0)	/* End of update.	 */
		return;
	itrace = score[(nrow * i) + j].s_itrace;
	jtrace = score[(nrow * i) + j].s_jtrace;
	if (itrace == i) {	/* [i, j-1]		 */
		ninsl = 0;	/* Collect inserts.	 */
		if (i != size)
			ninsl = 1;
		ndraw = 1;
		while (itrace != 0 || jtrace != 0) {
			if (score[(nrow * itrace) + jtrace].s_itrace != itrace)
				break;
			jtrace = score[(nrow * itrace) + jtrace].s_jtrace;
			if (i != size)
				++ninsl;
			++ndraw;
		}
		traceback(offs, size, itrace, jtrace);
		if (ninsl != 0) {
			ttcolor(CTEXT);
			ttinsl(offs + j - ninsl, offs + size - 1, ninsl);
		}
		do {		/* B[j], A[j] blank.	 */
			k = offs + j - ndraw;
			uline(k, vscreen[k], &blanks);
		} while (--ndraw);
		return;
	}
	if (jtrace == j) {	/* [i-1, j]		 */
		ndell = 0;	/* Collect deletes.	 */
		if (j != size)
			ndell = 1;
		while (itrace != 0 || jtrace != 0) {
			if (score[(nrow * itrace) + jtrace].s_jtrace != jtrace)
				break;
			itrace = score[(nrow * itrace) + jtrace].s_itrace;
			if (j != size)
				++ndell;
		}
		if (ndell != 0) {
			ttcolor(CTEXT);
			ttdell(offs + i - ndell, offs + size - 1, ndell);
		}
		traceback(offs, size, itrace, jtrace);
		return;
	}
	traceback(offs, size, itrace, jtrace);
	k = offs + j - 1;
	uline(k, vscreen[k], pscreen[offs + i - 1]);
}
