/*
 * Telnet front end for the player I/O layer: scepterd's default mode.
 *
 * On QNX each player ran a copy of qnx/mud.c (under inetd) that talked to
 * the server by Send/Receive.  Here the server listens itself and serves up
 * to MaxUsers connections from one select() loop, translating the messages
 * of the protocol described in ipc.c much as mud.c did:
 *
 *   0<text>	text and CRLF
 *   1<prompt>	the prompt without a newline, then IAC GA; wait for a line.
 *				Output for the player is held until the line arrives, as
 *				the QNX ring buffer held it until mud.c asked again.
 *   1			(empty) command mode: if no line arrives within PollMs,
 *				hand the game an empty line, as an idle client did
 *   2<text>	text, then close the connection
 *   3			"End - aborted", then close
 *   5<pkg> <json>	a GMCP message, for a client that accepted GMCP; it is
 *				held at a prompt, like text, and dropped for any other client
 *
 * GMCP (telnet option 201, #189) is offered with IAC WILL GMCP when a
 * connection opens, and used once the client answers IAC DO GMCP.  IAC DONT
 * GMCP turns it off without a reply.  What the client sends by GMCP (its
 * Core.Hello and Core.Supports.Set) is read and ignored.  Every other option
 * is refused (DO -> WONT, WILL -> DONT), so clients stay in line mode and
 * echo locally.  Control characters are stripped from
 * input, empty lines are ignored, and lines are capped at LineMax (mud.c
 * allowed 255, one more than its 256-byte message buffer held).
 *
 * Connection ids are slot numbers 1..MaxUsers; they become the player's
 * Pid and Trm.  A slot is not reused until the player on it has logged off,
 * so a stale Trm never reaches someone else.  A lost connection is logged
 * off by CheckHup(), which NetRea() calls before handing the game anything
 * else, so it never runs inside another player's command.
 *
 * NetRea() returns "1" with channel 0 when nothing happens for a second
 * (FindPid(0) is NULL, so main() runs MonAttack() and prompts nobody), and
 * "9" after SIGINT or SIGTERM, which shuts the server down.  SIGUSR1 and
 * SIGUSR2 give "7R" and "7X", the warnings of a coming restart or reset
 * (#136).
 */
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "common.h"
#include "scepter.h"

#define	PollMs		500			/* idle poll, as qnx/inter.c */
#define	TickMs		1000		/* MonAttack() needs a call once a second */
#define	LineMax		254			/* longest input line: C1 holds "1", it and NUL */

#define	TnSE		240
#define	TnGA		249
#define	TnSB		250
#define	TnWILL		251
#define	TnWONT		252
#define	TnDO		253
#define	TnDONT		254
#define	TnIAC		255

#define	OptGMCP		201

enum { CFree, CNew, CBusy, CPrompt, CPoll, CClose, CHup };	/* Conn.State */
enum { TText, TIac, TOpt, TSub, TSubIac };					/* Conn.Tn */

typedef struct
{
	int		State;
	int		Fd;
	int		Tn;				/* telnet input state */
	int		Verb;			/* WILL/WONT/DO/DONT awaiting its option */
	int		Gmcp;			/* the client accepted GMCP */
	int		GmcpOffer;		/* our IAC WILL GMCP awaits its answer */
	long	PollAt;			/* CPoll: when to send the game an empty line */
	time_t	LastIn;			/* for Who() */
	char	Host[64];

	char	In[1024];		/* complete lines, each ended by '\n' */
	int		InLen;
	int		LineLen;		/* characters in the line being received */

	char	Out[65536];
	int		OutLen;
	int		Ready;			/* Out[0..Ready) may be sent; the rest is held */
} ConnType;

static ConnType	Conn[MaxUsers + 1];		/* Conn[0] is unused */
static int		Listener = -1;
static int		LastPick;
static long		LastTick;
static volatile sig_atomic_t Stop;
static volatile sig_atomic_t Warn;		/* 'R' or 'X', for NetRea() */

static long Now(void)
{
	struct timespec	ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void OnStop(int sig)
{
	UNUSED(sig);
	Stop = TRUE;
}

static void OnWarn(int sig)
{
	Warn = sig == SIGUSR2 ? 'X' : 'R';
}

static void Drop(ConnType *C)
{
	if (C->Fd >= 0)
	{
		close(C->Fd);
		fprintf(stderr, "scepterd: connection %d closed\n", (int)(C - Conn));
	}
	C->Fd = -1;
	C->OutLen = C->Ready = 0;
	C->InLen = C->LineLen = 0;
	/* a player still on the slot is logged off by CheckHup() */
	C->State = FindPid((int)(C - Conn)) ? CHup : CFree;
}

/* Queue Len bytes of output; Send makes them (and anything held) ready. */
static void Put(ConnType *C, const char *Data, int Len, int Send)
{
	if (C->State == CFree || C->State == CHup)
		return;
	if (C->OutLen + Len > (int)sizeof(C->Out))
	{
		Drop(C);	/* the player has stopped reading */
		return;
	}
	memcpy(C->Out + C->OutLen, Data, Len);
	C->OutLen += Len;
	if (Send)
		C->Ready = C->OutLen;
}

/* Telnet replies go out at once, ahead of any held text. */
static void Reply(ConnType *C, int Verb, int Opt)
{
	char	Cmd[3];

	if (C->OutLen + 3 > (int)sizeof(C->Out))
		return;
	Cmd[0] = (char)TnIAC;
	Cmd[1] = (char)Verb;
	Cmd[2] = (char)Opt;
	memmove(C->Out + C->Ready + 3, C->Out + C->Ready, C->OutLen - C->Ready);
	memcpy(C->Out + C->Ready, Cmd, 3);
	C->OutLen += 3;
	C->Ready += 3;
}

static void Text(ConnType *C, int c)
{
	if (c == '\r' || c == '\n')
	{
		if (C->LineLen && C->InLen < (int)sizeof(C->In))
		{
			C->In[C->InLen++] = '\n';
			C->LineLen = 0;
		}
	}
	else if (c >= ' ' && c <= '~'
		&&	 C->LineLen < LineMax && C->InLen < (int)sizeof(C->In) - 1)
	{
		C->In[C->InLen++] = (char)c;
		C->LineLen++;
	}
}

static void Telnet(ConnType *C, int c)
{
	switch (C->Tn)
	{
		case TText:
			if (c == TnIAC)
				C->Tn = TIac;
			else
				Text(C, c);
			break;

		case TIac:
			C->Tn = TText;
			if (c >= TnWILL && c <= TnDONT)
			{
				C->Verb = c;
				C->Tn = TOpt;
			}
			else if (c == TnSB)
				C->Tn = TSub;
			else if (c == TnIAC)
				Text(C, c);
			break;

		case TOpt:
			if (c == OptGMCP && C->Verb == TnDO)
			{
				if (!C->Gmcp && !C->GmcpOffer)
					Reply(C, TnWILL, c);	/* asked again after a DONT */
				C->Gmcp = TRUE;
				C->GmcpOffer = FALSE;
			}
			else if (c == OptGMCP && C->Verb == TnDONT)
				C->Gmcp = C->GmcpOffer = FALSE;
			else if (C->Verb == TnDO)
				Reply(C, TnWONT, c);
			else if (C->Verb == TnWILL)
				Reply(C, TnDONT, c);
			C->Tn = TText;
			break;

		case TSub:
			if (c == TnIAC)
				C->Tn = TSubIac;
			break;

		case TSubIac:
			C->Tn = c == TnSE ? TText : TSub;
			break;
	}
}

/* Copy the first complete input line into Str. */
static int TakeLine(ConnType *C, char *Str)
{
	char	*nl = memchr(C->In, '\n', C->InLen);
	int		len;

	if (!nl)
		return FALSE;
	len = (int)(nl - C->In);
	memcpy(Str, C->In, len);
	Str[len] = '\0';
	len++;
	memmove(C->In, C->In + len, C->InLen - len);
	C->InLen -= len;
	return TRUE;
}

static void Flush(ConnType *C)
{
	ssize_t	n;

	if (C->Fd < 0 || !C->Ready)
		return;
	n = write(C->Fd, C->Out, C->Ready);
	if (n < 0)
	{
		if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
			Drop(C);
		return;
	}
	memmove(C->Out, C->Out + n, C->OutLen - n);
	C->OutLen -= (int)n;
	C->Ready -= (int)n;
}

static void FlushAll(void)
{
	int		i;

	for (i = 1; i <= MaxUsers; i++)
	{
		Flush(&Conn[i]);
		if (Conn[i].State == CClose && !Conn[i].OutLen)
			Drop(&Conn[i]);
	}
}

/* At exit, give each player the last of their output (Off()'s "3"). */
static void Finish(void)
{
	int		i;

	for (i = 1; i <= MaxUsers; i++)
		if (Conn[i].Fd >= 0)
		{
			struct timeval	tv = { 1, 0 };
			int				fl = fcntl(Conn[i].Fd, F_GETFL);

			fcntl(Conn[i].Fd, F_SETFL, fl & ~O_NONBLOCK);
			setsockopt(Conn[i].Fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
			Conn[i].Ready = Conn[i].OutLen;
			while (Conn[i].Fd >= 0 && Conn[i].OutLen)
			{
				int		was = Conn[i].OutLen;

				Flush(&Conn[i]);
				if (Conn[i].OutLen == was)
					break;
			}
			if (Conn[i].Fd >= 0)
				close(Conn[i].Fd);
			Conn[i].Fd = -1;
		}
}

void NetListen(char *Addr)
{
	struct addrinfo	hints, *res, *ai;
	char			host[256], *port, *p;
	int				i, err, pass, on = 1, off = 0;

	/* [HOST:]PORT; HOST may be bracketed, and empty means every address */
	snprintf(host, sizeof host, "%s", Addr);
	if ((p = strrchr(host, ':')))
	{
		*p = '\0';
		port = p + 1;
		p = host;
		if (*p == '[' && p[strlen(p) - 1] == ']')
		{
			p[strlen(p) - 1] = '\0';
			p++;
		}
	}
	else
	{
		port = host;
		p = "127.0.0.1";
	}

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;
	if ((err = getaddrinfo(*p ? p : NULL, port, &hints, &res)))
	{
		fprintf(stderr, "scepterd: %s: %s\n", Addr, gai_strerror(err));
		exit(1);
	}
	/* for every address, try a dual-stack IPv6 socket first (pass 0) */
	for (pass = *p ? 1 : 0; pass < 2 && Listener < 0; pass++)
		for (ai = res; ai; ai = ai->ai_next)
		{
			if (!pass && ai->ai_family != AF_INET6)
				continue;
			if ((Listener = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol)) < 0)
				continue;
			setsockopt(Listener, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
			if (!pass)
				setsockopt(Listener, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof off);
			if (!bind(Listener, ai->ai_addr, ai->ai_addrlen) && !listen(Listener, 16))
				break;
			close(Listener);
			Listener = -1;
		}
	freeaddrinfo(res);
	if (Listener < 0)
	{
		fprintf(stderr, "scepterd: cannot listen on %s: %s\n", Addr, strerror(errno));
		exit(1);
	}
	fcntl(Listener, F_SETFL, fcntl(Listener, F_GETFL) | O_NONBLOCK);
	fprintf(stderr, "scepterd: listening on %s\n", Addr);

	for (i = 0; i <= MaxUsers; i++)
	{
		Conn[i].State = CFree;
		Conn[i].Fd = -1;
	}
	signal(SIGPIPE, SIG_IGN);
	signal(SIGINT, OnStop);
	signal(SIGTERM, OnStop);
	signal(SIGUSR1, OnWarn);
	signal(SIGUSR2, OnWarn);
	atexit(Finish);
	LastTick = Now();
}

static void Answer(void)
{
	struct sockaddr_storage	sa;
	socklen_t				len = sizeof sa;
	ConnType				*C;
	int						fd, i;

	if ((fd = accept(Listener, (struct sockaddr *)&sa, &len)) < 0)
		return;
	for (i = 1; i <= MaxUsers && Conn[i].State != CFree; i++)
		;
	if (i > MaxUsers)
	{
		static const char	full[] = "Too many users\r\n";

		if (write(fd, full, sizeof full - 1) < 0)
			;	/* nothing more can be done for them */
		close(fd);
		return;
	}
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);

	C = &Conn[i];
	C->State = CNew;
	C->Fd = fd;
	C->Tn = TText;
	C->InLen = C->LineLen = 0;
	C->OutLen = C->Ready = 0;
	C->Gmcp = FALSE;
	C->GmcpOffer = TRUE;
	Reply(C, TnWILL, OptGMCP);
	C->LastIn = time(NULL);
	if (getnameinfo((struct sockaddr *)&sa, len, C->Host, sizeof C->Host,
					NULL, 0, NI_NUMERICHOST))
		strcpy(C->Host, "?");
	fprintf(stderr, "scepterd: connection %d from %s\n", i, C->Host);
}

static void Receive(ConnType *C)
{
	unsigned char	buf[1024];
	ssize_t			n, i;

	n = read(C->Fd, buf, sizeof buf);
	if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
		return;
	if (n <= 0)
	{
		Drop(C);
		return;
	}
	for (i = 0; i < n; i++)
		Telnet(C, buf[i]);
}

/* Find the next message for the game, taking connections in turn. */
static int Pick(char *Str, int *Chn, long now)
{
	int		k;

	for (k = 1; k <= MaxUsers; k++)
	{
		int			i = (LastPick + k - 1) % MaxUsers + 1;
		ConnType	*C = &Conn[i];

		*Chn = i;
		if (C->State == CNew)
			strcpy(Str, "0");
		else if ((C->State == CPrompt || C->State == CPoll)
			&&	 TakeLine(C, Str + 1))
		{
			Str[0] = '1';
			C->LastIn = time(NULL);
			C->Ready = C->OutLen;	/* release text held at a prompt */
		}
		else if (C->State == CPoll && now >= C->PollAt)
			strcpy(Str, "1");
		else
			continue;

		C->State = CBusy;
		LastPick = i;
		return TRUE;
	}
	return FALSE;
}

void NetRea(char *Str, int *Chn)
{
	for (;;)
	{
		fd_set			rfd, wfd;
		struct timeval	tv;
		long			now, wait;
		int				i, max;

		FlushAll();
		for (i = 1; i <= MaxUsers; i++)
			if (Conn[i].State == CHup)
			{
				CheckHup();
				break;
			}

		now = Now();
		if (Stop)
		{
			*Chn = 0;
			strcpy(Str, "9");
			return;
		}
		if (Warn)
		{
			*Chn = 0;
			Str[0] = '7';
			Str[1] = (char)Warn;
			Str[2] = '\0';
			Warn = 0;
			return;
		}
		if (Pick(Str, Chn, now))
		{
			LastTick = now;
			return;
		}
		if (now >= LastTick + TickMs)
		{
			*Chn = 0;
			strcpy(Str, "1");
			LastTick = now;
			return;
		}

		wait = LastTick + TickMs - now;
		FD_ZERO(&rfd);
		FD_ZERO(&wfd);
		FD_SET(Listener, &rfd);
		max = Listener;
		for (i = 1; i <= MaxUsers; i++)
		{
			ConnType	*C = &Conn[i];

			if (C->Fd < 0)
				continue;
			FD_SET(C->Fd, &rfd);
			if (C->Ready)
				FD_SET(C->Fd, &wfd);
			if (C->Fd > max)
				max = C->Fd;
			if (C->State == CPoll && C->PollAt - now < wait)
				wait = C->PollAt - now;
		}
		tv.tv_sec = wait / 1000;
		tv.tv_usec = wait % 1000 * 1000;
		if (select(max + 1, &rfd, &wfd, NULL, &tv) <= 0)
			continue;

		if (FD_ISSET(Listener, &rfd))
			Answer();
		for (i = 1; i <= MaxUsers; i++)
			if (Conn[i].Fd >= 0 && FD_ISSET(Conn[i].Fd, &rfd))
				Receive(&Conn[i]);
	}
}

void NetOut(int Chn, char *Str)
{
	ConnType	*C;

	if (Chn < 1 || Chn > MaxUsers)
		return;
	C = &Conn[Chn];
	if (C->State == CFree || C->State == CHup || C->State == CClose)
		return;		/* gone, or going: e.g. a prompt after "2" */

	switch (*Str)
	{
		case '0':
			Put(C, Str + 1, (int)strlen(Str + 1), FALSE);
			Put(C, "\r\n", 2, C->State != CPrompt);
			break;

		case '1':
			if (Str[1])
			{
				static const char	ga[] = { (char)TnIAC, (char)TnGA };

				Put(C, Str + 1, (int)strlen(Str + 1), TRUE);
				Put(C, ga, 2, TRUE);
				C->State = CPrompt;
			}
			else
			{
				C->Ready = C->OutLen;
				C->State = CPoll;
				C->PollAt = Now() + PollMs;
			}
			break;

		case '2':
		case '3':
			if (*Str == '3')
				Put(C, "End - aborted\r\n", 15, TRUE);
			else if (Str[1])
			{
				Put(C, Str + 1, (int)strlen(Str + 1), TRUE);
				Put(C, "\r\n", 2, TRUE);
			}
			C->Ready = C->OutLen;
			if (C->State != CFree && C->State != CHup)
				C->State = CClose;
			break;

		case '5':
			if (C->Gmcp)
			{
				static const char	sb[] = { (char)TnIAC, (char)TnSB, (char)OptGMCP },
									se[] = { (char)TnIAC, (char)TnSE };
				char	*p, *q;

				Put(C, sb, 3, FALSE);
				/* IAC in the data is doubled (UTF-8 JSON never has one) */
				for (p = Str + 1; (q = strchr(p, (char)TnIAC)); p = q + 1)
				{
					Put(C, p, (int)(q - p + 1), FALSE);
					Put(C, q, 1, FALSE);
				}
				Put(C, p, (int)strlen(p), FALSE);
				Put(C, se, 2, C->State != CPrompt);
			}
			break;
	}
}

void NetFlush(void)
{
	FlushAll();
}

void NetWho(void)
{
	UserPoint	Player;
	time_t		now = time(NULL);
	int			i;

	QOut(Term, "0");
	if (!User->Brief)
		QOut(Term, "0Ch   Idle  Name");
	for (i = 1; i <= MaxUsers; i++)
	{
		long	idle;
		char	*name = "(logging in)", *task = "";

		if (Conn[i].Fd < 0 || Conn[i].State == CClose)
			continue;
		if ((Player = FindPid(i)) && Player->Status != SLogin)
		{
			name = Player->Name;
			if (Player->NonExistant)
				task = " (talk)";
		}
		idle = (long)(now - Conn[i].LastIn);
		snprintf(B1, sizeof(B1), "0%2d %3ld:%02ld  %s%s", i, idle / 60, idle % 60, name, task);
		QOut(Term, B1);
	}
}

const char *NetHost(int Chn)
{
	return Chn >= 1 && Chn <= MaxUsers ? Conn[Chn].Host : "?";
}

void NetCheckHup(void)
{
	UserPoint	Plyr = UserTail;
	int			i;

	while (Plyr)
	{
		int		Count, ILoop;

		if (Plyr->Trm < 1 || Plyr->Trm > MaxUsers
		||	Conn[Plyr->Trm].State != CHup)
		{
			Plyr = Plyr->NextUser;
			continue;
		}

		/* MsgTerm() hears from User, so make the departing player User */
		User = Plyr;
		Term = Plyr->Trm;
		CmdCode = 0;
		if (Plyr->Status != SLogin)
		{
			snprintf(B1, sizeof(B1), "0### %s was devoured by US West Communications.",
					Plyr->Name);
			Count = MsgTerm(BrAll);
			for (ILoop = 0; ILoop < Count; ILoop++)
				if (TermList[ILoop] != Term)
					QOut(TermList[ILoop], B1);
		}
		Logoff(Plyr);
		User = NULL;
		Plyr = UserTail;
	}

	for (i = 1; i <= MaxUsers; i++)
		if (Conn[i].State == CHup && !FindPid(i))
			Conn[i].State = CFree;
}
