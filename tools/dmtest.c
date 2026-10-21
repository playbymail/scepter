/*
 * dmtest -- regression tests for security bugs that only a DM or bad data
 * can reach, for saving rooms, and for play that needs two characters or
 * a fixed outcome, run against the real engine code.
 *
 *	dmtest GAMEDIR TEST
 *	dmtest GAMEDIR legacy NAME PASSWORD
 *	dmtest - layout
 *
 * TEST is an issue number: 20, 23, 22, 24, 26, 82 or 88; for the command
 * parser 45, 47 or 48; 17 for saving EFILE; for play 27, 28, 29, 30, 32,
 * 33 or 38, which may use a second character on terminal 2; or for saving
 * rooms 42,
 * 43, 44, 56 or 64, which save room objects and monsters and
 * read them back as a segment swap would; they use rooms 900-940 and 1949.  The game directory (made
 * with mkgame) is opened as scepterd opens it; test 20 writes a description
 * line into it, so use a scratch copy.  There is no DM login yet (#70), so
 * each test sets up the state the DM editor or a corrupt record would leave
 * and calls the engine directly.  The game's output for the console is
 * captured and checked.  Prints "ok" or "FAIL" lines; exits 1 on failure.
 * Run it under a sanitizer, as tools/securitytest.py does, to catch the
 * memory errors themselves.
 *
 * Two helpers serve the other tests.  "legacy" saves a 2nd-level character
 * NAME whose password is in the old form, as upgradegame leaves a version 1
 * character.  "layout" prints where the password fields are in a player
 * record, so tools/datatest.py can make version 1 files.
 *
 * main.c is included so the server's own start-up code can be reused.
 */
#include <stdarg.h>
#include <stddef.h>
#include <sys/stat.h>

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

/* The contents of a small file, or "" */
static const char *ReadFile(const char *Path)
{
	static char	Text[8192];
	FILE		*fp;
	size_t		n = 0;

	if ((fp = fopen(Path, "r")))
	{
		n = fread(Text, 1, sizeof(Text) - 1, fp);
		fclose(fp);
	}
	Text[n] = '\0';
	return Text;
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

/* #82: password hashes, password lines, login limits, the DM's PW field */
static void Test82(void)
{
	char	H1[PWHashLen], H2[PWHashLen], PW[MaxPWLen + 1];
	int		I;

	Check(SetPW(H1, "open sesame") && !strncmp(H1, "$2b$04$", 7) && strlen(H1) == 60,
		  "SetPW makes a cost 4 bcrypt hash");
	Check(SetPW(H2, "open sesame") && strcmp(H1, H2), "each hash has its own salt");
	Check(CheckPW(H1, "open sesame") && CheckPW(H2, "open sesame"),
		  "CheckPW takes the password");
	Check(!CheckPW(H1, "Open sesame") && !CheckPW(H1, "open sesam")
		  && !CheckPW(H1, ""), "CheckPW refuses others, case and all");
	Check(!CheckPW("", "") && !CheckPW("", "open sesame"), "no hash, no password");

	snprintf(Buf, sizeof(Buf), "  open sesame  ");
	LenBuf = strlen(Buf);
	Loc = 0;
	Check(GetPW(PW) == 11 && !strcmp(PW, "open sesame"),
		  "a password line loses its outer spaces only");
	memset(Buf, 'x', MaxPWLen + 1);
	LenBuf = MaxPWLen + 1;
	Loc = 0;
	Check(GetPW(PW) == -1 && !*PW, "a password over 72 characters is refused");
	LenBuf = MaxPWLen;
	Loc = 0;
	Check(GetPW(PW) == MaxPWLen, "a 72-character password is taken");

	/* five wrong passwords from an address refuse it, doubling to 2 hours */
	for (I = 1; I < 5; I++)
		LoginFailed("10.0.0.1", "Dungeon");
	Check(!LoginWait("10.0.0.1", "Dungeon"), "four wrong passwords: no wait");
	LoginFailed("10.0.0.1", "Dungeon");
	Check(LoginWait("10.0.0.1", "Dungeon") == 600, "the fifth: refused 10 minutes");
	Check(!LoginWait("10.0.0.2", "Dungeon"), "other addresses are not refused");
	{
		static const int	Want[] = { 1200, 2400, 4800, 7200, 7200 };

		for (I = 0; I < 5; I++)
		{
			int		J;

			RealTime += LoginWait("10.0.0.1", "Dungeon");
			for (J = 0; J < 5; J++)
				LoginFailed("10.0.0.1", "Dungeon");
			if (LoginWait("10.0.0.1", "Dungeon") != Want[I])
				break;
		}
		Check(I == 5, "each refusal doubles, up to 2 hours");
	}
	Check(strstr(ReadFile("dat/errlog"), "Wrong password for Dungeon from 10.0.0.1;"
				 " address refused for 10 minutes.") != NULL,
		  "wrong passwords and refusals are logged");
	RealTime += 7200;
	LoginOK("10.0.0.1", "Dungeon");

	/* ten wrong passwords for a name, from anywhere: one try per 30 seconds */
	for (I = 0; I < 9; I++)
	{
		char	Host[32];

		snprintf(Host, sizeof(Host), "10.0.1.%d", I);
		LoginFailed(Host, "Target");
	}
	Check(!LoginWait("10.0.2.1", "Target"), "nine wrong passwords: no wait");
	LoginFailed("10.0.1.9", "Target");
	Check(LoginWait("10.0.2.1", "Target") == 30, "the tenth: 30 seconds, from anywhere");
	Check(!LoginWait("10.0.2.1", "Dungeon"), "other names do not wait");
	RealTime += 30;
	Check(!LoginWait("10.0.2.1", "Target"), "then one more try");
	LoginFailed("10.0.2.1", "Target");
	Check(LoginWait("10.0.2.2", "Target") == 30, "a wrong one waits again");
	LoginOK("10.0.2.2", "Target");
	Check(!LoginWait("10.0.2.2", "Target"), "the right one clears the wait");

	/* MODIFY PLAYER ... PW sets a hash, for master DMs only */
	User->SSJ = TRUE;
	User->Entry = XEdit;
	strcpy(H1, User->PWHash);
	Say("modify player Dungeon pw=Swordfish");
	Check(!strcmp(H1, User->PWHash), "an associate DM cannot set a password");
	User->Master = TRUE;
	Say("modify player Dungeon pw=Swordfish");
	Check(CheckPW(User->PWHash, "Swordfish") && !CheckPW(User->PWHash, "SWORDFISH")
		  && !User->PW, "a master DM sets a password, case and all");
}

/* Give the character an object, as a corrupt record would leave it */
static void Carry(ObjectType *Obj)
{
	ObjectPoint	Copy;

	if (!(Copy = (ObjectPoint)malloc(sizeof(ObjectType))))
		Abort(" dmtest: out of memory.");
	*Copy = *Obj;
	Copy->Next = User->ObjectTail;
	User->ObjectTail = Copy;
	User->LastAtk = 0;
}

/* #88: reading or using an object whose spell number is out of range */
static void Test88(void)
{
	ObjectType	Obj;

	Obj = ProtoObject;
	strcpy(Obj.Name, "scroll,odd *");
	Obj.ObClass = Scroll;
	Obj.Object.Scroll.Spell = 30000;
	Carry(&Obj);
	Capture();
	Say("read scroll");
	Check(strstr(Captured(), "Nothing happens") != NULL,
		  "a scroll with spell 30000 does nothing");

	Obj = ProtoObject;
	strcpy(Obj.Name, "wand,odd *");
	Obj.ObClass = MagDevice;
	Obj.Object.MagDevice.MSpell = 200;
	Obj.Object.MagDevice.MLevel = 1;
	Obj.Object.MagDevice.NumCharges = 5;
	Carry(&Obj);
	Capture();
	Say("use wand");
	Check(strstr(Captured(), "Nothing happens") != NULL,
		  "a device with spell 200 does nothing");

	Obj = ProtoObject;
	strcpy(Obj.Name, "rod,odd *");
	Obj.ObClass = MagDevice;
	Obj.Object.MagDevice.MSpell = -300;
	Obj.Object.MagDevice.MLevel = 0;
	Obj.Object.MagDevice.NumCharges = 5;
	Carry(&Obj);
	Capture();
	Say("use rod");
	Check(strstr(Captured(), "Nothing happens") != NULL,
		  "a device with spell -300 does nothing");
}

/* A new object called Name; a chest is open and holds Inside */
static ObjectPoint NewObj(const char *Name, ObClassType Class, ObjectPoint Inside)
{
	ObjectPoint	Obj;

	if (!(Obj = (ObjectPoint)malloc(sizeof(ObjectType))))
		Abort(" dmtest: out of memory.");
	*Obj = ProtoObject;
	snprintf(Obj->Name, sizeof(Obj->Name), "%s", Name);
	Obj->ObClass = Class;
	Obj->Next = NULL;
	if (Class == Chest)
	{
		Obj->Object.Chest.ObjectTail = Inside;
		Obj->Object.Chest.NumInside = Inside != NULL;
	}
	return Obj;
}

static ObjectPoint Box(const char *Name, const char *Inside)
{
	return NewObj(Name, Chest, Inside ? NewObj(Inside, Misc, NULL) : NULL);
}

/* Link the objects into a list, first to last */
static ObjectPoint List(ObjectPoint First, ...)
{
	va_list		Args;
	ObjectPoint	Obj = First;

	va_start(Args, First);
	while (Obj)
		Obj = Obj->Next = va_arg(Args, ObjectPoint);
	va_end(Args);
	return First;
}

/* A list as text: "rock a[sword] b[]" */
static const char *Show(ObjectPoint Obj)
{
	static char	Text[1024];
	static int	Depth;

	if (!Depth)
		*Text = '\0';
	for (; Obj; Obj = Obj->Next)
	{
		strcat(Text, Obj->Name);
		if (Obj->ObClass == Chest)
		{
			strcat(Text, "[");
			Depth++;
			Show(Obj->Object.Chest.ObjectTail);
			Depth--;
			strcat(Text, "]");
		}
		if (Obj->Next)
			strcat(Text, " ");
	}
	return Text;
}

/* Save Obj as room Rm's objects, as WriteSeg() does, and read them back */
static const char *Reload(RmCodeType Rm, ObjectPoint Obj)
{
	ObjectPoint	Back;

	WriteUObj(Rm, Obj, 0);
	ReadUObj(Rm, &Back, 0);
	return Show(Back);
}

/* Room Rm's saved objects */
static const char *Load(RmCodeType Rm)
{
	ObjectPoint	Back;

	ReadUObj(Rm, &Back, 0);
	return Show(Back);
}

static void Same(const char *Got, const char *Want, const char *What)
{
	char	Msg[1200];

	snprintf(Msg, sizeof(Msg), "%s: %s", What, Got);
	Check(!strcmp(Got, Want), Msg);
}

/* #42: an emptied or removed chest's old contents must not come back */
static void Test42(void)
{
	Same(Reload(900, Box("a", "sword")), "a[sword]", "a chest is saved");
	Same(Reload(900, Box("a", NULL)), "a[]", "the chest, emptied");

	Reload(901, List(Box("a", "sword"), Box("b", "shield"), NULL));
	Same(Reload(901, Box("b", "shield")), "b[shield]", "the first chest taken");
	Same(Reload(901, List(Box("b", "shield"), Box("c", NULL), NULL)),
		 "b[shield] c[]", "an empty chest put down");
}

/* #43: each chest in a room keeps its own contents */
static void Test43(void)
{
	Same(Reload(910, List(NewObj("rock", Misc, NULL), Box("a", "sword"),
						  Box("b", "shield"), NULL)),
		 "rock a[sword] b[shield]", "two chests after a rock");
	Same(Reload(911, List(Box("a", "sword"), Box("b", "shield"), NULL)),
		 "a[sword] b[shield]", "two chests, one listed first");
}

/* #44: eight chests are all saved; a chest can't go into a chest */
static void Test44(void)
{
	ObjectPoint	Crate;

	Same(Reload(920, List(Box("a", "1"), Box("b", "2"), Box("c", "3"),
						  Box("d", "4"), Box("e", "5"), Box("f", "6"),
						  Box("g", "7"), Box("h", "8"), NULL)),
		 "a[1] b[2] c[3] d[4] e[5] f[6] g[7] h[8]", "eight chests");
	Same(Reload(921, NewObj("a", Chest, Box("inner", "sword"))),
		 "a[inner[]]", "a chest inside a chest, from old data");

	Crate = Box("crate,oak *", NULL);
	Crate->Next = Room[User->RmCode].RmObjectTail;
	Room[User->RmCode].RmObjectTail = Crate;
	Carry(Box("box,small *", NULL));
	Capture();
	Say("put box crate");
	Check(strstr(Captured(), "A chest won't fit inside another chest!") != NULL,
		  "PUT refuses a chest into a chest");
	Check(!Crate->Object.Chest.ObjectTail && User->ObjectTail
		  && !strcmp(User->ObjectTail->Name, "box,small *"),
		  "the box is still carried");
}

/* #56: an empty chest before a full one doesn't take its contents */
static void Test56(void)
{
	Same(Reload(930, List(NewObj("sign", Misc, NULL), Box("a", NULL),
						  Box("b", "sword"), NULL)),
		 "sign a[] b[sword]", "an empty chest, then a full one");
}

/* How many records in table fp (RecSize-byte records) belong to (Room, Type) */
static int Copies(FILE *fp, size_t RecSize, int Room, int Type)
{
	ObjRec	Head;		/* only the leading Room and DataType are read */
	int		Slot, N = 0;

	for (Slot = 0; Slot < 1009; Slot++)
	{
		SeekRec(fp, (long)Slot * RecSize);
		if (fread((char *)&Head, 4, 1, fp) == 1
		&&	Head.Room == Room && Head.DataType == Type)
			N++;
	}
	return N;
}

/* #64: deleting a record must not hide records that collided past it */
static void Test64(void)
{
	MonsterPoint	Mon;
	MonsterType		Proto = ProtoMonster;

	/* 940 and 1949 = 940 + 1009 hash to the same slot */
	Reload(940, NewObj("rock", Misc, NULL));
	Reload(1949, NewObj("gem", Misc, NULL));
	DeleteUObj(940);
	Same(Load(1949), "gem", "room 1949's objects, after room 940's are deleted");
	Reload(1949, NewObj("gem", Misc, NULL));
	Check(Copies(EDBObj, sizeof(ObjRec), 1949, 0) == 1,
		  "rewriting room 1949 doesn't add a second record");
	Same(Reload(940, NewObj("rock", Misc, NULL)), "rock",
		 "room 940's objects saved again");
	Check(Copies(EDBObj, sizeof(ObjRec), 940, 0) == 1
		  && Copies(EDBObj, sizeof(ObjRec), 1949, 0) == 1,
		  "one record each");

	strcpy(Proto.Name, "rat");
	WriteUMon(940, &Proto);
	strcpy(Proto.Name, "bat");
	WriteUMon(1949, &Proto);
	DeleteMon(940);
	ReadUMon(1949, &Mon);
	Check(Mon && !strcmp(Mon->Name, "bat"),
		  "room 1949's monsters, after room 940's are deleted");
	WriteUMon(1949, &Proto);
	Check(Copies(EDBMon, sizeof(MonRec), 1949, 0) == 1,
		  "rewriting room 1949's monsters doesn't add a second record");
	ReadUMon(940, &Mon);
	Check(!Mon, "room 940 has no monsters");
}

/* Read Line as a player command; return the Num of its first word */
static int ParseNum(const char *Line, char *Word)
{
	int		Num;

	snprintf(C1, sizeof(C1), "1%s", Line);
	DoInput();
	GetWord(Word, &Num);
	return Num;
}

/* #47: GetWord must not read past the line or overflow a number */
static void Test47(void)
{
	Alfa	Word;

	Check(ParseNum("#77", Word) == 77 && !*Word, "#77 is number 77");
	Check(ParseNum("#", Word) == 0 && !*Word,
		  "a trailing # doesn't read the 7 left from the line before");
	Check(ParseNum("sword#2", Word) == 2 && !strcmp(Word, "sword"),
		  "sword#2 is the second sword");
	Check(ParseNum("sword#99999999999", Word) == 999999999,
		  "a long #n saturates at 999999999");
	Check(ParseNum("123", Word) == 123 && !*Word, "123 is a number");
	Check(ParseNum("-45", Word) == -45, "-45 is a negative number");
	Check(ParseNum("99999999999", Word) == 999999999,
		  "a long number saturates at 999999999");
	Check(ParseNum("-0000000000000012", Word) == -12,
		  "leading zeros don't saturate");
}

/* The command table entry Abbrev matches: 0 none, -1 not unique */
static int Match(const char *Abbrev)
{
	Alfa	Word;

	snprintf(Word, sizeof(Word), "%s", Abbrev);
	return BinaryMatch(Word, CmdListLen, CmdList);
}

/* #45: abbreviations of the last command, YELL, match it */
static void Test45(void)
{
	Check(!strcmp(CmdList[CmdListLen], "YELL"), "YELL is the last command");
	Check(Match("Y") == CmdListLen && Match("YE") == CmdListLen
		  && Match("YEL") == CmdListLen && Match("YELL") == CmdListLen,
		  "Y, YE, YEL and YELL are YELL");
	Check(Match("YELLS") == 0 && Match("Z") == 0, "YELLS and Z are no command");
	Check(Match("WI") > 0 && !strcmp(CmdList[Match("WI")], "WIELD"), "WI is WIELD");
	Check(Match("T") == -1, "T is not unique");
	Check(Match("*A") > 0 && !strcmp(CmdList[Match("*A")], "*ABORT"),
		  "*A, at the start of the table, is *ABORT");
}

/* #48: an empty message is an empty line */
static void Test48(void)
{
	*C1 = '\0';
	DoInput();
	Check(LenBuf == 0, "an empty message gives LenBuf 0");
}

/* #17: the hourly update must survive a read-only data directory */
static void Test17(void)
{
	char	Old[8192];

	strcpy(Old, ReadFile(EFILE));
	strcpy(Notice, "A new notice");
	if (chmod(WORKDIR "/dat", 0555))
		Abort(" dmtest: cannot chmod dat.");
	UpdateF();
	chmod(WORKDIR "/dat", 0755);
	Check(*Old && !strcmp(ReadFile(EFILE), Old),
		  "a read-only dat leaves EFILE as it was");

	UpdateF();
	Check(!strncmp(ReadFile(EFILE), "A new notice\n", 13),
		  "a writable dat gets the new EFILE");
	Check(access(WORKDIR "/dat/efile.tmp", F_OK), "no efile.tmp is left behind");
}

/* Act as Plyr: commands come from Plyr's terminal */
static void As(UserPoint Plyr)
{
	User = Plyr;
	Pid = Term = Plyr->Trm;
}

/*
 * Log in another fighter Name on terminal Trm.  Only terminal 1's output
 * is captured, so a character whose messages are checked must be the DM.
 */
static UserPoint Another(const char *Name, int Trm)
{
	UserPoint	Me = User, Plyr;

	Pid = Term = Trm;
	User = Plyr = Login();
	Say(Name);
	Say("fighter");
	Say("15, 7, 9, 10, 9");
	Say("sharp");
	Say("secretpw");
	Say("male");
	As(Me);
	if (Plyr->Entry != XCmd)
		Abort(" dmtest: could not create a second character.");
	return Plyr;
}

static void MoveTo(UserPoint Plyr, int Rm)
{
	DeletePlayer(Plyr, Plyr->RmCode);
	PlacePlayer(Plyr, Rm);
}

/* An object Name of Class that Plyr carries */
static ObjectPoint Carried(UserPoint Plyr, const char *Name, ObClassType Class)
{
	ObjectPoint	Obj = NewObj(Name, Class, NULL);

	Obj->Carry = TRUE;
	Obj->Next = Plyr->ObjectTail;
	Plyr->ObjectTail = Obj;
	return Obj;
}

/* The first object called Name that Plyr carries (FindObject changes Name) */
static ObjectPoint Holds(UserPoint Plyr, const char *Name)
{
	Alfa	Word;

	snprintf(Word, sizeof(Word), "%s", Name);
	return FindObject(Word, 0, Plyr->ObjectTail);
}

/* #27: OFFER goes ahead only when the other player has what is asked for */
static void Test27(void)
{
	UserPoint	Dm = User, Bob = Another("Bob", 2);
	ObjectPoint	Sword = Carried(User, "sword", Weap);
	ObjectPoint	Shld = Carried(Bob, "shield", Shield);

	MoveTo(Bob, 1);						/* the DM's room */
	MoveTo(User, 1);
	Capture();
	Say("offer bob sword for helmet");
	Check(strstr(Captured(), "Player doesn't have that!") != NULL,
		  "an offer for something Bob doesn't have is refused");
	Say("offer bob sword for shield");
	Check(strstr(Captured(), "Offer sent to Bob.") != NULL,
		  "an offer for Bob's shield is sent");
	As(Bob);
	Say("accept");
	As(Dm);
	Check(Holds(Bob, "sword") == Sword && Holds(User, "shield") == Shld,
		  "Bob accepts: they swap the sword and the shield");
}

/* #28: FEINT and CIRCLE delay the victim by 10 and 8 seconds */
static void Test28(void)
{
	UserPoint	Victim = Another("Victim", 2);

	MoveTo(User, 8);					/* not a safe haven */
	MoveTo(Victim, 8);
	User->Dex = 50;						/* the feint always works */
	Say("feint victim");
	Check(Victim->LastAtk == RealTime + 10, "FEINT delays the victim 10 seconds");
	User->LastAtk = 0;
	Say("circle victim");
	Check(Victim->LastAtk == RealTime + 8, "CIRCLE delays the victim 8 seconds");
}

/* #29: stealing cash isn't noticed just because a ring slot is empty */
static void Test29(void)
{
	UserPoint	Victim = User, Robber = Another("Robber", 2);
	int			I;

	MoveTo(Victim, 8);
	MoveTo(Robber, 8);
	Victim->Money = 1000;
	Robber->Class = Thief;
	Robber->Lvl = 20;					/* StealChance well over 100 */
	Robber->Dex = 50;
	Capture();
	As(Robber);
	for (I = 0; I < 20; I++)
	{
		Robber->LastAtk = 0;
		Say("steal 1 dungeon");
	}
	As(Victim);
	Check(Victim->Money == 980, "the thief steals 20 shillings");
	Check(!strstr(Captured(), "steals"), "the victim notices none of it");
}

/* #30, #31: PUT into a chest on the ground puts the weight down; GET and
 * PUT find chest#2 */
static void Test30(void)
{
	ObjectPoint	Gem = Carried(User, "gem", Misc), Chest2 = Box("chest", NULL);
	int			Weight = User->Weight;
	RmCodeType	Rm = User->RmCode;

	Gem->Weight = 10;
	User->Weight += 10;
	Chest2->Next = Room[Rm].RmObjectTail;
	Room[Rm].RmObjectTail = Box("chest", NULL);
	Room[Rm].RmObjectTail->Next = Chest2;

	Say("put gem chest");
	Check(Room[Rm].RmObjectTail->Object.Chest.ObjectTail == Gem,
		  "PUT gem chest puts it in the first chest");
	Check(User->Weight == Weight, "the gem's weight is put down");
	Say("get gem chest");
	Check(Holds(User, "gem") == Gem && User->Weight == Weight + 10,
		  "GET gem chest takes it back, weight and all");

	Say("put gem chest#2");
	Check(Chest2->Object.Chest.ObjectTail == Gem,
		  "PUT gem chest#2 puts it in the second chest");
	Say("get gem chest#2");
	Check(Holds(User, "gem") == Gem, "GET gem chest#2 takes it back");
	Check(User->Weight == Weight + 10, "the gem's weight is carried once");
}

/* #32: the victim, not the attacker, hears that their armor falls apart */
static void Test32(void)
{
	UserPoint	Victim = User, Attacker = Another("Attacker", 2);
	ObjectPoint	Arm = Carried(Victim, "armor", Armor);

	MoveTo(Victim, 8);
	MoveTo(Attacker, 8);
	Arm->Object.Armor.ArmHits = 1;
	Victim->USArm = Arm;
	Capture();
	As(Attacker);
	Attacker->DefPlayer = Victim;
	HitPlayer(1);
	As(Victim);
	Check(!Victim->USArm, "the armor falls apart");
	Check(strstr(Captured(), "Your armor falls apart!") != NULL,
		  "the victim is told");
}

/* #33: REPAIR and PAWN offers for the armor don't unwield the weapon */
static void Test33(void)
{
	ObjectPoint	Sword = Carried(User, "sword", Weap);

	Carried(User, "armor", Armor)->Price = 30;
	Carried(User, "shield", Shield)->Price = 10;
	User->USWeap = Sword;
	User->Money = 1000;
	MoveTo(User, 23);					/* the smithy */
	Say("repair armor");
	Check(User->Entry == XRepair && User->USWeap == Sword,
		  "the smith's offer leaves the sword wielded");
	Say("no");
	Check(User->USWeap == Sword, "declining leaves the sword wielded");
	Say("repair armor");
	Say("yes");
	Check(User->USWeap == Sword, "repairing the armor leaves the sword wielded");

	MoveTo(User, 22);					/* the pawnshop */
	Say("pawn shield");
	Check(User->Entry == XSell && User->USWeap == Sword,
		  "the pawnbroker's offer leaves the sword wielded");
	Say("no");
	Check(User->USWeap == Sword, "declining leaves the sword wielded");
	Say("pawn armor");
	Say("yes");
	Check(User->USWeap == Sword && !Holds(User, "armor"),
		  "pawning the armor leaves the sword wielded");
	Say("pawn sword");
	Say("yes");
	Check(!User->USWeap && !Holds(User, "sword"),
		  "the pawned sword is gone and no longer wielded");
}

/* #38: summoned monsters start at 90-100% of their hits, not always 100% */
static void Test38(void)
{
	MonsterPoint	Mon;
	int				I, Hurt = 0, Bad = 0;

	MoveTo(User, 8);					/* encounter table 1 */
	for (I = 0; I < 60; I++)
		SpellMon(SPELL_SUMMON, 10, 10, User, NULL, NULL, "");
	for (Mon = Room[User->RmCode].RmMonsterTail; Mon; Mon = Mon->Next)
		if (Mon->Hits < Mon->MaxHits)
			Hurt++;
		else
		if (Mon->Hits > Mon->MaxHits || Mon->Hits < Mon->MaxHits * 9 / 10)
			Bad++;
	Check(Hurt > 0, "some summoned monsters start below full hits");
	Check(!Bad, "none start outside 90-100%");
}

/* Save a 2nd-level character Name with only the old hash of PassWord */
static void Legacy(char *Name, char *PassWord)
{
	strcpy(User->Name, Name);
	User->Lvl = 2;
	User->PW = Hash(PassWord);
#ifdef PWHashLen
	memset(User->PWHash, 0, sizeof(User->PWHash));
#endif
	WritePlayer(User);
	Check(PlayerOnFile(Name), "an old-style character is saved");
}

int main(int argc, char **argv)
{
	static const struct { const char *Name; void (*Fn)(void); } Tests[] =
	{
		{"20", Test20}, {"23", Test23}, {"22", Test22}, {"24", Test24},
		{"26", Test26}, {"82", Test82}, {"88", Test88}, {"42", Test42},
		{"43", Test43}, {"44", Test44}, {"56", Test56}, {"64", Test64},
		{"47", Test47}, {"48", Test48}, {"17", Test17}, {"28", Test28},
		{"29", Test29}, {"30", Test30}, {"32", Test32}, {"33", Test33},
		{"38", Test38}, {"27", Test27}, {"45", Test45},
	};
	int		I;

#ifdef PWHashLen
	if (argc == 3 && !strcmp(argv[2], "layout"))
	{
		printf("pw %zu pwhash %zu %d usersave %zu record %zu\n",
			   offsetof(UserSave, PW), offsetof(UserSave, PWHash), PWHashLen,
			   sizeof(UserSave), sizeof(UserFRec));
		return 0;
	}
#endif
	if (!(argc == 3 || argc == 5 && !strcmp(argv[2], "legacy")) || chdir(argv[1]))
	{
		fprintf(stderr, "usage: dmtest GAMEDIR TEST | GAMEDIR legacy NAME PASSWORD"
				" | - layout\n");
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
	Say("secretpw");
	Say("male");
	if (!User || User->Entry != XCmd)
	{
		fprintf(stderr, "dmtest: could not create a character\n");
		return 1;
	}

	if (!strcmp(argv[2], "legacy"))
	{
		Legacy(argv[3], argv[4]);
		return Failures ? 1 : 0;
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
