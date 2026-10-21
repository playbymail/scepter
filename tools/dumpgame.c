/*
 * dumpgame -- print a game's whole saved state as text (#146).
 *
 *	dumpgame GAMEDIR
 *
 * Every piece of persistent state comes out in a fixed order, one record
 * per line, each field named as it is in scepter.h:
 *
 *	notice, news, task, run		settings (EFILE)
 *	encounter, treasure			the encounter and treasure tables
 *	start, special				special rooms (SPECIAL)
 *	desc						description lines that aren't empty
 *	mlist, olist				monster and object templates
 *	room						rooms that aren't empty, then their objects
 *								("obj 2.1" is the first thing in the second
 *								item), monsters and monsters' inventories
 *	player						saved characters, by name, then their objects
 *
 * Everything is read through the engine's own functions (ReadMList,
 * ReadOList, ReadDesc, S and so ReadSeg, ReadUObj and ReadUMon, PlayerName
 * and ReadPlayer, Init_Scepter and LoadSpecial), so the dump reads whatever
 * store those functions use.  Fields that only matter during play, and
 * pointers, are left out; a pointer that leads to saved things is shown
 * by those things.  PWHash is shown only as present, locked or empty.
 *
 * The game directory is opened as scepterd opens it, and not written to.
 * tools/dumptest.py (make check-dump) compares dumps with tools/dumps/ and
 * checks that every saved struct field is dumped.
 *
 * main.c is included so the server's own start-up code can be reused.
 */
#define main scepterd_main
#include "main.c"
#undef main

static const char	*ClassName[] =
{
	"Portal", "Treasure", "Weap", "Armor", "Shield", "Coins", "Scroll",
	"Chest", "Door", "MagDevice", "Teleport", "Keys", "Card", "Ring", "Misc",
	"ObjDummy"
};
static const char	*ArticleName[] = { "A", "An", "Some", "The", "None" };
static const char	*ChName[] =
{
	"Fighter", "Thief", "MagicUser", "DM", "Cleric", "Paladin", "Ranger",
	"Barbarian"
};
static const char	*SexName[] = { "Male", "Female" };
static const char	*RoleName[] =
{
	"none", "bar", "newsstand", "pawnshop", "store", "smith"
};

#define	EnumName(List, N)	Enum(List, sizeof(List) / sizeof(List[0]), N)

/* The name of enum value N, or its number if it has none */
static const char *Enum(const char **List, int Len, int N)
{
	static char	Num[8][16];
	static int	Next;

	if (N >= 0 && N < Len)
		return List[N];
	Next = (Next + 1) % 8;
	snprintf(Num[Next], sizeof(Num[Next]), "%d", N);
	return Num[Next];
}

/* Print " Key=" and the string s (at most Max bytes) in quotes, escaped */
static void Str(const char *Key, const char *s, size_t Max)
{
	size_t	i;

	printf(" %s=\"", Key);
	for (i = 0; i < Max && s[i]; i++)
	{
		unsigned char	c = s[i];

		if (c == '"' || c == '\\')
			printf("\\%c", c);
		else
		if (c < ' ' || c >= 0x7f)
			printf("\\%03o", c);
		else
			putchar(c);
	}
	putchar('"');
}

#define	Text(Key, s)	Str(Key, s, sizeof(s))
#define	Num(Key, v)	printf(" %s=%ld", Key, (long)(v))

static void DumpObj(ObjectPoint O)
{
	Text("Name", O->Name);
	printf(" Article=%s", EnumName(ArticleName, O->Article));
	Num("Weight", O->Weight);
	Num("Price", O->Price);
	Num("DescCode", O->DescCode);
	Num("DescRec", O->DescRec);
	Num("Carry", O->Carry);
	Num("Magic", O->Magic);
	Num("Permanent", O->Permanent);
	Num("Invisible", O->Invisible);
	Num("Cursed", O->Cursed);
	printf(" ObClass=%s", EnumName(ClassName, O->ObClass));

	/* only the fields of the object's own kind mean anything */
	switch (O->ObClass)
	{
		case Portal:
			Num("ToWhere", O->Object.Portal.ToWhere);
			break;
		case Weap:
			Num("MinHp", O->Object.Weap.MinHp);
			Num("MaxHp", O->Object.Weap.MaxHp);
			Num("Strikes", O->Object.Weap.Strikes);
			Num("WeaPlus", O->Object.Weap.WeaPlus);
			Num("WeapType", O->Object.Weap.WeapType);
			break;
		case Shield:
			Num("ShPlus", O->Object.Shield.ShPlus);
			Num("ShHits", O->Object.Shield.ShHits);
			break;
		case Armor:
			Num("ArmPlus", O->Object.Armor.ArmPlus);
			Num("ArmHits", O->Object.Armor.ArmHits);
			break;
		case Coins:
			Num("Multiplier", O->Object.Coins.Multiplier);
			break;
		case Scroll:
			Num("Spell", O->Object.Scroll.Spell);
			break;
		case Chest:
			Num("Closed", O->Object.Chest.Closed);
			Num("Trap", O->Object.Chest.Trap);
			Num("Locked", O->Object.Chest.Locked);
			Num("NumInside", O->Object.Chest.NumInside);
			break;
		case Door:
			Num("DToWhere", O->Object.Door.DToWhere);
			Num("DClosed", O->Object.Door.DClosed);
			Num("DSpring", O->Object.Door.DSpring);
			Num("DToll", O->Object.Door.DToll);
			Num("DTrap", O->Object.Door.DTrap);
			Num("DLocked", O->Object.Door.DLocked);
			break;
		case Keys:
			Num("UnLock", O->Object.Keys.UnLock);
			Num("NumUses", O->Object.Keys.NumUses);
			break;
		case MagDevice:
			Num("MSpell", O->Object.MagDevice.MSpell);
			Num("NumCharges", O->Object.MagDevice.NumCharges);
			Num("MLevel", O->Object.MagDevice.MLevel);
			break;
		case Teleport:
			Num("TToWhere", O->Object.Teleport.TToWhere);
			Num("TActiveRm", O->Object.Teleport.TActiveRm);
			break;
		case Ring:
			Num("RSpell", O->Object.Ring.RSpell);
			Num("RLevel", O->Object.Ring.RLevel);
			break;
		default:
			break;
	}
	putchar('\n');
}

/* A list of objects, each line starting with Head and the object's place */
static void DumpObjs(const char *Head, const char *Path, ObjectPoint O)
{
	int		N;

	for (N = 1; O; O = O->Next, N++)
	{
		char	Here[64];

		snprintf(Here, sizeof(Here), "%s%s%d", Path, *Path ? "." : "", N);
		printf("%s obj %s", Head, Here);
		DumpObj(O);
		if (O->ObClass == Chest)
			DumpObjs(Head, Here, O->Object.Chest.ObjectTail);
	}
}

static void DumpMon(MonsterPoint M)
{
	Text("Name", M->Name);
	printf(" Article=%s", EnumName(ArticleName, M->Article));
	Num("DescRec", M->DescRec);
	Num("DescCode", M->DescCode);
	Num("AtkSpeed", M->AtkSpeed);
	Num("AC", M->AC);
	Num("Hits", M->Hits);
	Num("MaxHits", M->MaxHits);
	Num("Lvl", M->Lvl);
	Num("Num", M->Num);
	Num("MReact", M->MReact);
	Num("MParley", M->MParley);
	Num("WhichObj", M->WhichObj);
	Num("Time", M->Time);
	Num("Filler", M->Filler);
	Num("Defend", M->Defend);
	Num("Block", M->Block);
	Num("Follow", M->Follow);
	Num("Guard", M->Guard);
	Num("AtkLastAggr", M->AtkLastAggr);
	Num("SlowReact", M->SlowReact);
	Num("FastReact", M->FastReact);
	Num("Invisible", M->Invisible);
	Num("Regenerate", M->Regenerate);
	Num("Drain", M->Drain);
	Num("Poison", M->Poison);
	Num("AntiMagic", M->AntiMagic);
	Num("Undead", M->Undead);
	Num("MoralReact", M->MoralReact);
	Num("Flee", M->Flee);
	Num("Assistance", M->Assistance);
	Num("MonSpells", M->MonSpells);
	Num("Top", M->Top);
	Num("Permanent", M->Permanent);
	Num("Magic", M->Magic);
	Num("SummonHelp", M->SummonHelp);
	Num("ImmoralReact", M->ImmoralReact);
	Num("Unique", M->Unique);
	Num("Watch", M->Watch);
	Num("Nice", M->Nice);
	putchar('\n');
}

static void DumpSettings(void)
{
	int		I, J;

	printf("notice");
	Text("Notice", Notice);
	putchar('\n');
	for (I = 0; I < 5; I++)
	{
		printf("news %d", I + 1);
		Text("NewsBuf", NewsBuf[I]);
		putchar('\n');
	}
	for (I = 0; I < 3; I++)
	{
		printf("task %d", I + 1);
		Num("TLvl", TLvl[I]);
		Text("TName", TName[I]);
		Text("TClass", TClass[I]);
		Text("TBuf", TBuf[I]);
		putchar('\n');
	}
	printf("run");
	Str("TaskClosed", &TaskClosed, 1);
	Num("NumRun", NumRun);
	Num("MSpeed", MSpeed);
	putchar('\n');

	for (I = 1; I <= LEncounter; I++)
	{
		printf("encounter %d", I);
		for (J = 0; J < 8; J++)
			printf(" %d", EncIndex(I, J));
		putchar('\n');
	}
	for (I = 1; I <= ObjListLen; I++)
	{
		printf("treasure %d", I);
		for (J = 0; J < 8; J++)
			printf(" %d", ObjIndex(I, J));
		putchar('\n');
	}
}

static void DumpSpecial(void)
{
	int		Rm, I;

	printf("start");
	Num("StartRoom", StartRoom);
	Num("ReviveRoom", ReviveRoom);
	Num("JailRoom", JailRoom);
	putchar('\n');

	for (Rm = 1; Rm <= NumRooms; Rm++)
	{
		char	*Greeting = RoomGreeting(Rm);
		int		Trains = 0;

		for (I = Fighter; I <= Barbarian; I++)
			Trains |= TrainsClass(Rm, (ChType)I) << I;
		if (!RoomRole(Rm) && !Greeting && !StockCount(Rm) && !StaggerCount(Rm)
		&&	!Trains)
			continue;

		printf("special %d Role=%s", Rm, EnumName(RoleName, RoomRole(Rm)));
		Str("Greeting", Greeting ? Greeting : "", 512);
		printf(" Stock=");
		for (I = 1; I <= StockCount(Rm); I++)
			printf("%s%d", I > 1 ? "," : "", StockItem(Rm, I));
		printf(" Stagger=");
		for (I = 1; I <= StaggerCount(Rm); I++)
			printf("%s%d", I > 1 ? "," : "", StaggerItem(Rm, I));
		printf(" Trains=");
		for (I = Fighter; I <= Barbarian; I++)
			if (Trains & 1 << I)
				printf("%s%s", Trains & ((1 << I) - 1) ? "," : "", ChName[I]);
		putchar('\n');
	}
}

/* Description records are numbered from 0 and stop at the first missing */
static void DumpDesc(void)
{
	int		Rec, Line;

	for (Rec = 0; ReadDesc(Rec); Rec++)
		for (Line = 0; Line < StrPerSeg; Line++)
			if (DescBuf[Line][0])
			{
				printf("desc %d %d", Rec, Line + 1);
				Str("Text", DescBuf[Line], sizeof(DescBuf[Line]));
				putchar('\n');
			}
	printf("descs %d\n", Rec);
}

static void DumpTemplates(void)
{
	MonsterType	M;
	ObjectType	O;
	int			I;

	for (I = 0; I <= RanMonLen; I++)
		if (ReadMList(I, &M))
		{
			printf("mlist %d", I);
			DumpMon(&M);
		}
	for (I = 0; I <= RanObjLen; I++)
		if (ReadOList(I, &O))
		{
			printf("olist %d", I);
			DumpObj(&O);
		}
}

/* A room with nothing saved in it isn't dumped */
static int EmptyRoom(RoomType *R)
{
	RoomType	Blank;

	memset(&Blank, 0, sizeof(Blank));
	return !memcmp(R->Adjoin, Blank.Adjoin, sizeof(R->Adjoin)) && !R->Out
		&& !R->DescRec && !R->DescCode && !R->WhichEncounter
		&& !R->EncounterTime && !R->LastDir && !R->NotifyDM && !R->Safe
		&& !R->AntiMagic && !R->Dark && !R->RmObjectTail && !R->RmMonsterTail;
}

static void DumpRoom(int Rm, RoomType *R)
{
	MonsterPoint	M;
	char			Head[32];
	int				I;

	printf("room %d Adjoin=", Rm);
	for (I = 0; I < 6; I++)
		printf("%s%d", I ? "," : "", R->Adjoin[I]);
	Num("Out", R->Out);
	Num("DescRec", R->DescRec);
	Num("DescCode", R->DescCode);
	Num("WhichEncounter", R->WhichEncounter);
	Num("EncounterTime", R->EncounterTime);
	Num("LastDir", R->LastDir);
	Num("NotifyDM", R->NotifyDM);
	Num("Safe", R->Safe);
	Num("AntiMagic", R->AntiMagic);
	Num("Dark", R->Dark);
	putchar('\n');

	snprintf(Head, sizeof(Head), "room %d", Rm);
	DumpObjs(Head, "", R->RmObjectTail);
	for (M = R->RmMonsterTail, I = 1; M; M = M->Next, I++)
	{
		printf("room %d mon %d", Rm, I);
		DumpMon(M);
		snprintf(Head, sizeof(Head), "room %d mon %d", Rm, I);
		DumpObjs(Head, "", M->ObjectTail);
	}
}

/*
 * Load each segment of rooms through S(), as play does, dump it and throw
 * it away.  The slot is emptied by hand, so that S() never writes a
 * segment back to make room for the next.
 */
static void DumpRooms(void)
{
	int		Seg, I, Shown = 0;

	for (Seg = 0; Seg < NumSegs; Seg++)
	{
		int		Base = S(Seg ? Seg * 10 : 1) / 10 * 10;

		for (I = 0; I < 10; I++)
		{
			RoomType		*R = &Room[Base + I];
			MonsterPoint	M, Next;
			ObjectPoint		O, ONext;

			if (!EmptyRoom(R))
			{
				DumpRoom(Seg * 10 + I, R);
				Shown++;
			}
			for (O = R->RmObjectTail; O; O = ONext)
			{
				ONext = O->Next;
				ODestroy(O);
			}
			for (M = R->RmMonsterTail; M; M = Next)
			{
				Next = M->Next;
				Destroy(M);
			}
			R->RmObjectTail = NULL;
			R->RmMonsterTail = NULL;
		}
		SlotTbl[Base / 10] = -1;
	}
	printf("rooms %d shown %d\n", NumSegs * 10, Shown);
}

static int ByName(const void *a, const void *b)
{
	return strcmp((const char *)a, (const char *)b);
}

static void DumpPlayers(void)
{
	static Alfa	Names[10000];
	UserType	P;
	int			NNames, I;

	for (NNames = 0; NNames < 10000 && PlayerName(NNames, Names[NNames]); NNames++)
		;
	qsort(Names, NNames, sizeof(Alfa), ByName);

	for (I = 0; I < NNames; I++)
	{
		ObjectPoint	O;
		int			N;

		ReadPlayer(&P, Names[I]);
		printf("player");
		Text("Name", P.Name);
		Num("Lvl", P.Lvl);
		printf(" Class=%s", EnumName(ChName, P.Class));
		Num("Hits", P.Hits);
		Num("MaxHits", P.MaxHits);
		Num("Fatigue", P.Fatigue);
		Num("MaxFatigue", P.MaxFatigue);
		Num("Magic", P.Magic);
		Num("MaxMagic", P.MaxMagic);
		Num("Experience", P.Experience);
		Num("RmCode", P.RmCode);
		Num("Str", P.Str);
		Num("Int", P.Int);
		Num("Dex", P.Dex);
		Num("Pty", P.Pty);
		Num("Con", P.Con);
		Num("LastAccess", P.LastAccess);
		Num("PW", P.PW);
		Num("Money", P.Money);
		printf(" Sex=%s", EnumName(SexName, P.Sex));
		Num("SSharp", P.SSharp);
		Num("SThrust", P.SThrust);
		Num("SBlunt", P.SBlunt);
		Num("SLong", P.SLong);
		Num("Assoc", P.Assoc);
		Num("Spells", P.Spells);
		Num("SSJ", P.SSJ);
		Num("Master", P.Master);
		Num("PlayTester", P.PlayTester);
		Num("AGuild", P.AGuild);
		Num("TGuild", P.TGuild);
		Num("Evil", P.Evil);
		Num("Invisible", P.Invisible);
		Num("NonExistant", P.NonExistant);
		Num("SkillNew", P.SkillNew);
		Num("SpellNew", P.SpellNew);
		Num("Poisoned", P.Poisoned);
		Num("Hidden", P.Hidden);
		Num("It", P.It);
		Num("Suck", P.Suck);
		Num("Brief", P.Brief);
		Num("Echo", P.Echo);
		Num("MesBlock", P.MesBlock);
		Num("Dead", P.Dead);
		Num("Plague", P.Plague);
		printf(" PWHash=%s\n", !P.PWHash[0] ? "empty"
			   : !strcmp(P.PWHash, LockedPW) ? "locked" : "present");

		for (O = P.ObjectTail, N = 1; O; O = O->Next, N++)
		{
			printf("player %s obj %d Worn=%s", P.Name, N,
				   O == P.USWeap ? "weapon" : O == P.USArm ? "armor"
				   : O == P.USShield ? "shield" : O == P.USRingL ? "ringl"
				   : O == P.USRingR ? "ringr" : "-");
			DumpObj(O);
		}
		while (P.ObjectTail)
		{
			O = P.ObjectTail;
			P.ObjectTail = O->Next;
			ODestroy(O);
		}
	}
	printf("players %d\n", NNames);
}

int main(int argc, char **argv)
{
	if (argc != 2 || chdir(argv[1]))
	{
		fprintf(stderr, "usage: dumpgame GAMEDIR\n");
		return 2;
	}

	/* the same start-up as scepterd's main(), without players */
	if (!OpenGame("dumpgame"))
		return 1;
	QCre();
	Init_Scepter();
	LoadSpecial();
	Init();
	if (!(EDBMon = OpenData(DBMON, DMonDB)) || !(EDBObj = OpenData(DBOBJ, DObjDB)))
		Abort(" dumpgame: cannot open game files.");

	DumpSettings();
	DumpSpecial();
	DumpDesc();
	DumpTemplates();
	DumpRooms();
	DumpPlayers();

	fclose(EDBMon);
	fclose(EDBObj);
	DBClose();
	return ferror(stdout) || fflush(stdout) ? 1 : 0;
}
