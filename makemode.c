/* This file is in the public domain. */

/*
 * Makefile mode.
 *
 * Make's recipe syntax requires literal TAB characters; the editor's
 * global default of inserting spaces would corrupt such files.  This
 * mode forces real tabs and a tab width of 8 for the current buffer,
 * and is registered as an autoexec hook for the standard Make variants
 * (GNU make, BSD make, and *.mk fragments).
 */

#include <sys/queue.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

#include "def.h"
#include "funmap.h"

void	makemode_init(void);
int	makefilemode(int, int);

void
makemode_init(void)
{
	funmap_add(makefilemode, "makefile-mode", 0);
}

int
makefilemode(int f, int n)
{
	if (curbp->b_flag & BFNOTAB)
		(void)notabmode(FFARG, 0);

	curbp->b_tabw = 8;
	curwp->w_rflag |= WFFRAME;
	return (TRUE);
}
