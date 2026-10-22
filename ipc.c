/*
 * Player I/O layer, and the console harness.
 *
 * The QNX version received messages from the scepter/mud client tasks with
 * Send/Receive/Reply and queued replies in a shared ring buffer.  scepterd
 * now serves telnet connections itself (telnet.c) once QListen() is called;
 * otherwise (scepterd -c, and tools/fightsim) this file plays the part of a
 * single client on stdin/stdout so the game can be run and tested locally.
 *
 * Protocol (unchanged from QNX), first character of each message:
 *   client -> server: 0 login, 1<text> input, 4 hang up
 *   server -> client: 0<text> print line, 1<prompt> read input
 *                     (empty prompt: poll, reply with empty input if idle),
 *                     2<text> print and disconnect, 3 aborted,
 *                     5<package> <json> a GMCP message (#189)
 *
 * Only telnet sends GMCP, to clients that accepted it; the console ignores
 * it.  QGmcp() makes the message.  Before a "2" or "3" a telnet client is
 * sent Core.Goodbye, with the text it is about to see ("Goodbye." if none).
 *
 * QRea() returns "9" when stdin reaches end of file (telnet: on SIGINT or
 * SIGTERM); main() then shuts down.  With telnet it also returns "7R" on
 * SIGUSR1 and "7X" on SIGUSR2, the warning of a restart or a reset (#136).
 */
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

#include "common.h"
#include "scepter.h"

#define	ConsolePid	1		/* the one and only "task" talking to us */

static short *EncSegment;
static short *ObjSegment;

static char	Prompt[256];		/* last '1' message for the console */
static int	HavePrompt;
static int	Connected;			/* FALSE until the console has logged in */

static int	Net;				/* TRUE: players connect by telnet */

static char	InBuf[512];			/* raw bytes read from stdin */
static int	InLen;
static int	InEOF;

/* Serve telnet connections on Addr ([HOST:]PORT) instead of the console. */
void QListen(char *Addr)
{
	Net = TRUE;
	NetListen(Addr);
}

void QAss(char *Mbx, int *Chn)
{
	UNUSED(Mbx);
	UNUSED(Chn);
}

void QCre(void)
{
	EncSegment = (short *)calloc((LEncounter + 1) * 8, sizeof(short));
	ObjSegment = (short *)calloc((ObjListLen + 1) * 8, sizeof(short));
	if (!EncSegment || !ObjSegment)
		Abort(" ipc: table allocation failed.");
	HavePrompt = FALSE;
	Connected = FALSE;
}

int GetETIndex(int table, int row, int col)
{
	short	*Index = table ? ObjSegment : EncSegment;

	return Index[row * 8 + col];
}

void PutETIndex(int table, int row, int col, int val)
{
	short	*Index = table ? ObjSegment : EncSegment;

	Index[row * 8 + col] = (short)val;
}

void QDea(int Chn)
{
	UNUSED(Chn);
}

/* Return TRUE if a complete line is buffered, copying it (without the
 * newline, control characters removed) into Str. */
static int TakeLine(char *Str, int Max)
{
	char	*nl = memchr(InBuf, '\n', InLen);
	int		i, n, len;

	if (!nl && !(InEOF && InLen))
		return FALSE;

	len = nl ? (int)(nl - InBuf) : InLen;
	for (i = n = 0; i < len; i++)
		if (InBuf[i] >= ' ' && InBuf[i] <= '~' && n < Max - 1)
			Str[n++] = InBuf[i];
	Str[n] = '\0';

	if (nl)
		len++;
	memmove(InBuf, InBuf + len, InLen - len);
	InLen -= len;
	return TRUE;
}

/* Read more input, waiting at most TimeoutUsec (-1 = forever).
 * Returns FALSE on timeout. */
static int Fill(long TimeoutUsec)
{
	fd_set			rfd;
	struct timeval	tv, *tp = NULL;
	ssize_t			n;

	if (InEOF)
		return TRUE;

	FD_ZERO(&rfd);
	FD_SET(0, &rfd);
	if (TimeoutUsec >= 0)
	{
		tv.tv_sec = TimeoutUsec / 1000000;
		tv.tv_usec = TimeoutUsec % 1000000;
		tp = &tv;
	}
	if (select(1, &rfd, NULL, NULL, tp) <= 0)
		return FALSE;

	if (InLen == sizeof(InBuf))		/* overlong line: discard it */
		InLen = 0;
	n = read(0, InBuf + InLen, sizeof(InBuf) - InLen);
	if (n <= 0)
		InEOF = TRUE;
	else
		InLen += n;
	return TRUE;
}

void QRea(char *Str, int *Chn)
{
	if (Net)
	{
		NetRea(Str, Chn);
		return;
	}

	*Chn = ConsolePid;

	if (!Connected)
	{
		Connected = TRUE;
		HavePrompt = FALSE;
		strcpy(Str, "0");
		return;
	}

	Str[0] = '1';
	if (HavePrompt && Prompt[1])
	{
		/* a real prompt: wait for the player */
		fputs(Prompt + 1, stdout);
		fflush(stdout);
		while (!TakeLine(Str + 1, 255))
		{
			if (InEOF)
			{
				strcpy(Str, "9");
				return;
			}
			Fill(-1);
		}
	}
	else
	{
		/* command mode: give the game a tick if the player is idle */
		fflush(stdout);
		if (!TakeLine(Str + 1, 255))
		{
			Fill(500000L);
			if (!TakeLine(Str + 1, 255))
			{
				if (InEOF)
				{
					strcpy(Str, "9");
					return;
				}
				Str[1] = '\0';
			}
		}
	}
	HavePrompt = FALSE;
}

void QOut(int Chn, char *Str)
{
	if (*Str < '0' || *Str > '5')
	{
		printf("Error in QOut(): %s\n", Str);
		return;
	}
	if (Net)
	{
		if (*Str == '2' || *Str == '3')
		{
			char	Reason[512];

			JsonString(Reason, sizeof Reason, *Str == '3' ? "End - aborted"
											: Str[1] ? Str + 1 : "Goodbye.");
			QGmcp(Chn, "Core.Goodbye", Reason);
		}
		NetOut(Chn, Str);
		return;
	}
	if (Chn != ConsolePid)
		return;		/* nobody else is connected */

	switch (*Str)
	{
		case '0':
			printf("%s\n", Str + 1);
			break;

		case '1':
			strncpy(Prompt, Str, sizeof(Prompt) - 1);
			Prompt[sizeof(Prompt) - 1] = '\0';
			HavePrompt = TRUE;
			break;

		case '2':
		case '3':
			if (Str[1])
				printf("%s\n", Str + 1);
			printf("--- disconnected; starting a new session ---\n");
			Connected = FALSE;
			HavePrompt = FALSE;
			break;
	}
}

/* Send Chn the GMCP message "Package Json"; one too long to send is dropped. */
void QGmcp(int Chn, const char *Package, const char *Json)
{
	char	Msg[8192];
	int		n = snprintf(Msg, sizeof Msg, "5%s %s", Package, Json);

	if (n > 0 && n < (int)sizeof Msg)
		QOut(Chn, Msg);
}

/* Write Str into Out as a JSON string, quotes and all, cut short if need be. */
void JsonString(char *Out, int Max, const char *Str)
{
	int		n = 0;

	if (Max < 3)
	{
		if (Max > 0)
			*Out = '\0';
		return;
	}
	Out[n++] = '"';
	for (; *Str && n < Max - 8; Str++)
	{
		unsigned char	c = (unsigned char)*Str;

		if (c == '"' || c == '\\')
		{
			Out[n++] = '\\';
			Out[n++] = (char)c;
		}
		else if (c < ' ')
			n += snprintf(Out + n, Max - n, "\\u%04x", c);
		else
			Out[n++] = (char)c;
	}
	Out[n++] = '"';
	Out[n] = '\0';
}

void GetNextMsg(void)
{
	/* The console prints at once; telnet writes what the sockets accept. */
	if (Net)
		NetFlush();
}

void Who(void)
{
	UserPoint	Player;

	if (Net)
	{
		NetWho();
		return;
	}
	QOut(Term, "0");
	if (!User->Brief)
		QOut(Term, "0Name");
	for (Player = UserTail; Player; Player = Player->NextUser)
	{
		snprintf(B1, sizeof(B1), "0%s%s", Player->Name,
				Player->NonExistant ? " (talk)" : "");
		QOut(Term, B1);
	}
}

void CheckHup(void)
{
	/* the console never hangs up; telnet logs off lost connections */
	if (Net)
		NetCheckHup();
}

/* The address a player connected from, for limiting login attempts */
const char *QHost(int Chn)
{
	return Net ? NetHost(Chn) : "console";
}

int IsGuest(int term)
{
	UNUSED(term);
	return FALSE;
}
