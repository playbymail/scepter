/*
 * fightsim -- run fights through the real engine code with a simulated clock.
 *
 *	fightsim [options] GAMEDIR MONSTER ROOM
 *
 *	-n FIGHTS	number of fights (default 200)
 *	-c CLASS	fighter, thief, cleric, paladin, ranger or magic-user
 *				(default fighter)
 *	-s STATS	"str, int, dex, pty, con" (default "15, 7, 9, 10, 9")
 *	-k SKILL	sharp, thrust, blunt or pole (default sharp)
 *	-w OBJ		weapon object template, 0 for none (default 2, short sword)
 *	-a OBJ		armor object template, 0 for none (default 3, leather armor)
 *	-r SEED		random seed (default: time and pid, as scepterd)
 *	-m			the monster attacks first: it targets the character as soon
 *				as it appears, and the character does not act until the
 *				monster has made its first attack (hit or miss)
 *	-v			print the game text
 *
 * A 1st-level character is created through the normal login prompts,
 * equipped, and fights a fresh copy of MList template MONSTER in ROOM of
 * an existing game (made with mkgame) FIGHTS times, restored to full
 * strength and fresh gear before each fight.  The game directory's files
 * are opened as scepterd opens them; the simulation writes to them only
 * if the engine does (fightsim never saves the character).
 *
 * Each simulated second runs MonAttack() once, and the character attacks
 * whenever ReadyCheck() would allow it.  A round is one attack by the
 * character.  Fights still running after an hour count as unfinished.
 *
 * main.c is included so the server's own start-up code can be reused.
 */
#define main scepterd_main
#include "main.c"
#undef main

static void Say(const char *Text)
{
	snprintf(C1, sizeof(C1), "1%s", Text);
	DoInput();
	DoEntry();
}

static ObjectPoint Give(int Template)
{
	ObjectPoint	Obj;

	if (!Template)
		return NULL;
	if (!(Obj = malloc(sizeof(ObjectType))) || !ReadOList(Template, Obj))
	{
		fprintf(stderr, "fightsim: object template %d not found\n", Template);
		exit(1);
	}
	Obj->Next = User->ObjectTail;
	User->ObjectTail = Obj;
	User->Weight += Obj->Weight;
	return Obj;
}

/* the word a player would type for an object or monster name */
static void NameWord(const char *Name, char *Word)
{
	size_t	n = strcspn(Name, ",");

	snprintf(Word, sizeof(Alfa), "%.*s", (int)n, Name);
}

static void Usage(void)
{
	fprintf(stderr, "usage: fightsim [-n fights] [-c class] [-s stats] "
			"[-k skill] [-w obj] [-a obj] [-r seed] [-m] [-v] GAMEDIR MONSTER ROOM\n");
	exit(2);
}

int main(int argc, char **argv)
{
	const char	*Class = "fighter", *Stats = "15, 7, 9, 10, 9", *Skill = "sharp";
	int			Fights = 200, WeapT = 2, ArmT = 3, Seed = -1, Verbose = FALSE;
	int			MonFirst = FALSE;
	int			MonTmpl, Rm, Fight, Wins = 0, Unfinished = 0, Opt;
	int			MinR = 1 << 30, MaxR = 0;
	long		TotRounds = 0, TotSecs = 0, WinRounds = 0;
	UserPoint	Me;
	UserType	Snap;
	ObjectPoint	Weap, Arm;
	ObjectType	WeapNew, ArmNew;
	MonsterType	Tmpl;
	char		Cmd[64], Word[sizeof(Alfa)];

	while ((Opt = getopt(argc, argv, "n:c:s:k:w:a:r:mv")) != -1)
		switch (Opt)
		{
			case 'n':	Fights = atoi(optarg);	break;
			case 'c':	Class = optarg;			break;
			case 's':	Stats = optarg;			break;
			case 'k':	Skill = optarg;			break;
			case 'w':	WeapT = atoi(optarg);	break;
			case 'a':	ArmT = atoi(optarg);	break;
			case 'r':	Seed = atoi(optarg);	break;
			case 'm':	MonFirst = TRUE;		break;
			case 'v':	Verbose = TRUE;			break;
			default:	Usage();
		}
	if (argc - optind != 3 || Fights < 1)
		Usage();
	MonTmpl = atoi(argv[optind + 1]);
	Rm = atoi(argv[optind + 2]);
	if (chdir(argv[optind]))
	{
		perror(argv[optind]);
		return 1;
	}

	/* the same start-up as scepterd's main() */
	if (CheckGame(WORKDIR) + !DBOpen(WORKDIR))
	{
		DBClose();
		fprintf(stderr, "fightsim: cannot use this game directory\n");
		return 1;
	}
	QCre();
	Init_Scepter();
	if (Seed >= 0)
		SetRandom(Seed, Seed * 7 + 3);
	LoadSpecial();
	InitCmds();
	InitSpells();
	FolCount = 0;
	Init();
	if (!(EDBMon = OpenData(DBMON, DMonDB)) || !(EDBObj = OpenData(DBOBJ, DObjDB)))
		Abort(" fightsim: cannot open game files.");
	OpenCave();
	InitEvents();
	RealTime = time(NULL);
	Today = GetDate(RealTime);

	if (Rm < 1 || Rm > NumRooms || !ReadMList(MonTmpl, &Tmpl) || !*Tmpl.Name)
	{
		fprintf(stderr, "fightsim: bad monster template or room\n");
		return 1;
	}
	if (!Verbose && !freopen("/dev/null", "w", stdout))
	{
		perror("fightsim: /dev/null");
		return 1;
	}

	/* create the character by answering the login prompts */
	Pid = Term = 1;
	User = Login();
	User->LastInput = RealTime;
	Loc = LenBuf + 1;
	while (User && User->Entry != XCmd)
	{
		EntryType	Was = User->Entry;
		const char	*Answer;

		switch (Was)
		{
			case XName:		Answer = "Simulant";	break;
			case XNewClass:	Answer = Class;			break;
			case XStats:	Answer = Stats;			break;
			case XSkill:	Answer = Skill;			break;
			case XNewPW:	Answer = "secretpw";		break;
			case XSex:		Answer = "male";		break;
			default:
				fprintf(stderr, "fightsim: unexpected prompt %d\n", Was);
				return 1;
		}
		Say(Answer);
		if (User && User->Entry == Was)
		{
			fprintf(stderr, "fightsim: the game rejected '%s'\n", Answer);
			return 1;
		}
	}
	Me = User;

	Weap = Give(WeapT);
	Arm = Give(ArmT);
	if (Weap)
	{
		NameWord(Weap->Name, Word);
		snprintf(Cmd, sizeof(Cmd), "wield %s", Word);
		Say(Cmd);
		WeapNew = *Weap;
	}
	if (Arm)
	{
		NameWord(Arm->Name, Word);
		snprintf(Cmd, sizeof(Cmd), "wear %s", Word);
		Say(Cmd);
		ArmNew = *Arm;
	}

	StopFollow(Me);
	DeletePlayer(Me, Me->RmCode);
	PlacePlayer(Me, Rm);
	Snap = *Me;

	NameWord(Tmpl.Name, Word);
	snprintf(Cmd, sizeof(Cmd), "attack %s", Word);

	fprintf(stderr, "%s: Str %d Int %d Dex %d Pty %d Con %d, hits %d, fatigue %d,"
			" AC %d, skills %d/%d/%d/%d\n", CName[(int)Me->Class], Me->Str,
			Me->Int, Me->Dex, Me->Pty, Me->Con, Me->MaxHits, Me->MaxFatigue,
			Me->AC, Me->SSharp, Me->SThrust, Me->SBlunt, Me->SLong);
	fprintf(stderr, "monster %d: %s, level %d, hits %d, AC %d, attack speed %d;"
			" monster speed %d; room %d%s\n", MonTmpl, Tmpl.Name, Tmpl.Lvl,
			Tmpl.Hits, Tmpl.AC, Tmpl.AtkSpeed, MSpeed, Rm,
			MonFirst ? "; monster attacks first" : "");

	for (Fight = 0; Fight < Fights; Fight++)
	{
		MonsterPoint	Mon = malloc(sizeof(MonsterType));
		int				Rounds = 0, Secs = 0, Won = FALSE;

		/* restore the character and gear, keeping the list links */
		{
			ObjectPoint	Tail = Me->ObjectTail;
			UserPoint	Nx = Me->Next, NU = Me->NextUser;

			*Me = Snap;
			Me->ObjectTail = Tail;
			Me->Next = Nx;
			Me->NextUser = NU;
		}
		if (Weap)
		{
			ObjectPoint	Nx = Weap->Next;

			*Weap = WeapNew;
			Weap->Next = Nx;
		}
		if (Arm)
		{
			ObjectPoint	Nx = Arm->Next;

			*Arm = ArmNew;
			Arm->Next = Nx;
		}
		Me->USWeap = Weap;
		Me->USArm = Arm;
		Me->LastAtk = RealTime - 50;
		Me->DefMon = NULL;

		*Mon = Tmpl;
		InsertMonster(Mon, Me->RmCode);
		Me->HitAtTime = RealTime;		/* MonAttack() sets it on each attack */
		if (MonFirst)
			Mon->DefPlayer = Me;

		while (Secs < 3600)
		{
			RealTime++;
			Secs++;

			MonAttack();
			if (Me->Dead)
				break;

			User = Me;
			Term = Me->Trm;
			if (MonFirst && Me->HitAtTime < RealTime - Secs + 1)
				continue;				/* still waiting for the monster */
			if (RealTime + 1 >= Me->LastAtk)
			{
				if (Weap && !Me->USWeap && Weap->Object.Weap.Strikes > 0)
				{
					NameWord(Weap->Name, Word);
					snprintf(C1, sizeof(C1), "1wield %s", Word);
					DoInput();
					DoEntry();				/* re-wield after a fumble */
				}
				Say(Cmd);
				Rounds++;
				if (!Room[Me->RmCode].RmMonsterTail)
				{
					Won = TRUE;
					break;
				}
			}
		}

		if (Won)
		{
			Wins++;
			WinRounds += Rounds;
		}
		else if (!Me->Dead)
			Unfinished++;
		TotRounds += Rounds;
		TotSecs += Secs;
		if (Rounds < MinR)
			MinR = Rounds;
		if (Rounds > MaxR)
			MaxR = Rounds;

		/* clear the room for the next fight */
		while (Room[Me->RmCode].RmMonsterTail)
		{
			MonsterPoint	M = Room[Me->RmCode].RmMonsterTail;

			DeleteMonster(M, Me->RmCode);
			free(M);
		}
		while (Room[Me->RmCode].RmObjectTail)
		{
			ObjectPoint	O = Room[Me->RmCode].RmObjectTail;

			DeleteObject(O, &Room[Me->RmCode].RmObjectTail);
			ODestroy(O);
		}
	}

	fprintf(stderr, "%d fights: won %d (%.1f%%), lost %d, unfinished %d\n",
			Fights, Wins, 100.0 * Wins / Fights, Fights - Wins - Unfinished,
			Unfinished);
	fprintf(stderr, "rounds per fight: average %.2f (min %d, max %d);"
			" average in wins %.2f\n", (double)TotRounds / Fights, MinR, MaxR,
			Wins ? (double)WinRounds / Wins : 0.0);
	fprintf(stderr, "average fight length: %.1f seconds\n",
			(double)TotSecs / Fights);
	return 0;
}
