#include <signal.h>
#include <time.h>
#include <unistd.h>

#include "common.h"
#include "scepter.h"

static void	Init_Scepter(void);
static void	DoEntry(void);

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

	/* Refuse a game whose data files this build can't read (datafile.c). */
	if (CheckGame(WORKDIR))
	{
		fprintf(stderr, "%s: cannot use this game directory\n", argv[0]);
		exit(1);
	}

	/* The QNX server forked into the background here.  It now runs in the
	 * foreground, serving telnet connections (telnet.c), or with -c the
	 * console harness in ipc.c as its only player. */
	if (!Console)
		QListen(Addr);

	QCre();
	Init_Scepter();
	LoadSpecial();
	InitCmds();

	if ((EDesc = OpenData(DSCRPT, DDesc)) == NULL)
		Abort(" File open error on DSCRPT.");

	CurrentRec = 0;

	if ((OList = OpenData(OLIST, DOList)) == NULL)
		Abort(" File open error on DLIST.");

	if ((MList = OpenData(MLIST, DMList)) == NULL)
		Abort(" File open error on MLIST.");

	InitSpells();
	FolCount = 0;

	Init();

	if ((EDBMon = OpenData(DBMON, DMonDB)) == NULL)
		Abort(" File open error on DBMON.");

	if ((EDBObj = OpenData(DBOBJ, DObjDB)) == NULL)
		Abort(" File open error on DBOBJ.");

	OpenCave();

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

		switch (C1[0])
		{
			case '0':
				QAss(C1 + 1, &Term);
				Term = Pid;
				User = Login();
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
	fclose(EDesc);
	fclose(OList);
	fclose(MList);
	fclose(EDBMon);
	fclose(EDBObj);
	CloseCave();
	exit(0);
}

static void Init_Scepter(void)
{
	int		ILoop;

	if ((EFile = fopen(EFILE, "r")) == NULL)
		Abort(" File open error on EFILE.");

	fgets(Notice, 256, EFile);
	Notice[strlen(Notice) - 1] = '\0';

	for (ILoop = 0; ILoop < 5; ILoop++)
	{
		fgets(NewsBuf[ILoop], 256, EFile);
		NewsBuf[ILoop][strlen(NewsBuf[ILoop]) - 1] = '\0';
	}
	for (ILoop = 0; ILoop < 3; ILoop++)
	{
		fgets(B1, 256, EFile);
		TName[ILoop][0] = TClass[ILoop][0] = '\0';
		sscanf(B1, "%d%s%s", &TLvl[ILoop], TName[ILoop], TClass[ILoop]);
		fgets(TBuf[ILoop], 256, EFile);
		TBuf[ILoop][strlen(TBuf[ILoop]) - 1] = '\0';
	}

	fscanf(EFile, "%c%d%d", &TaskClosed, &NumRun, &MSpeed);

	for (ILoop = 1; ILoop <= LEncounter; ILoop++)
	{
		int j, e[8];

		fscanf(EFile, "%d%d%d%d%d%d%d%d",
				  &e[0], &e[1], &e[2], &e[3], &e[4], &e[5], &e[6], &e[7]);

		for (j = 0; j < 8; j++)
			PutEncIndex(ILoop, j, e[j]);
	}
	for (ILoop = 1; ILoop <= ObjListLen; ILoop++)
	{
		int		j, o[8];

		fscanf(EFile, "%d%d%d%d%d%d%d%d",
				  &o[0], &o[1], &o[2], &o[3], &o[4], &o[5], &o[6], &o[7]);
		for (j = 0; j < 8; j++)
			PutObjIndex(ILoop, j, o[j]);
	}

	fclose(EFile);

	RealTime = time((long *) 0);
	SetRandom((int) (RealTime & 0x7fff), getpid());

	NumSegs	= 200;
	NumRooms = NumSegs * 10 - 1;
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
			sprintf(B1,	"0 Sce107 - Undefined entry type = %d", User->Entry);
			QOut(Term, B1);
			User->Entry = XCmd;
			break;
	}
}
