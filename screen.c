/* Copyright (c) 1987, Oliver Laumann, Technical University of Berlin.
 * Not derived from licensed software.
 *
 * Permission is granted to freely use, copy, modify, and redistribute
 * this software, provided that no attempt is made to gain profit from it,
 * the author is not construed to be liable for any results of using the
 * software, alterations are clearly marked as such, and this notice is
 * not modified.
 */

static char ScreenVersion[] = "screen 1.1b 20-Mar-87";

#include <stdio.h>
#include <sgtty.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include "screen.h"

#define MAXWIN     10
#define MAXARGS    64
#define MAXLINE  1024
#define MSGWAIT     4

#define DEFAULT_SHELL   "/bin/sh"

#define Ctrl(c) ((c)&037)

#define PtyProto  "/dev/ptyXY"
#define TtyProto  "/dev/ttyXY"

extern char *blank, Term[], **environ;
extern rows, cols;
extern status;
extern time_t TimeDisplayed;
extern char AnsiVersion[];
extern short ospeed;
extern errno;
extern sys_nerr;
extern char *sys_errlist[];
extern char *rindex(), *malloc(), *getenv(), *MakeTermcap(), *ttyname();
static SigChld();
static char *Filename(), **SaveArgs();

static char PtyName[32], TtyName[32];
static char *ShellProg;
static char *ShellArgs[2];
static char inbuf[IOSIZE];
static inlen;
static ESCseen;
static GotSignal;
static char SockPath[512];
static char SockDir[] = ".screen";
static char *SockName;
static char *NewEnv[MAXARGS];
static char Esc = Ctrl('a');
static char MetaEsc = 'a';
static char *home;
static HasWindow;

struct mode {
    struct sgttyb m_ttyb;
    struct tchars m_tchars;
    struct ltchars m_ltchars;
    int m_ldisc;
    int m_lmode;
} OldMode, NewMode;

static struct win *curr, *other;
static CurrNum, OtherNum;
static struct win *wtab[MAXWIN];

#define MSG_CREATE    0
#define MSG_ERROR     1

struct msg {
    int type;
    union {
	struct {
	    int aflag;
	    int nargs;
	    char line[MAXLINE];
	} create;
	char message[MAXLINE];
    } m;
};

#define KEY_IGNORE         0
#define KEY_HARDCOPY       1
#define KEY_SUSPEND        2
#define KEY_SHELL          3
#define KEY_NEXT           4
#define KEY_PREV           5
#define KEY_KILL           6
#define KEY_REDISPLAY      7
#define KEY_WINDOWS        8
#define KEY_VERSION        9
#define KEY_OTHER         10
#define KEY_0             11
#define KEY_1             12
#define KEY_2             13
#define KEY_3             14
#define KEY_4             15
#define KEY_5             16
#define KEY_6             17
#define KEY_7             18
#define KEY_8             19
#define KEY_9             20
#define KEY_CREATE        21

struct key {
    int type;
    char **args;
} ktab[256];

char *KeyNames[] = {
    "hardcopy", "suspend", "shell", "next", "prev", "kill", "redisplay",
    "windows", "version", "other", "select0", "select1", "select2", "select3",
    "select4", "select5", "select6", "select7", "select8", "select9",
    0
};

main (ac, av) char **av; {
    register n, len;
    register struct win **pp, *p;
    char *ap;
    int s, r, w, x = 0;
    int aflag = 0;
    struct timeval tv;
    time_t now;
    char buf[IOSIZE], *myname = (ac == 0) ? "screen" : av[0];
    char rc[256];

    while (--ac) {
	ap = *++av;
	if (strcmp (ap, "-c") == 0) {
	    if (ac < 2)
		goto help;
	    CheckSockName (1);
	    s = MakeClientSocket ();
	    SendCreateMsg (s, ac, av, aflag);
	    close (s);
	    exit (0);
	} else if (strcmp (ap, "-a") == 0) {
	    aflag = 1;
	} else if (ap[0] == '-' && ap[1] == 'e') {
	    if (ap[2]) {
		ap += 2;
	    } else {
		if (--ac == 0) goto help;
		ap = *++av;
	    }
	    if (strlen (ap) != 2)
		Msg (0, "Two characters are required with -e option.");
	    Esc = ap[0];
	    MetaEsc = ap[1];
	} else {
help:
	    Msg (0, "Use: %s [-a] [-exy] [-c cmd args]", myname);
	}
    }
    if ((ShellProg = getenv ("SHELL")) == 0)
	ShellProg = DEFAULT_SHELL;
    ShellArgs[0] = ShellProg;
    CheckSockName (0);
    s = MakeServerSocket ();
    InitTerm ();
    MakeNewEnv ();
    GetTTY (0, &OldMode);
    ospeed = (short)OldMode.m_ttyb.sg_ospeed;
    InitKeytab ();
    sprintf (rc, "%.*s/.screenrc", 245, home);
    ReadRc (rc);
    if ((n = MakeWindow (ShellProg, ShellArgs, aflag, 0)) == -1) {
	SetTTY (0, &OldMode);
	FinitTerm ();
	exit (1);
    }
    SetCurrWindow (n);
    HasWindow = 1;
    SetMode (&OldMode, &NewMode);
    SetTTY (0, &NewMode);
    signal (SIGCHLD, SigChld);
    tv.tv_usec = 0;
    while (1) {
	if (status) {
	    time (&now);
	    if (now - TimeDisplayed < MSGWAIT) {
		tv.tv_sec = MSGWAIT - (now - TimeDisplayed);
	    } else RemoveStatus (curr);
	}
	r = 0;
	w = 0;
	if (inlen)
	    w |= 1 << curr->ptyfd;
	else
	    r |= 1 << 0;
	for (pp = wtab; pp < wtab+MAXWIN; ++pp) {
	    if (!(p = *pp))
		continue;
	    if ((*pp)->active && status)
		continue;
	    if ((*pp)->outlen > 0)
		continue;
	    r |= 1 << (*pp)->ptyfd;
	}
	r |= 1 << s;
	fflush (stdout);
	if (select (32, &r, &w, &x, status ? &tv : (struct timeval *)0) == -1) {
	    if (errno == EINTR)
		continue;
	    HasWindow = 0;
	    Msg (errno, "select");
	    /*NOTREACHED*/
	}
	if (GotSignal && !status) {
	    SigHandler ();
	    continue;
	}
	if (r & 1 << s) {
	    RemoveStatus (curr);
	    ReceiveMsg (s);
	}
	if (r & 1 << 0) {
	    RemoveStatus (curr);
	    if (ESCseen) {
		inbuf[0] = Esc;
		inlen = read (0, inbuf+1, IOSIZE-1) + 1;
		ESCseen = 0;
	    } else {
		inlen = read (0, inbuf, IOSIZE);
	    }
	    if (inlen > 0)
		inlen = ProcessInput (inbuf, inlen);
	}
	if (GotSignal && !status) {
	    SigHandler ();
	    continue;
	}
	for (pp = wtab; pp < wtab+MAXWIN; ++pp) {
	    if (!(p = *pp))
		continue;
	    if (p->outlen) {
		WriteString (p, p->outbuf, p->outlen);
	    } else if (r & 1 << p->ptyfd) {
		if ((len = read (p->ptyfd, buf, IOSIZE)) == -1) {
		    if (errno == EWOULDBLOCK)
			len = 0;
		}
		if (len > 0)
		    WriteString (p, buf, len);
	    }
	}
	if (GotSignal && !status) {
	    SigHandler ();
	    continue;
	}
	if (w & 1 << curr->ptyfd && inlen > 0) {
	    if ((len = write (curr->ptyfd, inbuf, inlen)) > 0) {
		inlen -= len;
		bcopy (inbuf+len, inbuf, inlen);
	    }
	}
	if (GotSignal && !status)
	    SigHandler ();
    }
    /*NOTREACHED*/
}

static SigHandler () {
    while (GotSignal) {
	GotSignal = 0;
	DoWait ();
    }
}

static SigChld () {
    GotSignal = 1;
}

static DoWait () {
    register pid;
    register struct win **pp;
    union wait wstat;

    while ((pid = wait3 (&wstat, WNOHANG|WUNTRACED, NULL)) > 0) {
	for (pp = wtab; pp < wtab+MAXWIN; ++pp) {
	    if (*pp && pid == (*pp)->wpid) {
		if (WIFSTOPPED (wstat)) {
		    kill((*pp)->wpid, SIGCONT);
		} else {
		    if (*pp == curr)
			curr = 0;
		    if (*pp == other)
			other = 0;
		    FreeWindow (*pp);
		    *pp = 0;
		}
	    }
	}
    }
    CheckWindows ();
}

static CheckWindows () {
    register struct win **pp;

    /* If the current window disappeared and the "other" window is still
     * there, switch to the "other" window, else switch to the window
     * with the lowest index.
     * If there current window is still there, but the "other" window
     * vanished, "SetCurrWindow" is called in order to assign a new value
     * to "other".
     * If no window is alive at all, exit.
     */
    if (!curr && other) {
	SwitchWindow (OtherNum);
	return;
    }
    if (curr && !other) {
	SetCurrWindow (CurrNum);
	return;
    }
    for (pp = wtab; pp < wtab+MAXWIN; ++pp) {
	if (*pp) {
	    if (!curr)
		SwitchWindow (pp-wtab);
	    return;
	}
    }
    SetTTY (0, &OldMode);
    FinitTerm ();
    exit (0);
}

InitKeytab () {
    register i;

    ktab['h'].type = ktab[Ctrl('h')].type = KEY_HARDCOPY;
    ktab['z'].type = ktab[Ctrl('z')].type = KEY_SUSPEND;
    ktab['c'].type = ktab[Ctrl('c')].type = KEY_SHELL;
    ktab[' '].type = ktab[Ctrl(' ')].type = 
    ktab['n'].type = ktab[Ctrl('n')].type = KEY_NEXT;
    ktab['-'].type = ktab['p'].type = ktab[Ctrl('p')].type = KEY_PREV;
    ktab['k'].type = ktab[Ctrl('k')].type = KEY_KILL;
    ktab['l'].type = ktab[Ctrl('l')].type = KEY_REDISPLAY;
    ktab['w'].type = ktab[Ctrl('w')].type = KEY_WINDOWS;
    ktab['v'].type = ktab[Ctrl('v')].type = KEY_VERSION;
    ktab[Esc].type = KEY_OTHER;
    for (i = 0; i <= 9; i++)
	ktab[i+'0'].type = KEY_0+i;
}

static ProcessInput (buf, len) char *buf; {
    register n, k;
    register char *s, *p;

    for (s = p = buf; len > 0; len--, s++) {
	if (*s == Esc) {
	    if (len > 1) {
		len--; s++;
		k = ktab[*s].type;
		if (*s == MetaEsc) {
		    *p++ = Esc;
		} else if (k >= KEY_0 && k <= KEY_9) {
		    p = buf;
		    SwitchWindow (k - KEY_0);
		} else switch (ktab[*s].type) {
		case KEY_HARDCOPY:
		    p = buf;
		    DumpWindow ();
		    break;
		case KEY_SUSPEND:
		    p = buf;
		    SetTTY (0, &OldMode);
		    FinitTerm ();
		    kill (getpid (), SIGTSTP);
		    SetTTY (0, &NewMode);
		    Activate (wtab[CurrNum]);
		    break;
		case KEY_SHELL:
		    p = buf;
		    if ((n = MakeWindow (ShellProg, ShellArgs, 0, 0)) != -1)
			SwitchWindow (n);
		    break;
		case KEY_NEXT:
		    p = buf;
		    SwitchWindow (NextWindow ());
		    break;
		case KEY_PREV:
		    p = buf;
		    SwitchWindow (PreviousWindow ());
		    break;
		case KEY_KILL:
		    p = buf;
		    FreeWindow (wtab[CurrNum]);
		    if (other == curr)
			other = 0;
		    curr = wtab[CurrNum] = 0;
		    CheckWindows ();
		    break;
		case KEY_REDISPLAY:
		    p = buf;
		    Activate (wtab[CurrNum]);
		    break;
		case KEY_WINDOWS:
		    p = buf;
		    ShowWindows ();
		    break;
		case KEY_VERSION:
		    p = buf;
		    Msg (0, "%s  %s", ScreenVersion, AnsiVersion);
		    break;
		case KEY_OTHER:
		    p = buf;
		    SwitchWindow (OtherNum);
		    break;
		case KEY_CREATE:
		    p = buf;
		    if ((n = MakeWindow (ktab[*s].args[0], ktab[*s].args,
			    0, 0)) != -1)
			SwitchWindow (n);
		    break;
		}
	    } else ESCseen = 1;
	} else *p++ = *s;
    }
    return p - buf;
}

static SwitchWindow (n) {
    if (!wtab[n])
	return;
    SetCurrWindow (n);
    Activate (wtab[n]);
}

static SetCurrWindow (n) {
    /*
     * If we come from another window, this window becomes the
     * "other" window:
     */
    if (curr) {
	curr->active = 0;
	other = curr;
	OtherNum = CurrNum;
    }
    CurrNum = n;
    curr = wtab[n];
    curr->active = 1;
    /*
     * If the "other" window is currently undefined (at program start
     * or because it has died), or if the "other" window is equal to the
     * one just selected, we try to find a new one:
     */
    if (other == 0 || other == curr) {
	OtherNum = NextWindow ();
	other = wtab[OtherNum];
    }
}

static NextWindow () {
    register struct win **pp;

    for (pp = wtab+CurrNum+1; pp != wtab+CurrNum; ++pp) {
	if (pp == wtab+MAXWIN)
	    pp = wtab;
	if (*pp)
	    break;
    }
    return pp-wtab;
}

static PreviousWindow () {
    register struct win **pp;

    for (pp = wtab+CurrNum-1; pp != wtab+CurrNum; --pp) {
	if (pp < wtab)
	    pp = wtab+MAXWIN-1;
	if (*pp)
	    break;
    }
    return pp-wtab;
}

static FreeWindow (wp) struct win *wp; {
    register i;

    close (wp->ptyfd);
    for (i = 0; i < rows; ++i) {
	free (wp->image[i]);
	free (wp->attr[i]);
    }
    free (wp->image);
    free (wp->attr);
    free (wp);
}

static MakeWindow (prog, args, aflag, StartAt) char *prog, **args; {
    register struct win **pp, *p;
    register char **cp;
    register n, f;
    int tf;
    int mypid;
    char ebuf[10];

    pp = wtab+StartAt;
    do {
	if (*pp == 0)
	    break;
	if (++pp == wtab+MAXWIN)
	    pp = wtab;
    } while (pp != wtab+StartAt);
    if (*pp) {
	Msg (0, "No more windows.");
	return -1;
    }
    n = pp - wtab;
    if ((f = OpenPTY ()) == -1) {
	Msg (0, "No more PTYs.");
	return -1;
    }
    fcntl (f, F_SETFL, FNDELAY);
    if ((p = *pp = (struct win *)malloc (sizeof (struct win))) == 0) {
nomem:
	Msg (0, "Out of memory.");
	return -1;
    }
    if ((p->image = (char **)malloc (rows * sizeof (char *))) == 0)
	goto nomem;
    for (cp = p->image; cp < p->image+rows; ++cp) {
	if ((*cp = malloc (cols)) == 0)
	    goto nomem;
	bclear (*cp, cols);
    }
    if ((p->attr = (char **)malloc (rows * sizeof (char *))) == 0)
	goto nomem;
    for (cp = p->attr; cp < p->attr+rows; ++cp) {
	if ((*cp = malloc (cols)) == 0)
	    goto nomem;
	bzero (*cp, cols);
    }
    if ((p->tabs = malloc (cols+1)) == 0)  /* +1 because 0 <= x <= cols */
	goto nomem;
    ResetScreen (p);
    p->active = 0;
    p->ptyfd = f;
    strncpy (p->cmd, Filename (args[0]), MAXSTR-1);
    p->cmd[MAXSTR-1] = '\0';
    switch (p->wpid = fork ()) {
    case -1:
	Msg (errno, "Cannot fork");
	free (p);
	return -1;
    case 0:
	mypid = getpid ();
	if ((f = open ("/dev/tty", O_RDWR)) != -1) {
	    ioctl (f, TIOCNOTTY, (char *)0);
	    close (f);
	}
	if ((tf = open (TtyName, O_RDWR)) == -1) {
	    SendErrorMsg ("Cannot open %s: %s", TtyName, sys_errlist[errno]);
	    exit (1);
	}
	dup2 (tf, 0);
	dup2 (tf, 1);
	dup2 (tf, 2);
	for (f = getdtablesize () - 1; f > 2; f--)
	    close (f);
	ioctl (0, TIOCSPGRP, &mypid);
	setpgrp (0, mypid);
	SetTTY (0, &OldMode);
	NewEnv[2] = MakeTermcap (aflag);
	sprintf (ebuf, "WINDOW=%d", n);
	NewEnv[3] = ebuf;
	execve (prog, args, NewEnv);
	SendErrorMsg ("Cannot exec %s: %s", prog, sys_errlist[errno]);
	sleep (2);
	exit (1);
    }
    return n;
}

static DumpWindow () {
    register i, j, k;
    register char *p;
    register FILE *f;
    char fn[20];

    sprintf (fn, "hardcopy.%d", CurrNum);
    if ((f = fopen (fn, "w")) == NULL) {
	Msg (0, "Cannot open \"%s\".", fn);
	return;
    }
    Msg (0, "Dumping screen image...");
    for (i = 0; i < rows; ++i) {
	p = curr->image[i];
	for (k = cols-1; k >= 0 && p[k] == ' '; --k) ;
	for (j = 0; j <= k; ++j)
	    putc (p[j], f);
	putc ('\n', f);
    }
    fclose (f);
    Msg (0, "Screen image written to \"%s\".", fn);
}

static ShowWindows () {
    char buf[1024];
    register char *s;
    register struct win **pp, *p;

    for (s = buf, pp = wtab; pp < wtab+MAXWIN; ++pp) {
	if ((p = *pp) == 0)
	    continue;
	if (s - buf + 5 + strlen (p->cmd) > cols-1)
	    break;
	if (s > buf) {
	    *s++ = ' '; *s++ = ' ';
	}
	*s++ = pp - wtab + '0';
	if (p == curr)
	    *s++ = '*';
	*s++ = ' ';
	strcpy (s, p->cmd);
	s += strlen (s);
    }
    Msg (0, buf);
}

static OpenPTY () {
    register char *p, *l, *d;
    register i, f, tf;

    strcpy (PtyName, PtyProto);
    strcpy (TtyName, TtyProto);
    for (p = PtyName, i = 0; *p != 'X'; ++p, ++i) ;
    for (l = "pqr"; *p = *l; ++l) {
	for (d = "0123456789abcdef"; p[1] = *d; ++d) {
	    if ((f = open (PtyName, O_RDWR)) != -1) {
		TtyName[i] = p[0];
		TtyName[i+1] = p[1];
		if ((tf = open (TtyName, O_RDWR)) != -1) {
		    close (tf);
		    return f;
		}
		close (f);
	    }
	}
    }
    return -1;
}

static SetTTY (fd, mp) struct mode *mp; {
    ioctl (fd, TIOCSETP, &mp->m_ttyb);
    ioctl (fd, TIOCSETC, &mp->m_tchars);
    ioctl (fd, TIOCSLTC, &mp->m_ltchars);
    ioctl (fd, TIOCLSET, &mp->m_lmode);
    ioctl (fd, TIOCSETD, &mp->m_ldisc);
}

static GetTTY (fd, mp) struct mode *mp; {
    ioctl (fd, TIOCGETP, &mp->m_ttyb);
    ioctl (fd, TIOCGETC, &mp->m_tchars);
    ioctl (fd, TIOCGLTC, &mp->m_ltchars);
    ioctl (fd, TIOCLGET, &mp->m_lmode);
    ioctl (fd, TIOCGETD, &mp->m_ldisc);
}

static SetMode (op, np) struct mode *op, *np; {
    *np = *op;
    np->m_ttyb.sg_flags &= ~(CRMOD|ECHO);
    np->m_ttyb.sg_flags |= CBREAK;
    np->m_tchars.t_intrc = -1;
    np->m_tchars.t_quitc = -1;
    np->m_ltchars.t_suspc = -1;
    np->m_ltchars.t_dsuspc = -1;
    np->m_ltchars.t_flushc = -1;
    np->m_ltchars.t_lnextc = -1;
}

static CheckSockName (client) {
    struct stat s;
    register char *p;

    if (client) {
	if ((SockName = getenv ("STY")) == 0 || *SockName == '\0')
	    Msg (0, "$STY is undefined or invalid.");
    } else {
	if ((p = ttyname (0)) == 0 || (p = ttyname (1)) == 0 ||
		(p = ttyname (2)) == 0 || *p == '\0')
	    Msg (0, "screen must run on a tty.");
	SockName = Filename (p);
    }
    if ((home = getenv ("HOME")) == 0)
	Msg (0, "$HOME is undefined.");
    sprintf (SockPath, "%s/%s", home, SockDir);
    if (stat (SockPath, &s) == -1) {
	if (errno == ENOENT) {
	    if (mkdir (SockPath, 0700) == -1)
		Msg (errno, "Cannot make directory %s", SockPath);
	} else Msg (errno, "Cannot get status of %s", SockPath);
    } else {
	if ((s.st_mode & S_IFMT) != S_IFDIR)
	    Msg (0, "%s is not a directory.", SockPath);
	if ((s.st_mode & 0777) != 0700)
	    Msg (0, "Directory %s must have mode 700.", SockPath);
    }
    strcat (SockPath, "/");
    strcat (SockPath, SockName);
}

static MakeServerSocket () {
    register s;
    struct sockaddr_un sun;

    (void) unlink (SockPath);
    if ((s = socket (AF_UNIX, SOCK_STREAM, 0)) == -1)
	Msg (errno, "socket");
    sun.sun_family = AF_UNIX;
    strcpy (sun.sun_path, SockPath);
    if (bind (s, (struct sockaddr *)&sun, strlen (SockPath)+2) == -1)
	Msg (errno, "bind");
    if (listen (s, 5) == -1)
	Msg (errno, "listen");
    return s;
}

static MakeClientSocket () {
    register s;
    struct sockaddr_un sun;

    if ((s = socket (AF_UNIX, SOCK_STREAM, 0)) == -1)
	Msg (errno, "socket");
    sun.sun_family = AF_UNIX;
    strcpy (sun.sun_path, SockPath);
    if (connect (s, (struct sockaddr *)&sun, strlen (SockPath)+2) == -1)
	Msg (errno, "connect: %s", SockPath);
    return s;
}

static SendCreateMsg (s, ac, av, aflag) char **av; {
    struct msg m;
    register char *p;
    register len, n;

    m.type = MSG_CREATE;
    for (p = m.m.create.line, n = 0; --ac && n < MAXARGS-1; ++n) {
	len = strlen (*++av) + 1;
	if (p + len >= m.m.create.line+MAXLINE)
	    break;
	strcpy (p, *av);
	p += len;
    }
    m.m.create.nargs = n;
    m.m.create.aflag = aflag;
    if (write (s, &m, sizeof (m)) != sizeof (m))
	Msg (errno, "write");
}

/*VARARGS1*/
static SendErrorMsg (fmt, p1, p2, p3, p4, p5, p6) char *fmt; {
    register s;
    struct msg m;

    s = MakeClientSocket ();
    m.type = MSG_ERROR;
    sprintf (m.m.message, fmt, p1, p2, p3, p4, p5, p6);
    (void) write (s, &m, sizeof (m));
    close (s);
}

static ReceiveMsg (s) {
    register ns;
    struct sockaddr_un sun;
    int len = sizeof (sun);
    struct msg m;

    if ((ns = accept (s, (struct sockaddr *)&sun, &len)) == -1) {
	Msg (errno, "accept");
	return;
    }
    if ((len = read (ns, &m, sizeof (m))) != sizeof (m)) {
	if (len == -1)
	    Msg (errno, "read");
	else
	    Msg (0, "Short message (%d bytes)", len);
	close (ns);
	return;
    }
    switch (m.type) {
    case MSG_CREATE:
	ExecCreate (&m);
	break;
    case MSG_ERROR:
	Msg (0, "%s", m.m.message);
	break;
    default:
	Msg (0, "Invalid message (type %d).", m.type);
    }
    close (ns);
}

static ExecCreate (mp) struct msg *mp; {
    char *args[MAXARGS];
    register n;
    register char **pp = args, *p = mp->m.create.line;

    for (n = mp->m.create.nargs; n > 0; --n) {
	*pp++ = p;
	p += strlen (p) + 1;
    }
    *pp = 0;
    if ((n = MakeWindow (mp->m.create.line, args, mp->m.create.aflag, 0)) != -1)
	SwitchWindow (n);
}

static ReadRc (fn) char *fn; {
    FILE *f;
    register char *p, **pp, **ap;
    register argc, num, c;
    char buf[256];
    char *args[MAXARGS];
    int key;

    ap = args;
    if ((f = fopen (fn, "r")) == NULL)
	return;
    while (fgets (buf, 256, f) != NULL) {
	if (p = rindex (buf, '\n'))
	    *p = '\0';
	if ((argc = Parse (fn, buf, ap)) == 0)
	    continue;
	if (strcmp (ap[0], "escape") == 0) {
	    p = ap[1];
	    if (argc < 2 || strlen (p) != 2)
		Msg (0, "%s: two characters required after escape.", fn);
	    Esc = *p++;
	    MetaEsc = *p;
	} else if (strcmp (ap[0], "screen") == 0) {
	    num = 0;
	    if (argc > 1 && IsNum (ap[1], 10)) {
		num = atoi (ap[1]);
		if (num < 0 || num > MAXWIN-1)
		    Msg (0, "%s: illegal screen number %d.", fn, num);
		--argc; ++ap;
	    }
	    if (argc < 2) {
		ap[1] = ShellProg; argc = 2;
	    }
	    ap[argc] = 0;
	    (void) MakeWindow (ap[1], ap+1, 0, num);
	} else if (strcmp (ap[0], "bind") == 0) {
	    p = ap[1];
	    if (argc < 2 || *p == '\0')
		Msg (0, "%s: key expected after bind.", fn);
	    if (p[1] == '\0') {
		key = *p;
	    } else if (p[0] == '^' && p[1] != '\0' && p[2] == '\0') {
		c = p[1];
		if (isupper (c))
		    p[1] = tolower (c);    
		key = Ctrl(c);
	    } else if (IsNum (p, 7)) {
		(void) sscanf (p, "%o", &key);
	    } else {
		Msg (0,
		    "%s: bind: character, ^x, or octal number expected.", fn);
	    }
	    if (argc < 3) {
		ktab[key].type = 0;
	    } else {
		for (pp = KeyNames; *pp; ++pp)
		    if (strcmp (ap[2], *pp) == 0) break;
		if (*pp) {
		    ktab[key].type = pp-KeyNames+1;
		} else if (ap[2][0] == '/') {
		    ktab[key].type = KEY_CREATE;
		    ktab[key].args = SaveArgs (argc-2, ap+2);
		} else Msg (0, "%s: unknown function \"%s\".", fn, ap[2]);
	    }
	} else Msg (0, "%s: unknown keyword \"%s\".", fn, ap[0]);
    }
    fclose (f);
}

static Parse (fn, buf, args) char *fn, *buf, **args; {
    register char *p = buf, **ap = args;
    register delim, argc = 0;

    argc = 0;
    for (;;) {
	while (*p && (*p == ' ' || *p == '\t')) ++p;
	if (*p == '\0' || *p == '#')
	    return argc;
	if (argc > MAXARGS-1)
	    Msg (0, "%s: too many tokens.", fn);
	delim = 0;
	if (*p == '"' || *p == '\'') {
	    delim = *p; *p = '\0'; ++p;
	}
	++argc;
	*ap = p; ++ap;
	while (*p && !(delim ? *p == delim : (*p == ' ' || *p == '\t')))
	    ++p;
	if (*p == '\0') {
	    if (delim)
		Msg (0, "%s: Missing quote.", fn);
	    else
		return argc;
	}
	*p++ = '\0';
    }
}

static char **SaveArgs (argc, argv) register argc; register char **argv; {
    register char **ap, **pp;

    if ((pp = ap = (char **)malloc ((argc+1) * sizeof (char **))) == 0)
	Msg (0, "Out of memory.");
    while (argc--) {
	if ((*pp = malloc (strlen (*argv)+1)) == 0)
	    Msg (0, "Out of memory.");
	strcpy (*pp, *argv);
	++pp; ++argv;
    }
    *pp = 0;
    return ap;
}

static MakeNewEnv () {
    register char **op, **np = NewEnv;
    static char buf[MAXSTR];

    if (strlen (SockName) > MAXSTR-5)
	SockName = "?";
    sprintf (buf, "STY=%s", SockName);
    *np++ = buf;
    *np++ = Term;
    np += 2;
    for (op = environ; *op; ++op) {
	if (np == NewEnv + MAXARGS - 1)
	    break;
	if (!IsSymbol (*op, "TERM") && !IsSymbol (*op, "TERMCAP")
		&& !IsSymbol (*op, "STY"))
	    *np++ = *op;
    }
    *np = 0;
}

static IsSymbol (e, s) register char *e, *s; {
    register char *p;
    register n;

    for (p = e; *p && *p != '='; ++p) ;
    if (*p) {
	*p = '\0';
	n = strcmp (e, s);
	*p = '=';
	return n == 0;
    }
    return 0;
}

/*VARARGS2*/
Msg (err, fmt, p1, p2, p3, p4, p5, p6) char *fmt; {
    char buf[1024];
    register char *p = buf;

    sprintf (p, fmt, p1, p2, p3, p4, p5, p6);
    if (err) {
	p += strlen (p);
	if (err > 0 && err < sys_nerr)
	    sprintf (p, ": %s", sys_errlist[err]);
	else
	    sprintf (p, ": Error %d", err);
    }
    if (HasWindow) {
	MakeStatus (buf, curr);
    } else {
	printf ("%s\r\n", buf);
	exit (1);
    }
}

bclear (p, n) char *p; {
    bcopy (blank, p, n);
}

static char *Filename (s) char *s; {
    register char *p;

    p = s + strlen (s) - 1;
    while (p >= s && *p != '/') --p;
    return ++p;
}

static IsNum (s, base) register char *s; register base; {
    for (base += '0'; *s; ++s)
	if (*s < '0' || *s > base)
	    return 0;
    return 1;
}
