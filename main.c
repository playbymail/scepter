#include <signal.h>
#include <time.h>
#include <unistd.h>

#include "common.h"
#include "scepter.h"

static int	OpenGame(const char *Prog);
static void	Init_Scepter(void);
static void	DoEntry(void);
static void	DeployWarning(int Reset);
static void	DeployCountdown(void);

static unsigned GetDate(time_t t)
{
	struct tm *tp = localtime(&t);

	return ((tp->tm_mon + 1) << 7) | tp->tm_mday;	/* mmmm0dddddd */
}

static unsigned GetMinute(time_t t)
{
	return (unsigned)((t / 60) % 1440);
}

void Boom(int sig)
{
	switch (sig)
	{
		case SIGSEGV:
			Abort(" SIGSEGV");

		default:
			Abort(" Signal caught.");
	}
}

int main(int argc, char **argv)
{
	unsigned	minute;
	int Aborted = FALSE;
	int Console = FALSE;
	char *Addr = "127.0.0.1:4000";
	int Opt;

	while ((Opt = getopt(argc, argv, "cl:")) != -1)
		switch (Opt)
		{
			case 'c':	Console = TRUE;	break;
			case 'l':	Addr = optarg;	break;
			default:
				fprintf(stderr, "usage: %s [-c | -l [HOST:]PORT] [GAMEDIR]\n", argv[0]);
				exit(2);
		}

	/* The data paths are relative to the game directory (see mkgame). */
	if (argc - optind > 1)
	{
		fprintf(stderr, "usage: %s [-c | -l [HOST:]PORT] [GAMEDIR]\n", argv[0]);
		exit(2);
	}
	if (argc - optind == 1 && chdir(argv[optind]))
	{
		perror(argv[optind]);
		exit(1);
	}

	if (!OpenGame(argv[0]))
		exit(1);
	DBReport = DBLog;

	/* The QNX server forked into the background here.  It now runs in the
	 * foreground, serving telnet connections (telnet.c), or with -c the
	 * console harness in ipc.c as its only player. */
	if (!Console)
		QListen(Addr);

	QCre();
	Init_Scepter();
	LoadSpecial();
	InitCmds();

	CurrentRec = 0;

	InitSpells();
	FolCount = 0;

	Init();



	InitEvents();

	RealTime = time(NULL);
	minute = GetMinute(RealTime);
	Today = GetDate(RealTime);

	signal(SIGSEGV, Boom);

	do
	{
		QRea(C1, &Pid);
		RealTime = time(NULL);

		if (minute != GetMinute(RealTime))
		{
			minute = GetMinute(RealTime);
			Today = GetDate(RealTime);	/* gotta do it someplace */
			CheckHup();
		}

		MonAttack();
		DeployCountdown();

		switch (C1[0])
		{
			case '0':
				QAss(C1 + 1, &Term);
				Term = Pid;
				if (User = Login())
					User->LastInput = RealTime;
				Loc	= LenBuf + 1;
				break;

			case '1':
				User = FindPid(Pid);
				if (User)
				{
					User->LastInput = RealTime;
					Term = User->Trm;
					DoInput();
					DoEntry();
				}
				break;

			case '7':	/* a deploy is coming: SIGUSR1 (restart), SIGUSR2 (reset) */
				User = NULL;
				DeployWarning(C1[1] == 'X');
				break;

			case '9':	/* end of console input, or SIGINT/SIGTERM */
				User = NULL;
				Aborted = TRUE;
				break;

			default:
				break;
		}

		if (User)
		{
			PromptUser();
		}	

		GetNextMsg();

	} while (!Aborted);

	UpdateF();
	Off();

	CurrentRec = 0;
	DBClose();
	exit(0);
}

/*
 * Deploy warnings (#136, docs/deploy.md).  A deploy sends SIGUSR1 before
 * a restart or SIGUSR2 before a reset, and stops the server two minutes
 * later.  Every player is told what will happen and what to do about it,
 * then warned again WarnFinal seconds later.  Saving keeps a character
 * over a restart, but nothing survives a reset, so its warnings never
 * suggest saving.
 */
#define	WarnFinal	90

static int		WarnReset = -1;		/* -1 when no warning is pending */
static TimeType	WarnTime;

static void WarnAll(char *Msg)
{
	UserPoint	Usr;

	for (Usr = UserTail; Usr; Usr = Usr->NextUser)
		QOut(Usr->Trm, Msg);
}

static void DeployWarning(int Reset)
{
	WarnReset = Reset;
	WarnTime = RealTime;
	WarnAll(Reset
		? "0*** The game will be RESET in 2 minutes. Every character will be"
		  " deleted, and saving won't keep it. You'll need to make a new"
		  " character when it's back. ***"
		: "0*** The server restarts in 2 minutes. Type SAVE now to keep your"
		  " progress. Characters below 2nd level can't be saved and will be"
		  " lost. Reconnect in a minute or two. ***");
}

static void DeployCountdown(void)
{
	if (WarnReset < 0 || RealTime < WarnTime + WarnFinal)
		return;
	WarnAll(WarnReset ? "0*** Reset in 30 seconds. ***"
					  : "0*** Restarting in 30 seconds. Type SAVE now. ***");
	WarnReset = -1;
}

/*
 * Open the game in the current directory, refusing one whose database this
 * build can't use (gamedb.c).  Problems go to stderr only, so a refused
 * game is left exactly as it was.  Sets NumSegs from the rooms.
 */
static int OpenGame(const char *Prog)
{
	int		Bad;

	Bad = !DBOpen(WORKDIR);
	if (!Bad && !CountSegs(&NumSegs))
		Bad++;
	if (Bad)
	{
		DBClose();
		fprintf(stderr, "%s: cannot use this game directory\n", Prog);
	}
	return !Bad;
}

/* The settings and tables, from the game database (dbstore.c) */
static void Init_Scepter(void)
{
	SettingsType	Set;
	int				ILoop, J, E[8];

	if (!GetSettings(&Set))
		Abort(" Cannot read the game's settings.");
	strcpy(Notice, Set.Notice);
	for (ILoop = 0; ILoop < 5; ILoop++)
		strcpy(NewsBuf[ILoop], Set.News[ILoop]);
	for (ILoop = 0; ILoop < 3; ILoop++)
	{
		TLvl[ILoop] = Set.ObitLvl[ILoop];
		strcpy(TName[ILoop], Set.ObitName[ILoop]);
		strcpy(TClass[ILoop], Set.ObitClass[ILoop]);
		strcpy(TBuf[ILoop], Set.Obit[ILoop]);
	}
	TaskClosed = Set.TaskClosed;
	NumRun = Set.Runs;
	MSpeed = Set.MSpeed;

	for (ILoop = 1; ILoop <= LEncounter; ILoop++)
	{
		if (!GetTableRow(FALSE, ILoop, E))
			Abort(" Cannot read the encounter table.");
		for (J = 0; J < 8; J++)
			PutEncIndex(ILoop, J, E[J]);
	}
	for (ILoop = 1; ILoop <= ObjListLen; ILoop++)
	{
		if (!GetTableRow(TRUE, ILoop, E))
			Abort(" Cannot read the treasure table.");
		for (J = 0; J < 8; J++)
			PutObjIndex(ILoop, J, E[J]);
	}

	RealTime = time((long *) 0);
	SetRandom((int) (RealTime & 0x7fff), getpid());

	NumRooms = NumSegs * 10 - 1;		/* NumSegs is set by OpenGame() */
	UserTail = NULL;
	NUsers = 0;
	LenBuf = 0;
	Loc	= 0;
	LastUpdate = RealTime;
	TradeTime = RealTime - 60;
	NTPlyr[0] = NFPlyr[0] = '\0';

	for	(ILoop = 0; ILoop <= MaxUsers; ILoop++)
	{
		Active[ILoop] =	0;
		SlotTbl[ILoop] = -1;
	}
}

static void DoEntry(void)
{
	switch ((int) User->Entry)
	{
		case XCmd:
		case XDead:
		case XSpell:
		case XSure:
			DoCmd();
			break;

		case XEdit:
			Edit();
			Loc = LenBuf + 1;
			break;

		case XNotice:
			EnterNotice();
			break;

		case XNews:
			EnterNews();
			break;

		case XParley:
		case XRepair:
		case XSell:
			YesNo();
			break;

		case XName:
			GetName();
			break;

		case XPassword:
			MatchPW();
			break;

		case XNewPW:
			NewPW();
			break;

		case XOldPW:
			EnterOldPW();
			break;

		case XChangePW:
			EnterNewPW();
			break;

		case XAgainPW:
			EnterAgainPW();
			break;

		case XNewClass:
			ChooseClass();
			break;

		case XSex:
			EnterSex();
			break;

		case XStats:
			EnterStats();
			break;

		case XSkill:
			EnterSkill();
			break;

		case XDesc:
			EnterDesc();
			break;

		default:
			snprintf(B1, sizeof(B1), "0 Sce107 - Undefined entry type = %d", User->Entry);
			QOut(Term, B1);
			User->Entry = XCmd;
			break;
	}
}
