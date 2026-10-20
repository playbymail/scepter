/*
 * dmtest -- regression tests for security bugs that only a DM or bad data
 * can reach, run against the real engine code.
 *
 *	dmtest GAMEDIR TEST
 *
 * TEST is an issue number: 20, 23, 22, 24 or 26.  The game directory (made
 * with mkgame) is opened as scepterd opens it; test 20 writes a description
 * line into it, so use a scratch copy.  There is no DM login yet (#70), so
 * each test sets up the state the DM editor or a corrupt record would leave
 * and calls the engine directly.  The game's output for the console is
 * captured and checked.  Prints "ok" or "FAIL" lines; exits 1 on failure.
 * Run it under a sanitizer, as tools/securitytest.py does, to catch the
 * memory errors themselves.
 *
 * main.c is included so the server's own start-up code can be reused.
 */
#define main scepterd_main
#include "main.c"
#undef main

static int	Failures;
static char	Out[8192];				/* captured game output */

static void Check(int Cond, const char *What)
{
	fprintf(stderr, "%s %s\n", Cond ? "ok  " : "FAIL", What);
	if (!Cond)
		Failures++;
}

/* Send the game's console output to a file, to read back with Captured() */
static void Capture(void)
{
	fflush(stdout);
	if (!freopen("dmtest.out", "w+", stdout))
		Abort(" dmtest: cannot capture output.");
}

static const char *Captured(void)
{
	size_t	n;

	fflush(stdout);
	rewind(stdout);
	n = fread(Out, 1, sizeof(Out) - 1, stdout);
	Out[n] = '\0';
	return Out;
}

static void Say(const char *Text)
{
	snprintf(C1, sizeof(C1), "1%s", Text);
	DoInput();
	DoEntry();
}

/* Enter Text as line Line of description record Rec, as MODIFY EDESC does */
static void EnterLine(int Rec, int Line, const char *Text)
{
	User->Data = Rec * StrPerSeg + (Line - 1);
	User->Entry = XDesc;
	snprintf(Buf, sizeof(Buf), "%s", Text);
	LenBuf = strlen(Buf);
	Loc = 0;
	EnterDesc();
}

/* "1/" + N - 3 x's + "/": a one-phrase line N characters long */
static const char *Phrase(int N)
{
	static char	Text[256];

	memset(Text, 'x', N);
	memcpy(Text, "1/", 2);
	Text[N - 1] = '/';
	Text[N] = '\0';
	return Text;
}

/* #20: the description editor must not write past a DescBuf line */
static void Test20(void)
{
	Capture();
	EnterLine(1, 10, Phrase(80));		/* line 10 is the last row */
	Check(strstr(Captured(), "Bad description.") != NULL,
		  "an 80-character line is refused, as mkgame refuses it");

	EnterLine(1, 10, Phrase(79));
	SeekRec(EDesc, 1L * sizeof(DescBuf));
	Check(fread(DescBuf, sizeof(DescBuf), 1, EDesc) == 1
		  && !strcmp(DescBuf[9], Phrase(79)),
		  "a 79-character line is stored");
}

/* #23: a description row may fill all 80 bytes, with no NUL */
static void Test23(void)
{
	char	V[256];
	int		Cont;

	memset(DescBuf, 'y', sizeof(DescBuf));		/* no NUL anywhere */
	memcpy(DescBuf[0], "1/", 2);
	DescBuf[0][79] = '/';
	CurrentRec = 1;
	Pd(1, 1, 1, FALSE, &Cont, V);
	Check(strlen(V) == 77 && strspn(V, "y") == 77,
		  "an 80-byte row gives its 77-character phrase");
}

/* #22: asking for a phrase the line does not have */
static void Test22(void)
{
	char	V[256];
	int		Cont;

	memset(DescBuf, 0, sizeof(DescBuf));
	strcpy(DescBuf[0], "1/one/");
	strcpy(DescBuf[1], "1//");
	CurrentRec = 1;
	Pd(1, 1, 3, FALSE, &Cont, V);
	Check(*V == '\0', "phrase 3 of a one-phrase line is empty");
	Pd(1, 2, 3, FALSE, &Cont, V);
	Check(*V == '\0', "phrase 3 of an empty line is empty");
	Pd(1, 1, 1, FALSE, &Cont, V);
	Check(!strcmp(V, "one"), "phrase 1 still reads");
}

/* #24: a 19-character monster name that expands to 45 */
static void Test24(void)
{
	MonsterType	Mon = ProtoMonster;

	strcpy(Mon.Name, "abcdefghijklmno,***");
	Mon.Article = None;
	Mon.Num = 1;
	Mon.Top = TRUE;
	PM(&Mon, FALSE, B2);
	Check(!strcmp(B2, "abcdefghijklmnoabcdefghijklmnoabcdefghijklmno"),
		  "PM expands the name in full");
}

/* #26: object data with a bad weapon type or spell number */
static void Test26(void)
{
	ObjectType	Obj;

	Capture();
	Obj = ProtoObject;
	strcpy(Obj.Name, "sword,odd *");
	Obj.ObClass = Weap;
	Obj.Object.Weap.WeapType = 99;
	ObjDisplay(&Obj);
	Check(strstr(Captured(), "Unknown class weapon") != NULL,
		  "a bad weapon type shows as Unknown");

	Capture();
	Obj = ProtoObject;
	strcpy(Obj.Name, "scroll,odd *");
	Obj.ObClass = Scroll;
	Obj.Object.Scroll.Spell = 30000;
	ObjDisplay(&Obj);
	Check(strstr(Captured(), "spell = UNKNOWN") != NULL,
		  "a bad scroll spell shows as UNKNOWN");

	Capture();
	Obj = ProtoObject;
	strcpy(Obj.Name, "wand,odd *");
	Obj.ObClass = MagDevice;
	Obj.Object.MagDevice.MSpell = -5;
	ObjDisplay(&Obj);
	Check(strstr(Captured(), "spell = UNKNOWN") != NULL,
		  "a bad device spell shows as UNKNOWN");

	Capture();
	Obj = ProtoObject;
	strcpy(Obj.Name, "ring,odd *");
	Obj.ObClass = Ring;
	Obj.Object.Ring.RSpell = 999;
	ObjDisplay(&Obj);
	Check(strstr(Captured(), "spell = UNKNOWN") != NULL,
		  "a bad ring spell shows as UNKNOWN");
}

int main(int argc, char **argv)
{
	static const struct { const char *Name; void (*Fn)(void); } Tests[] =
	{
		{"20", Test20}, {"23", Test23}, {"22", Test22}, {"24", Test24},
		{"26", Test26},
	};
	int		I;

	if (argc != 3 || chdir(argv[1]))
	{
		fprintf(stderr, "usage: dmtest GAMEDIR TEST\n");
		return 2;
	}

	/* the same start-up as scepterd's main() */
	if (CheckGame(WORKDIR))
	{
		fprintf(stderr, "dmtest: cannot use this game directory\n");
		return 1;
	}
	QCre();
	Init_Scepter();
	LoadSpecial();
	InitCmds();
	if (!(EDesc = OpenData(DSCRPT, DDesc)) || !(OList = OpenData(OLIST, DOList))
	||	!(MList = OpenData(MLIST, DMList)))
		Abort(" dmtest: cannot open game files.");
	InitSpells();
	FolCount = 0;
	Init();
	if (!(EDBMon = OpenData(DBMON, DMonDB)) || !(EDBObj = OpenData(DBOBJ, DObjDB)))
		Abort(" dmtest: cannot open game files.");
	OpenCave();
	InitEvents();
	RealTime = time(NULL);
	Today = GetDate(RealTime);

	/* a character to act as the DM */
	freopen("/dev/null", "w", stdout);
	Pid = Term = 1;
	User = Login();
	User->LastInput = RealTime;
	Loc = LenBuf + 1;
	Say("Dungeon");
	Say("fighter");
	Say("15, 7, 9, 10, 9");
	Say("sharp");
	Say("secret");
	Say("male");
	if (!User || User->Entry != XCmd)
	{
		fprintf(stderr, "dmtest: could not create a character\n");
		return 1;
	}

	for (I = 0; I < (int)(sizeof(Tests) / sizeof(Tests[0])); I++)
		if (!strcmp(argv[2], Tests[I].Name))
		{
			Tests[I].Fn();
			return Failures ? 1 : 0;
		}
	fprintf(stderr, "dmtest: no test %s\n", argv[2]);
	return 2;
}
