/*
 * mkgame -- compile a text world into the data files for a new game.
 *
 *	mkgame WORLD GAMEDIR
 *	mkgame -u [-n] [--adopt] [--discard-edits] WORLD GAMEDIR
 *
 * WORLD is a world file (see docs/world-format.md).  GAMEDIR must be an
 * existing, empty directory; mkgame writes the game database (gamedb.c)
 * and the data files scepterd reads into it, and never creates or deletes
 * a directory or a file that was there before.  Run the server with
 * "scepterd GAMEDIR".  The world file itself is never written, so one
 * world can seed any number of games.
 *
 * -u updates an existing game, with the server stopped, to a new revision
 * of the world it was made from (#137; UpdateGame): its rooms, room
 * contents, descriptions, templates, tables and special rooms are replaced,
 * and its characters kept.  It refuses another world, and a game whose
 * world data DMs have changed since it was last made or updated, unless
 * --discard-edits.  --adopt updates a game made before the game recorded
 * its world (#138).  -n only reports what would change.
 *
 * Records are written with the engine's own structures (scepter.h), so
 * mkgame must be built from the same headers as scepterd.
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <getopt.h>
#include <stdarg.h>
#include <sys/stat.h>

#include "common.h"
#include "scepter.h"
#include "sha256.h"

#define	NumSegsInit	200					/* main.c: NumSegs */
#define	MaxRoom		(NumSegsInit * 10 - 1)
#define	MaxDesc		255					/* DescRec is a uchar */
#define	MaxArea		255
#define	AreaNameLen	60
#define	MaxCoord	999

typedef struct ONode
{
	ObjectType		Obj;
	struct ONode	*Kids[SavedItems + 1];	/* chest contents, 1-based */
	int				Line;
} ONode;

typedef struct
{
	MonsterType		Mon;
	ONode			*Inv[SavedItems + 1];	/* not supported by the engine */
	int				Line;
} MNode;

typedef struct
{
	int		Defined, Line;
	RoomType	Rm;
	ONode	*Objs[SavedItems + 1];
	MNode	*Mons[SavedItems + 1];

	/* special-room settings (special.c) */
	int		Role, RoleLine;
	char	*Greeting;
	int		NStock, Stock[20], StockLine;
	int		NStagger, Stagger[20], StaggerLine;
	unsigned	Trains;

	/* map position (#141): checked, but not yet stored in the game */
	int		Area, AreaLine;
	int		HasXYZ, XYZLine, X, Y, Z;
} RoomDef;

static const char	*RoleNames[] =
	{"none", "bar", "newsstand", "pawnshop", "store", "smith", NULL};
static const char	*ClassNames[] =
	{"fighter", "thief", "magic-user", "dm", "cleric", "paladin", "ranger",
	 "barbarian", NULL};

static char		WorldName[64];
static char		WorldId[37];			/* in small letters */
static int		WorldLine = 1;			/* the [world] header, for errors */
static char		Revision[SHA256Hex];	/* SHA-256 of the world file's bytes */
static int		MonSpeed = 10;
static int		StartRm = 1, ReviveRm, JailRm, StartLine, ReviveLine, JailLine;
static char		WNotice[256];
static char		WNews[5][256];

static char		Desc[MaxDesc + 1][StrPerSeg][80];
static int		DescDefined[MaxDesc + 1];
static int		MaxDescRec;

static char		AreaName[MaxArea + 1][AreaNameLen + 1];
static int		AreaDefined[MaxArea + 1], AreaLine[MaxArea + 1];
static int		AreaNamed[MaxArea + 1];

static RoomDef	Rooms[MaxRoom + 1];
static int		MaxRoomDefined;

static ObjectType	OTmpl[RanObjLen + 1];
static int			ODefined[RanObjLen + 1], OLine[RanObjLen + 1];
static MonsterType	MTmpl[RanMonLen + 1];
static int			MDefined[RanMonLen + 1], MLine[RanMonLen + 1];

static short	Enc[LEncounter + 1][8];
static int		EncDefined[LEncounter + 1];
static short	Tre[ObjListLen + 1][8];
static int		TreDefined[ObjListLen + 1];

static const char	*FileName;
static int			LineNo;
static int			Errors, Warnings;

/* the block currently being filled */
static enum { BNone, BWorld, BDesc, BArea, BRoom, BMon, BObj, BEnc, BTre, BSkip } Block;
static int			BlockId;
static RoomType		*CurRoom;
static MonsterType	*CurMon;
static ObjectType	*CurObj;
static int			CurObjKind;		/* TRUE once kind= has been seen */

static void Err(const char *Fmt, ...)
{
	va_list	ap;

	fprintf(stderr, "%s:%d: error: ", FileName, LineNo);
	va_start(ap, Fmt);
	vfprintf(stderr, Fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	Errors++;
}

static void Warn(int Line, const char *Fmt, ...)
{
	va_list	ap;

	fprintf(stderr, "%s:%d: warning: ", FileName, Line);
	va_start(ap, Fmt);
	vfprintf(stderr, Fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	Warnings++;
}

static char *Trim(char *s)
{
	char	*e;

	while (isspace((unsigned char)*s))
		s++;
	e = s + strlen(s);
	while (e > s && isspace((unsigned char)e[-1]))
		*--e = '\0';
	return s;
}

static int ParseInt(const char *v, int Lo, int Hi, int *Out)
{
	char	*end;
	long	n;

	errno = 0;
	n = strtol(v, &end, 10);
	if (end == v || *Trim(end) || errno)
	{
		Err("'%s' is not a number", v);
		return FALSE;
	}
	if (n < Lo || n > Hi)
	{
		Err("%ld is out of range %d..%d", n, Lo, Hi);
		return FALSE;
	}
	*Out = (int)n;
	return TRUE;
}

static int ParseBool(const char *v)
{
	if (!strcmp(v, "yes") || !strcmp(v, "true") || !strcmp(v, "1"))
		return TRUE;
	if (strcmp(v, "no") && strcmp(v, "false") && strcmp(v, "0"))
		Err("'%s' is not yes or no", v);
	return FALSE;
}

/* "a b c" -> up to Max ints in Lo..Hi; returns the count */
static int ParseList(char *v, int Max, int Lo, int Hi, int *Out)
{
	int		n = 0;
	char	*tok;

	for (tok = strtok(v, " \t,"); tok; tok = strtok(NULL, " \t,"))
	{
		if (n == Max)
		{
			Err("more than %d entries", Max);
			break;
		}
		if (!ParseInt(tok, Lo, Hi, &Out[n]))
			break;
		n++;
	}
	return n;
}

/* "REC:LINE" */
static void ParseDesc(const char *v, uchar *Rec, uchar *Line)
{
	int		r, l;

	if (sscanf(v, "%d:%d", &r, &l) != 2 || r < 0 || r > MaxDesc
	||	l < 0 || l > StrPerSeg)
		Err("desc must be RECORD:LINE (record 0..%d, line 0..%d)",
			MaxDesc, StrPerSeg);
	else
	{
		*Rec = (uchar)r;
		*Line = (uchar)l;
	}
}

/* Same rules as ModName() in edit2.c. */
static void ParseName(const char *v, char *Name)
{
	if (strlen(v) > 20)
		Err("name '%s' is longer than 20 characters", v);
	else if (!strchr(v, '*') || !strchr(v, ',') || *v == ' ')
		Err("name '%s' must contain ',' and '*' (e.g. \"sword,short *\")", v);
	else
		strcpy(Name, v);
}

static void ParseArticle(const char *v, ArticleType *Art)
{
	if (!strcmp(v, "a"))			*Art = A;
	else if (!strcmp(v, "an"))		*Art = An;
	else if (!strcmp(v, "some"))	*Art = Some;
	else if (!strcmp(v, "the"))		*Art = The;
	else if (!strcmp(v, "none"))	*Art = None;
	else Err("article must be a, an, some, the or none");
}

/* Spell names as in InitSpells() (obj.c), indexed by SPELL_* number. */
static const char	*Spells[] =
{
	"null", "vigor", "heal", "fireball", "lightning", "hurt", "curepoison",
	"disintegra", "befuddle", "teleport", "wish", "passdoor", "enchant",
	"bless", "protection", "curse", "poison", "intoxicate", "psi-blast",
	"polymorph", "summon", "animate", "charm", "illuminate", NULL
};

static int SpellNumber(const char *v)
{
	int		i;

	for (i = 1; Spells[i]; i++)
		if (!strcmp(v, Spells[i]))
			return i;
	Err("unknown spell '%s'", v);
	return 0;
}

/*
 * The world's id (#138): a UUID, 8-4-4-4-12 hex digits, kept in small
 * letters.  It names the world across every revision of its file.
 */
static void ParseId(const char *v)
{
	int		i;

	for (i = 0; v[i] && i < 36; i++)
		if (i == 8 || i == 13 || i == 18 || i == 23 ? v[i] != '-'
			: !isxdigit((unsigned char)v[i]))
			break;
	if (i != 36 || v[36])
	{
		Err("id must be a UUID, such as 3f1c9a52-6b8e-4d7e-9a41-2c5b0e7f8d13"
			" (make one with uuidgen)");
		return;
	}
	for (i = 0; i < 36; i++)
		WorldId[i] = (char)tolower((unsigned char)v[i]);
	WorldId[36] = '\0';
}

/* ---------- block keys ---------- */

static void WorldKey(char *k, char *v)
{
	int		n;

	if (!strcmp(k, "name"))
		snprintf(WorldName, sizeof(WorldName), "%s", v);
	else if (!strcmp(k, "id"))
		ParseId(v);
	else if (!strcmp(k, "monster-speed"))
		ParseInt(v, 1, 3600, &MonSpeed);
	else if (!strcmp(k, "notice"))
		snprintf(WNotice, sizeof(WNotice), "%s", v);
	else if (!strncmp(k, "news-", 5) && k[5] >= '1' && k[5] <= '5' && !k[6])
		snprintf(WNews[k[5] - '1'], sizeof(WNews[0]), "%s", v);
	else if (!strcmp(k, "start-room"))
	{
		if (ParseInt(v, 1, MaxRoom, &n))
			StartRm = n, StartLine = LineNo;
	}
	else if (!strcmp(k, "revive-room"))
	{
		if (ParseInt(v, 1, MaxRoom, &n))
			ReviveRm = n, ReviveLine = LineNo;
	}
	else if (!strcmp(k, "jail-room"))
	{
		if (ParseInt(v, 1, MaxRoom, &n))
			JailRm = n, JailLine = LineNo;
	}
	else
		Err("unknown world key '%s'", k);
}

static void DescKey(char *k, char *v)
{
	int		Line;

	if (!ParseInt(k, 1, StrPerSeg, &Line))
		return;
	if (strlen(v) > 79)
		Err("description line is longer than 79 characters");
	else if (!isdigit((unsigned char)v[0]) || v[1] != '/')
		Err("description line must start with a phrase count and '/'");
	else
		strcpy(Desc[BlockId][Line - 1], v);
}

static void AreaKey(char *k, char *v)
{
	const char	*p;

	if (strcmp(k, "name"))
	{
		Err("unknown area key '%s'", k);
		return;
	}
	AreaNamed[BlockId] = TRUE;
	for (p = v; *p && isprint((unsigned char)*p); p++)
		;
	if (*p)
		Err("area name must be printable ASCII");
	else if (strlen(v) > AreaNameLen)
		Err("area name is longer than %d characters", AreaNameLen);
	else
		strcpy(AreaName[BlockId], v);
}

static void RoomKey(char *k, char *v)
{
	static const char	*Dirs[] =
		{"north", "south", "east", "west", "up", "down", NULL};
	RoomDef	*R = &Rooms[BlockId];
	int		i, n;

	for (i = 0; Dirs[i]; i++)
		if (!strcmp(k, Dirs[i]))
		{
			if (ParseInt(v, 0, MaxRoom, &n))
				CurRoom->Adjoin[i] = (RmCodeType)n;
			return;
		}

	if (!strcmp(k, "out"))
	{
		if (ParseInt(v, 0, MaxRoom, &n))
			CurRoom->Out = (RmCodeType)n;
	}
	else if (!strcmp(k, "desc"))
		ParseDesc(v, &CurRoom->DescRec, &CurRoom->DescCode);
	else if (!strcmp(k, "name"))
	{
		const char	*p;

		for (p = v; *p && isprint((unsigned char)*p); p++)
			;
		if (*p)
			Err("room name must be printable ASCII");
		else if (strlen(v) > RoomNameLen)
			Err("room name is longer than %d characters", RoomNameLen);
		else
			strcpy(CurRoom->Name, v);
	}
	else if (!strcmp(k, "encounter-table"))
	{
		if (ParseInt(v, 0, LEncounter, &n))
			CurRoom->WhichEncounter = (uchar)n;
	}
	else if (!strcmp(k, "encounter-rate"))
	{
		if (ParseInt(v, 0, 255, &n))
			CurRoom->EncounterTime = (uchar)n;
	}
	else if (!strcmp(k, "safe"))		CurRoom->Safe = ParseBool(v);
	else if (!strcmp(k, "anti-magic"))	CurRoom->AntiMagic = ParseBool(v);
	else if (!strcmp(k, "dark"))		CurRoom->Dark = ParseBool(v);
	else if (!strcmp(k, "notify-dm"))	CurRoom->NotifyDM = ParseBool(v);
	else if (!strcmp(k, "area"))
	{
		if (ParseInt(v, 1, MaxArea, &n))
			R->Area = n, R->AreaLine = LineNo;
	}
	else if (!strcmp(k, "x") || !strcmp(k, "y") || !strcmp(k, "z"))
	{
		if (ParseInt(v, -MaxCoord, MaxCoord, &n))
		{
			*(*k == 'x' ? &R->X : *k == 'y' ? &R->Y : &R->Z) = n;
			R->HasXYZ = TRUE;
			R->XYZLine = LineNo;
		}
	}
	else if (!strcmp(k, "greeting"))
	{
		if (strlen(v) > 250)
			Err("greeting is longer than 250 characters");
		else if (!(R->Greeting = strdup(v)))
			exit(2);
	}
	else if (!strcmp(k, "role"))
	{
		for (i = 1; RoleNames[i] && strcmp(v, RoleNames[i]); i++)
			;
		if (!RoleNames[i])
			Err("role must be bar, newsstand, pawnshop, store or smith");
		else
			R->Role = i, R->RoleLine = LineNo;
	}
	else if (!strcmp(k, "stock"))
	{
		R->NStock = ParseList(v, 20, 1, RanObjLen, R->Stock);
		R->StockLine = LineNo;
	}
	else if (!strcmp(k, "staggers"))
	{
		R->NStagger = ParseList(v, 20, 1, MaxRoom, R->Stagger);
		R->StaggerLine = LineNo;
	}
	else if (!strcmp(k, "trains"))
	{
		char	*tok;

		for (tok = strtok(v, " \t,"); tok; tok = strtok(NULL, " \t,"))
		{
			for (i = 0; ClassNames[i] && strcmp(tok, ClassNames[i]); i++)
				;
			if (!ClassNames[i])
				Err("unknown class '%s'", tok);
			else
				R->Trains |= 1u << i;
		}
	}
	else
		Err("unknown room key '%s'", k);
}

static void MonKey(char *k, char *v)
{
	static const struct { const char *Key; int Bit; } Flags[] =
	{
		{"defend", 1},		{"block", 2},		{"follow", 3},
		{"guard", 4},		{"attack-last", 5},	{"slow-react", 6},
		{"fast-react", 7},	{"moral-react", 8},	{"immoral-react", 9},
		{"nice", 10},		{"flee", 11},		{"assistance", 12},
		{"casts-spells", 13}, {"permanent", 14}, {"magic", 15},
		{"invisible", 16},	{"regenerate", 17},	{"drain", 18},
		{"poison", 19},		{"anti-magic", 20},	{"undead", 21},
		{"summon-help", 22}, {"unique", 23},	{"watch", 24},
		{NULL, 0}
	};
	MonsterType	*M = CurMon;
	int			i, n, f;

	for (i = 0; Flags[i].Key; i++)
		if (!strcmp(k, Flags[i].Key))
		{
			f = ParseBool(v);
			switch (Flags[i].Bit)
			{
				case 1:  M->Defend = f;			break;
				case 2:  M->Block = f;			break;
				case 3:  M->Follow = f;			break;
				case 4:  M->Guard = f;			break;
				case 5:  M->AtkLastAggr = f;	break;
				case 6:  M->SlowReact = f;		break;
				case 7:  M->FastReact = f;		break;
				case 8:  M->MoralReact = f;		break;
				case 9:  M->ImmoralReact = f;	break;
				case 10: M->Nice = f;			break;
				case 11: M->Flee = f;			break;
				case 12: M->Assistance = f;		break;
				case 13: M->MonSpells = f;		break;
				case 14: M->Permanent = f;		break;
				case 15: M->Magic = f;			break;
				case 16: M->Invisible = f;		break;
				case 17: M->Regenerate = f;		break;
				case 18: M->Drain = f;			break;
				case 19: M->Poison = f;			break;
				case 20: M->AntiMagic = f;		break;
				case 21: M->Undead = f;			break;
				case 22: M->SummonHelp = f;		break;
				case 23: M->Unique = f;			break;
				case 24: M->Watch = f;			break;
			}
			return;
		}

	if (!strcmp(k, "name"))				ParseName(v, M->Name);
	else if (!strcmp(k, "article"))		ParseArticle(v, &M->Article);
	else if (!strcmp(k, "desc"))		ParseDesc(v, &M->DescRec, &M->DescCode);
	else if (!strcmp(k, "level"))		{ if (ParseInt(v, 0, 25, &n)) M->Lvl = (char)n; }
	else if (!strcmp(k, "hits"))		{ if (ParseInt(v, 0, 2500, &n)) M->Hits = (short)n; }
	else if (!strcmp(k, "max-hits"))	{ if (ParseInt(v, 0, 2500, &n)) M->MaxHits = (short)n; }
	else if (!strcmp(k, "ac"))			{ if (ParseInt(v, -25, 25, &n)) M->AC = (schar)n; }
	else if (!strcmp(k, "attack-speed")) { if (ParseInt(v, -10, 10, &n)) M->AtkSpeed = (schar)n; }
	else if (!strcmp(k, "parley"))		{ if (ParseInt(v, 0, 30, &n)) M->MParley = (char)n; }
	else if (!strcmp(k, "treasure-table")) { if (ParseInt(v, 0, ObjListLen, &n)) M->WhichObj = (uchar)n; }
	else
		Err("unknown monster key '%s'", k);
}

static int KindOf(const char *v)
{
	static const struct { const char *Name; ObClassType Class; } Kinds[] =
	{
		{"portal", Portal},		{"treasure", Treasure},	{"weapon", Weap},
		{"armor", Armor},		{"shield", Shield},		{"coins", Coins},
		{"scroll", Scroll},		{"chest", Chest},		{"door", Door},
		{"device", MagDevice},	{"teleport", Teleport},	{"keys", Keys},
		{"card", Card},			{"ring", Ring},			{"misc", Misc},
		{NULL, ObjDummy}
	};
	int		i;

	for (i = 0; Kinds[i].Name; i++)
		if (!strcmp(v, Kinds[i].Name))
			return Kinds[i].Class;
	Err("unknown object kind '%s'", v);
	return -1;
}

/* key valid only for some kinds */
static int For(int Ok, const char *k)
{
	if (!CurObjKind)
		Err("'%s' must come after kind", k);
	else if (!Ok)
		Err("'%s' does not apply to this kind of object", k);
	return CurObjKind && Ok;
}

static void ObjKey(char *k, char *v)
{
	ObjectType		*O = CurObj;
	union tObject	*U = &O->Object;
	ObClassType		C = O->ObClass;
	int				n;

	if (!strcmp(k, "name"))				ParseName(v, O->Name);
	else if (!strcmp(k, "article"))		ParseArticle(v, &O->Article);
	else if (!strcmp(k, "kind"))
	{
		if ((n = KindOf(v)) >= 0)
		{
			O->ObClass = (ObClassType)n;
			memset(U, 0, sizeof(*U));
			CurObjKind = TRUE;
		}
	}
	else if (!strcmp(k, "weight"))		{ if (ParseInt(v, 0, 1000, &n)) O->Weight = (short)n; }
	else if (!strcmp(k, "price"))		{ if (ParseInt(v, 0, 16000, &n)) O->Price = (short)n; }
	else if (!strcmp(k, "desc"))		ParseDesc(v, &O->DescRec, &O->DescCode);
	else if (!strcmp(k, "carry"))		O->Carry = ParseBool(v);
	else if (!strcmp(k, "magic"))		O->Magic = ParseBool(v);
	else if (!strcmp(k, "permanent"))	O->Permanent = ParseBool(v);
	else if (!strcmp(k, "invisible"))	O->Invisible = ParseBool(v);
	else if (!strcmp(k, "cursed"))		O->Cursed = ParseBool(v);

	/* weapon */
	else if (!strcmp(k, "min-damage"))
	{ if (For(C == Weap, k) && ParseInt(v, 0, 127, &n)) U->Weap.MinHp = (short)n; }
	else if (!strcmp(k, "max-damage"))
	{ if (For(C == Weap, k) && ParseInt(v, 0, 127, &n)) U->Weap.MaxHp = (short)n; }
	else if (!strcmp(k, "strikes"))
	{ if (For(C == Weap, k) && ParseInt(v, 0, 511, &n)) U->Weap.Strikes = (short)n; }
	else if (!strcmp(k, "skill"))
	{
		if (For(C == Weap, k))
		{
			if (!strcmp(v, "sharp"))		U->Weap.WeapType = Sharp;
			else if (!strcmp(v, "thrust"))	U->Weap.WeapType = Thrust;
			else if (!strcmp(v, "blunt"))	U->Weap.WeapType = Blunt;
			else if (!strcmp(v, "pole"))	U->Weap.WeapType = Long;
			else Err("skill must be sharp, thrust, blunt or pole");
		}
	}

	/* weapon, armor, shield */
	else if (!strcmp(k, "plus"))
	{
		if (For(C == Weap || C == Armor || C == Shield, k)
		&&	ParseInt(v, -5, 10, &n))
		{
			if (C == Weap)			U->Weap.WeaPlus = (short)n;
			else if (C == Armor)	U->Armor.ArmPlus = (short)n;
			else					U->Shield.ShPlus = (short)n;
		}
	}
	else if (!strcmp(k, "hits"))
	{
		if (For(C == Armor || C == Shield, k) && ParseInt(v, 0, 511, &n))
		{
			if (C == Armor)	U->Armor.ArmHits = (short)n;
			else			U->Shield.ShHits = (short)n;
		}
	}

	else if (!strcmp(k, "multiplier"))
	{ if (For(C == Coins, k) && ParseInt(v, 1, 127, &n)) U->Coins.Multiplier = (short)n; }

	/* scroll, ring, device */
	else if (!strcmp(k, "spell"))
	{
		if (For(C == Scroll || C == Ring || C == MagDevice, k))
		{
			n = SpellNumber(v);
			if (C == Scroll)		U->Scroll.Spell = (short)n;
			else if (C == Ring)		U->Ring.RSpell = (short)n;
			else					U->MagDevice.MSpell = (short)n;
		}
	}
	else if (!strcmp(k, "level"))
	{
		if (For(C == Ring || C == MagDevice, k) && ParseInt(v, 0, 25, &n))
		{
			if (C == Ring)	U->Ring.RLevel = (char)n;
			else			U->MagDevice.MLevel = (char)n;
		}
	}
	else if (!strcmp(k, "charges"))
	{ if (For(C == MagDevice, k) && ParseInt(v, 0, 127, &n)) U->MagDevice.NumCharges = (short)n; }

	/* chest, door */
	else if (!strcmp(k, "closed"))
	{
		if (For(C == Chest || C == Door, k))
		{
			if (C == Chest)	U->Chest.Closed = (char)ParseBool(v);
			else			U->Door.DClosed = ParseBool(v);
		}
	}
	else if (!strcmp(k, "trap"))
	{
		if (For(C == Chest || C == Door, k) && ParseInt(v, 0, 50, &n))
		{
			if (C == Chest)	U->Chest.Trap = (char)n;
			else			U->Door.DTrap = (short)n;
		}
	}
	else if (!strcmp(k, "lock"))
	{
		if (For(C == Chest || C == Door, k) && ParseInt(v, 0, 1023, &n))
		{
			if (C == Chest)	U->Chest.Locked = (short)n;
			else			U->Door.DLocked = (short)n;
		}
	}
	else if (!strcmp(k, "spring"))
	{ if (For(C == Door, k)) U->Door.DSpring = ParseBool(v); }
	else if (!strcmp(k, "toll"))
	{ if (For(C == Door, k)) U->Door.DToll = ParseBool(v); }

	/* portal, door, teleport */
	else if (!strcmp(k, "to"))
	{
		if (For(C == Portal || C == Door || C == Teleport, k)
		&&	ParseInt(v, 0, MaxRoom, &n))
		{
			if (C == Portal)		U->Portal.ToWhere = (RmCodeType)n;
			else if (C == Door)		U->Door.DToWhere = (RmCodeType)n;
			else					U->Teleport.TToWhere = (RmCodeType)n;
		}
	}
	else if (!strcmp(k, "active-room"))
	{ if (For(C == Teleport, k) && ParseInt(v, 0, MaxRoom, &n)) U->Teleport.TActiveRm = (short)n; }

	/* keys */
	else if (!strcmp(k, "unlocks"))
	{ if (For(C == Keys, k) && ParseInt(v, 0, 1023, &n)) U->Keys.UnLock = (short)n; }
	else if (!strcmp(k, "uses"))
	{ if (For(C == Keys, k) && ParseInt(v, 0, 127, &n)) U->Keys.NumUses = (short)n; }

	else
		Err("unknown object key '%s'", k);
}

static void ListKey(char *k, char *v, short Row[8], int Max)
{
	int		Vals[8], i, n;

	if (strcmp(k, Block == BEnc ? "monsters" : "objects"))
	{
		Err("unknown %s key '%s'", Block == BEnc ? "encounter" : "treasure", k);
		return;
	}
	n = ParseList(v, 8, 0, Max, Vals);
	for (i = 0; i < 8; i++)
		Row[i] = (short)(i < n ? Vals[i] : 0);
}

/* ---------- block headers ---------- */

static ONode *NewONode(void)
{
	ONode	*p = calloc(1, sizeof(*p));

	if (!p)
	{
		fprintf(stderr, "mkgame: out of memory\n");
		exit(2);
	}
	p->Obj.ObClass = ObjDummy;
	p->Line = LineNo;
	return p;
}


static void BeginObj(ObjectType *O)
{
	CurObj = O;
	CurObjKind = O->ObClass != ObjDummy;
	Block = BObj;
}

static MNode *NewMNode(void)
{
	MNode	*p = calloc(1, sizeof(*p));

	if (!p)
	{
		fprintf(stderr, "mkgame: out of memory\n");
		exit(2);
	}
	p->Line = LineNo;
	return p;
}

/* Parse "[kind N]" or "[room N / object K / object J]" etc. */
static void Header(char *h)
{
	char	*Tok[8];
	int		NTok = 0, Id, K, J, Hi;
	char	*t;

	Block = BSkip;
	for (t = strtok(h, " \t/"); t && NTok < 8; t = strtok(NULL, " \t/"))
		Tok[NTok++] = t;

	if (NTok == 1 && !strcmp(Tok[0], "world"))
	{
		Block = BWorld;
		WorldLine = LineNo;
		return;
	}
	if (NTok != 2 && NTok != 4 && NTok != 6)
	{
		Err("malformed block header");
		return;
	}

	Hi = !strcmp(Tok[0], "description") ? MaxDesc
	   : !strcmp(Tok[0], "area") ? MaxArea
	   : !strcmp(Tok[0], "room") ? MaxRoom
	   : !strcmp(Tok[0], "monster") ? RanMonLen
	   : !strcmp(Tok[0], "object") ? RanObjLen
	   : !strcmp(Tok[0], "encounter") ? LEncounter
	   : !strcmp(Tok[0], "treasure") ? ObjListLen : 0;
	if (!Hi)
	{
		Err("unknown block '%s'", Tok[0]);
		return;
	}
	if (!ParseInt(Tok[1], 1, Hi, &Id))
		return;
	BlockId = Id;

	if (NTok > 2 && strcmp(Tok[0], "room"))
	{
		Err("only rooms have nested blocks");
		return;
	}

	if (!strcmp(Tok[0], "description"))
	{
		if (DescDefined[Id]++)
			Err("description %d defined twice", Id);
		if (Id > MaxDescRec)
			MaxDescRec = Id;
		Block = BDesc;
	}
	else if (!strcmp(Tok[0], "area"))
	{
		if (AreaDefined[Id]++)
			Err("area %d defined twice", Id);
		AreaLine[Id] = LineNo;
		Block = BArea;
	}
	else if (!strcmp(Tok[0], "encounter"))
	{
		if (EncDefined[Id]++)
			Err("encounter %d defined twice", Id);
		Block = BEnc;
	}
	else if (!strcmp(Tok[0], "treasure"))
	{
		if (TreDefined[Id]++)
			Err("treasure %d defined twice", Id);
		Block = BTre;
	}
	else if (!strcmp(Tok[0], "monster"))
	{
		if (MDefined[Id]++)
			Err("monster %d defined twice", Id);
		MLine[Id] = LineNo;
		CurMon = &MTmpl[Id];
		Block = BMon;
	}
	else if (!strcmp(Tok[0], "object"))
	{
		if (ODefined[Id]++)
			Err("object %d defined twice", Id);
		OLine[Id] = LineNo;
		OTmpl[Id].ObClass = ObjDummy;
		BeginObj(&OTmpl[Id]);
	}
	else if (NTok == 2)		/* room */
	{
		if (Rooms[Id].Defined++)
			Err("room %d defined twice", Id);
		Rooms[Id].Line = LineNo;
		if (Id > MaxRoomDefined)
			MaxRoomDefined = Id;
		CurRoom = &Rooms[Id].Rm;
		Block = BRoom;
	}
	else
	{
		RoomDef	*R = &Rooms[Id];

		if (!R->Defined)
		{
			Err("[room %d] must come before its objects and monsters", Id);
			return;
		}
		if (!ParseInt(Tok[3], 1, SavedItems, &K))
			return;

		if (!strcmp(Tok[2], "object"))
		{
			ONode	**pp = &R->Objs[K];

			if (NTok == 6)
			{
				if (strcmp(Tok[4], "object") || !ParseInt(Tok[5], 1, SavedItems, &J))
				{
					Err("expected 'object J' inside an object");
					return;
				}
				if (!*pp)
				{
					Err("room %d object %d must come before its contents", Id, K);
					return;
				}
				pp = &(*pp)->Kids[J];
			}
			if (*pp)
			{
				Err("duplicate object block");
				return;
			}
			*pp = NewONode();
			BeginObj(&(*pp)->Obj);
		}
		else if (!strcmp(Tok[2], "monster"))
		{
			MNode	**mp = &R->Mons[K];

			if (NTok == 6)
			{
				ONode	**pp;

				if (strcmp(Tok[4], "object") || !ParseInt(Tok[5], 1, SavedItems, &J))
				{
					Err("expected 'object J' inside a monster");
					return;
				}
				if (!*mp)
				{
					Err("room %d monster %d must come before its inventory", Id, K);
					return;
				}
				pp = &(*mp)->Inv[J];
				if (*pp)
				{
					Err("duplicate object block");
					return;
				}
				*pp = NewONode();
				BeginObj(&(*pp)->Obj);
				return;
			}
			if (*mp)
			{
				Err("duplicate monster block");
				return;
			}
			*mp = NewMNode();
			CurMon = &(*mp)->Mon;
			Block = BMon;
		}
		else
			Err("rooms contain 'object K' and 'monster K' blocks");
	}
}

static void EndBlock(void)
{
	if (Block == BMon)
	{
		if (CurMon->MaxHits < CurMon->Hits)		/* as the editor does */
			CurMon->MaxHits = CurMon->Hits;
		CurMon->Num = 1;
		CurMon->Top = TRUE;
	}
	else if (Block == BObj && CurObj->ObClass == Weap
		 &&	 CurObj->Object.Weap.MaxHp < CurObj->Object.Weap.MinHp)
		CurObj->Object.Weap.MaxHp = CurObj->Object.Weap.MinHp + 1;
	Block = BNone;
}

static void ParseWorld(FILE *fp)
{
	char	Line[1024];

	while (fgets(Line, sizeof(Line), fp))
	{
		char	*p, *eq;

		LineNo++;
		if (!strchr(Line, '\n') && !feof(fp))
		{
			Err("line too long");
			return;
		}
		p = Trim(Line);
		if (!*p || *p == '#' || *p == ';')
			continue;

		if (*p == '[')
		{
			char	*e = strchr(p, ']');

			EndBlock();
			if (!e || *Trim(e + 1))
				Err("block header must end with ']'");
			else
			{
				*e = '\0';
				Header(p + 1);
			}
			continue;
		}

		if (!(eq = strchr(p, '=')))
		{
			Err("expected 'key = value'");
			continue;
		}
		*eq = '\0';
		{
			char	*k = Trim(p), *v = Trim(eq + 1);

			switch (Block)
			{
				case BNone:		Err("'%s' is outside any block", k);	break;
				case BSkip:		break;
				case BWorld:	WorldKey(k, v);							break;
				case BDesc:		DescKey(k, v);							break;
				case BArea:		AreaKey(k, v);							break;
				case BRoom:		RoomKey(k, v);							break;
				case BMon:		MonKey(k, v);							break;
				case BObj:		ObjKey(k, v);							break;
				case BEnc:		ListKey(k, v, Enc[BlockId], RanMonLen);	break;
				case BTre:		ListKey(k, v, Tre[BlockId], RanObjLen);	break;
			}
		}
	}
	EndBlock();
}

/* ---------- validation ---------- */

static void CheckRoomRef(int Line, const char *What, int Rm)
{
	if (Rm && !Rooms[Rm].Defined)
	{
		LineNo = Line;
		Err("%s refers to undefined room %d", What, Rm);
	}
}

static void CheckDesc(int Line, const char *What, int Rec, int Code)
{
	if (Rec && (!DescDefined[Rec] || (Code && !Desc[Rec][Code - 1][0])))
		Warn(Line, "%s uses description %d:%d, which is not defined",
			 What, Rec, Code);
}

static void CheckObj(ObjectType *O, int Line, const char *What)
{
	LineNo = Line;
	if (O->ObClass == ObjDummy)
		Err("%s has no kind", What);
	if (!O->Name[0])
		Err("%s has no name", What);
	CheckDesc(Line, What, O->DescRec, O->DescCode);
	if (O->ObClass == Portal)
		CheckRoomRef(Line, What, O->Object.Portal.ToWhere);
	else if (O->ObClass == Door)
		CheckRoomRef(Line, What, O->Object.Door.DToWhere);
	else if (O->ObClass == Teleport)
		CheckRoomRef(Line, What, O->Object.Teleport.TToWhere);
}

static void CheckMon(MonsterType *M, int Line, const char *What)
{
	LineNo = Line;
	if (!M->Name[0])
		Err("%s has no name", What);
	if (M->WhichObj && !TreDefined[M->WhichObj])
		Err("%s uses undefined treasure table %d", What, M->WhichObj);
	CheckDesc(Line, What, M->DescRec, M->DescCode);
}

static void Validate(void)
{
	char	What[64];
	int		i, j, k;

	for (i = 1; i <= RanObjLen; i++)
		if (ODefined[i])
		{
			snprintf(What, sizeof(What), "object %d", i);
			CheckObj(&OTmpl[i], OLine[i], What);
		}
	for (i = 1; i <= RanMonLen; i++)
		if (MDefined[i])
		{
			snprintf(What, sizeof(What), "monster %d", i);
			CheckMon(&MTmpl[i], MLine[i], What);
		}
	for (i = 1; i <= LEncounter; i++)
		for (j = 0; EncDefined[i] && j < 8; j++)
			if (Enc[i][j] && !MDefined[Enc[i][j]])
				Warn(0, "encounter %d uses undefined monster %d", i, Enc[i][j]);
	for (i = 1; i <= ObjListLen; i++)
		for (j = 0; TreDefined[i] && j < 8; j++)
			if (Tre[i][j] && !ODefined[Tre[i][j]])
				Warn(0, "treasure %d uses undefined object %d", i, Tre[i][j]);
	for (i = 1; i <= MaxArea; i++)
		if (AreaDefined[i] && !AreaNamed[i])
		{
			LineNo = AreaLine[i];
			Err("area %d has no name", i);
		}

	for (i = 1; i <= MaxRoom; i++)
	{
		RoomDef	*R = &Rooms[i];
		int		Gap = FALSE;

		if (!R->Defined)
			continue;
		snprintf(What, sizeof(What), "room %d", i);
		for (j = 0; j < 6; j++)
			CheckRoomRef(R->Line, What, R->Rm.Adjoin[j]);
		CheckRoomRef(R->Line, What, R->Rm.Out);
		CheckDesc(R->Line, What, R->Rm.DescRec, R->Rm.DescCode);
		if (R->Area && !AreaDefined[R->Area])
		{
			LineNo = R->AreaLine;
			Err("room %d is in undefined area %d", i, R->Area);
		}
		if (R->HasXYZ && !R->Area)
		{
			LineNo = R->XYZLine;
			Err("room %d has coordinates but no area", i);
		}
		for (j = 1; R->Area && j < i; j++)
			if (Rooms[j].Defined && Rooms[j].Area == R->Area
			&&	Rooms[j].X == R->X && Rooms[j].Y == R->Y && Rooms[j].Z == R->Z)
				Warn(R->Line, "room %d is at the same place as room %d",
					 i, j);
		if (R->Rm.WhichEncounter && !EncDefined[R->Rm.WhichEncounter])
		{
			LineNo = R->Line;
			Err("room %d uses undefined encounter table %d", i,
				R->Rm.WhichEncounter);
		}

		for (j = 1; j <= SavedItems; j++)
		{
			ONode	*o = R->Objs[j];

			if (!o)
			{
				Gap = TRUE;
				continue;
			}
			if (Gap)
				Warn(o->Line, "room %d objects are not numbered from 1"
					 " without gaps; they are packed in order", i);
			snprintf(What, sizeof(What), "room %d object %d", i, j);
			CheckObj(&o->Obj, o->Line, What);
			if (!o->Obj.Permanent)
				Warn(o->Line, "%s is not permanent: it is deleted the first"
					 " time the last player leaves the room (CleanRm)", What);

			for (k = 1; k <= SavedItems; k++)
				if (o->Kids[k])
				{
					char	KWhat[80];

					snprintf(KWhat, sizeof(KWhat), "%s content %d", What, k);
					CheckObj(&o->Kids[k]->Obj, o->Kids[k]->Line, KWhat);
					LineNo = o->Kids[k]->Line;
					if (o->Obj.ObClass != Chest)
						Err("only chests can contain objects");
					else if (o->Kids[k]->Obj.ObClass == Chest)
						Err("chests inside chests are not supported");
				}
		}

		Gap = FALSE;
		for (j = 1; j <= SavedItems; j++)
		{
			MNode	*m = R->Mons[j];

			if (!m)
			{
				Gap = TRUE;
				continue;
			}
			if (Gap)
				Warn(m->Line, "room %d monsters are not numbered from 1"
					 " without gaps; they are packed in order", i);
			snprintf(What, sizeof(What), "room %d monster %d", i, j);
			CheckMon(&m->Mon, m->Line, What);
			if (!m->Mon.Permanent)
				Warn(m->Line, "%s is not permanent: it is deleted the first"
					 " time the last player leaves the room (CleanRm)", What);
			for (k = 1; k <= SavedItems; k++)
				if (m->Inv[k])
				{
					char	KWhat[80];

					snprintf(KWhat, sizeof(KWhat), "%s object %d", What, k);
					CheckObj(&m->Inv[k]->Obj, m->Inv[k]->Line, KWhat);
				}
		}

		/* special-room settings */
		for (j = 0; j < R->NStock; j++)
			if (!ODefined[R->Stock[j]])
			{
				LineNo = R->StockLine;
				Err("room %d stocks undefined object %d", i, R->Stock[j]);
			}
		if (R->NStock && R->Role != RoleStore)
			Warn(R->StockLine, "room %d has stock but is not a store", i);
		if (R->Role == RoleStore && !R->NStock)
			Warn(R->RoleLine, "store room %d has no stock", i);
		for (j = 0; j < R->NStagger; j++)
			CheckRoomRef(R->StaggerLine, What, R->Stagger[j]);
		if (R->NStagger && R->Role != RoleBar)
			Warn(R->StaggerLine, "room %d has staggers but is not a bar", i);
	}

	if (!*WorldId)
	{
		LineNo = WorldLine;
		Err("the world has no id; add one to [world], such as"
			" 'id = 3f1c9a52-6b8e-4d7e-9a41-2c5b0e7f8d13' (make one with uuidgen)");
	}

	if (!ReviveRm)
		ReviveRm = StartRm;
	if (!JailRm)
		JailRm = StartRm;
	CheckRoomRef(StartLine, "start-room", StartRm);
	CheckRoomRef(ReviveLine, "revive-room", ReviveRm);
	CheckRoomRef(JailLine, "jail-room", JailRm);
}

/* ---------- output ---------- */

static void DBError(const char *Msg);

static char		GameDir[1024];

/* Each room's saved objects and monsters, linked through the nodes */
static ObjectPoint	RoomObjs[MaxRoom + 1];
static MonsterPoint	RoomMons[MaxRoom + 1];

/* Link the nodes From[1..SavedItems] that are there into a list */
static ObjectPoint Link(ONode **From)
{
	ObjectPoint	List = NULL, *Tail = &List;
	int			i;

	for (i = 1; i <= SavedItems; i++)
		if (From[i])
		{
			*Tail = &From[i]->Obj;
			Tail = &From[i]->Obj.Next;
		}
	*Tail = NULL;
	return List;
}

static int Count(ONode **From)
{
	int		i, n = 0;

	for (i = 1; i <= SavedItems; i++)
		n += From[i] != NULL;
	return n;
}

/*
 * Link each room's objects, chest contents, monsters and inventories into
 * the lists the game stores (dbstore.c), numbering duplicate monsters.
 */
static void BuildDB(void)
{
	int		Rm, i, j;

	for (Rm = 1; Rm <= MaxRoom; Rm++)
	{
		RoomDef			*R = &Rooms[Rm];
		MNode			*Mons[SavedItems];
		MonsterPoint	*Tail = &RoomMons[Rm];
		ObjectPoint		O;
		int				NMon = 0;

		if (!R->Defined)
			continue;

		RoomObjs[Rm] = Link(R->Objs);
		for (i = 1; i <= SavedItems; i++)
			if (R->Objs[i] && R->Objs[i]->Obj.ObClass == Chest)
			{
				ObjectType	*Box = &R->Objs[i]->Obj;

				Box->Object.Chest.NumInside = (char)Count(R->Objs[i]->Kids);
				Box->Object.Chest.ObjectTail = Link(R->Objs[i]->Kids);
				/* a chest in a chest keeps nothing (#44) */
				for (O = Box->Object.Chest.ObjectTail; O; O = O->Next)
					if (O->ObClass == Chest)
						O->Object.Chest.ObjectTail = NULL;
			}

		for (i = 1; i <= SavedItems; i++)
			if (R->Mons[i])
				Mons[NMon++] = R->Mons[i];
		for (i = 0; i < NMon; i++)
		{
			MonsterType	*M = &Mons[i]->Mon;
			int			Same = 0, Before = 0;

			/* number duplicates as FixMonCount() (mon.c) does */
			for (j = 0; j < NMon; j++)
				if (!strcmp(Mons[j]->Mon.Name, M->Name))
				{
					Same++;
					if (j < i)
						Before++;
				}
			M->Num = (char)(Same - Before > 9 ? 9 : Same - Before);
			M->Top = Before == 0;
			M->ObjectTail = Link(Mons[i]->Inv);
			for (O = M->ObjectTail; O; O = O->Next)
				if (O->ObClass == Chest)
					O->Object.Chest.ObjectTail = NULL;
			*Tail = M;
			Tail = &M->Next;
		}
		*Tail = NULL;
	}
}

/*
 * The world data, in the caller's transaction (dbstore.c): every template
 * slot, unused ones too, as the game has always had them, every
 * description record up to the highest, every room of every segment (room
 * 0 and empty rooms included) with its contents, the tables, and the
 * special rooms.  For an update (#137), what the new world no longer has
 * goes first: descriptions past its last, rooms past the last segment, and
 * the special rooms; each room's contents are replaced, so an empty room
 * is emptied.
 */
static int PutWorldData(int Update)
{
	int		i, Ok = TRUE, Row[8];

	if (Update)
	{
		char	Sql[256];

		snprintf(Sql, sizeof(Sql),
				 "DELETE FROM descriptions WHERE record > %d;"
				 "DELETE FROM room_stock; DELETE FROM room_staggers;"
				 "DELETE FROM room_trains; DELETE FROM room_specials;", MaxDescRec);
		Ok = sqlite3_exec(GameDB, Sql, NULL, NULL, NULL) == SQLITE_OK;
		snprintf(Sql, sizeof(Sql),
				 "DELETE FROM room_objects WHERE room > %d;"
				 "DELETE FROM room_monsters WHERE room > %d;"
				 "DELETE FROM rooms WHERE room > %d;", MaxRoom, MaxRoom, MaxRoom);
		Ok = Ok && sqlite3_exec(GameDB, Sql, NULL, NULL, NULL) == SQLITE_OK;
		if (!Ok)
			DBError(sqlite3_errmsg(GameDB));
	}
	for (i = 0; Ok && i <= MaxDescRec; i++)
		Ok = PutDesc(i, Desc[i]);
	for (i = 0; Ok && i <= RanMonLen; i++)
		Ok = PutMonTemplate(i, &MTmpl[i]);
	for (i = 0; Ok && i <= RanObjLen; i++)
		Ok = PutObjTemplate(i, &OTmpl[i]);
	for (i = 0; Ok && i <= MaxRoom; i++)
		Ok = PutRoom(i, &Rooms[i].Rm);
	for (i = 0; Ok && i <= MaxRoom; i++)
		Ok = (!(RoomObjs[i] || Update) || PutRoomObjs(i, RoomObjs[i]))
		  && (!(RoomMons[i] || Update) || PutRoomMons(i, RoomMons[i]));
	for (i = 1; Ok && i <= LEncounter; i++)
	{
		int		j;

		for (j = 0; j < 8; j++)
			Row[j] = Enc[i][j];
		Ok = PutTableRow(FALSE, i, Row);
	}
	for (i = 1; Ok && i <= ObjListLen; i++)
	{
		int		j;

		for (j = 0; j < 8; j++)
			Row[j] = Tre[i][j];
		Ok = PutTableRow(TRUE, i, Row);
	}
	for (i = 1; Ok && i <= MaxRoom; i++)
	{
		RoomDef	*R = &Rooms[i];

		if (R->Defined && (R->Role || R->Greeting || R->NStock || R->NStagger
						   || R->Trains))
			Ok = PutSpecial(i, (RoleType)R->Role, R->Greeting, R->NStock,
							R->Stock, R->NStagger, R->Stagger, R->Trains);
	}
	return Ok;
}

/* The world's record, W, from what was read */
static void ThisWorld(WorldType *W)
{
	snprintf(W->Id, sizeof(W->Id), "%s", WorldId);
	snprintf(W->Name, sizeof(W->Name), "%s", WorldName);
	snprintf(W->Revision, sizeof(W->Revision), "%s", Revision);
}

/* A new game's tables, in one transaction */
static void WriteTables(void)
{
	SettingsType	Set;
	WorldType		W;
	long			Made = (long)time(NULL);
	int				i, Ok;

	if (!DBOpen(GameDir) || !DBBegin())
	{
		fprintf(stderr, "mkgame: cannot write %s/%s\n", GameDir, DBName);
		exit(1);
	}
	Ok = PutWorldData(FALSE);

	/* a new game: no obituaries, open, on its first run */
	memset(&Set, 0, sizeof(Set));
	snprintf(Set.Notice, sizeof(Set.Notice), "%s", WNotice);
	for (i = 0; i < 5; i++)
		snprintf(Set.News[i], sizeof(Set.News[i]), "%s", WNews[i]);
	Set.TaskClosed = 'O';
	Set.Runs = 0;
	Set.MSpeed = MonSpeed;
	Set.StartRoom = StartRm;
	Set.ReviveRoom = ReviveRm;
	Set.JailRoom = JailRm;
	Ok = Ok && PutSettings(&Set);

	/* the world it was made from, and when (#138) */
	ThisWorld(&W);
	Ok = Ok && PutWorld(&W) && AddWorldEvent(Made, WMade, Revision)
		 && StampWorld(Made, TRUE);
	if (!Ok || !DBCommit())
	{
		DBRollback();
		fprintf(stderr, "mkgame: cannot write %s/%s\n", GameDir, DBName);
		exit(1);
	}
	DBClose();
}

/* ---------- updating a game (#137) ---------- */

/*
 * The world tables as they were before an update, saved as temp.old_TABLE,
 * to report what the update changed.  Cols are the columns compared: all
 * but the times, and a room's last_dir, which play changes.
 */
#define	RoomCols	"room, north, south, east, west, up, down, out, desc_rec," \
	" desc_code, which_encounter, encounter_time, notify_dm, safe, anti_magic, dark, name"

static const struct
{
	const char	*What, *Table, *Key, *Cols;
} Compared[] =
{
	{ "rooms",				"rooms",			"room",		RoomCols },
	{ "descriptions",		"descriptions",		"record",	"record, line, text" },
	{ "monster templates",	"monster_templates", "number",	"number, " MonCols },
	{ "object templates",	"object_templates",	"number",	"number, " ObjCols },
	{ "encounter tables",	"encounters",		"encounter", "encounter, slot, monster" },
	{ "treasure tables",	"treasures",		"treasure",	"treasure, slot, object" },
	{ "special rooms",		"room_specials",	"room",		"room, role, greeting" },
	{ "special rooms",		"room_stock",		"room",		"room, slot, object" },
	{ "special rooms",		"room_staggers",	"room",		"room, slot, to_room" },
	{ "special rooms",		"room_trains",		"room",		"room, class" },
	{ "room contents",		"room_objects",		"room",		"room, slot, " ObjCols },
	{ "room contents",		"chest_objects",	"room",		"room, chest, slot, " ObjCols },
	{ "room contents",		"room_monsters",	"room",		"room, slot, " MonCols },
	{ "room contents",		"monster_objects",	"room",		"room, monster, slot, " ObjCols },
	{ NULL }
};

static int Exec(const char *Sql)
{
	char	*Msg = NULL;

	if (sqlite3_exec(GameDB, Sql, NULL, NULL, &Msg) == SQLITE_OK)
		return TRUE;
	DBError(Msg ? Msg : sqlite3_errmsg(GameDB));
	sqlite3_free(Msg);
	return FALSE;
}

static int SaveOld(void)
{
	char	Sql[256];
	int		i;

	for (i = 0; Compared[i].What; i++)
	{
		snprintf(Sql, sizeof(Sql), "CREATE TEMP TABLE old_%s AS SELECT * FROM main.%s",
				 Compared[i].Table, Compared[i].Table);
		if (!Exec(Sql))
			return FALSE;
	}
	return TRUE;
}

/* Print the numbers (keys) the query Sql lists, as "  What: 1, 2, 3"; how many */
static int ListKeys(const char *What, const char *Sql)
{
	sqlite3_stmt	*St;
	int				N = 0, Len = 0;

	if (sqlite3_prepare_v2(GameDB, Sql, -1, &St, NULL) != SQLITE_OK)
	{
		DBError(sqlite3_errmsg(GameDB));
		return -1;
	}
	while (sqlite3_step(St) == SQLITE_ROW)
	{
		if (!N++)
			Len = printf("  %s:", What);
		if (Len > 70)
			Len = printf("\n   ");
		Len += printf(" %d", sqlite3_column_int(St, 0));
	}
	sqlite3_finalize(St);
	if (N)
		printf(" (%d)\n", N);
	return N;
}

/* A room in use: it leads somewhere, has a description or an encounter */
#define	Used	"(north OR south OR east OR west OR up OR down OR out OR desc_rec" \
	" OR desc_code OR which_encounter) <> 0"
#define	OldUsed	"(o.north OR o.south OR o.east OR o.west OR o.up OR o.down OR o.out" \
	" OR o.desc_rec OR o.desc_code OR o.which_encounter) <> 0"
#define	NewUsed	"(r.north OR r.south OR r.east OR r.west OR r.up OR r.down OR r.out" \
	" OR r.desc_rec OR r.desc_code OR r.which_encounter) <> 0"

/*
 * Report what the update changed, comparing the world tables with what
 * SaveOld() kept: rooms added (empty before), removed (empty now) and
 * changed, then each other kind of world data by its number.
 */
static void ReportChanges(void)
{
	static char	Sql[32768];
	int		i, j;

	ListKeys("rooms added", "SELECT room FROM temp.old_rooms WHERE NOT (" Used ")"
			 " AND room IN (SELECT room FROM main.rooms WHERE " Used ") ORDER BY room");
	ListKeys("rooms removed", "SELECT room FROM temp.old_rooms WHERE " Used
			 " AND room IN (SELECT room FROM main.rooms WHERE NOT (" Used ")) ORDER BY room");
	for (i = 0; Compared[i].What; i++)
	{
		size_t	Len;

		if (i && !strcmp(Compared[i].What, Compared[i - 1].What))
			continue;				/* listed with the first of its kind */
		Len = (size_t)snprintf(Sql, sizeof(Sql), "SELECT DISTINCT k FROM (");
		for (j = i; Compared[j].What && !strcmp(Compared[j].What, Compared[i].What); j++)
			Len += (size_t)snprintf(Sql + Len, sizeof(Sql) - Len,
				"%sSELECT %s AS k FROM (SELECT %s FROM temp.old_%s EXCEPT SELECT %s FROM main.%s)"
				" UNION SELECT %s FROM (SELECT %s FROM main.%s EXCEPT SELECT %s FROM temp.old_%s)",
				j == i ? "" : " UNION ", Compared[j].Key, Compared[j].Cols, Compared[j].Table,
				Compared[j].Cols, Compared[j].Table, Compared[j].Key, Compared[j].Cols,
				Compared[j].Table, Compared[j].Cols, Compared[j].Table);
		snprintf(Sql + Len, sizeof(Sql) - Len, ")%s ORDER BY k",
				 strcmp(Compared[i].What, "rooms") ? ""
				 : " WHERE k NOT IN (SELECT room FROM temp.old_rooms o JOIN main.rooms r"
				   " USING (room) WHERE (" OldUsed ") <> (" NewUsed "))");
		ListKeys(!strcmp(Compared[i].What, "rooms") ? "rooms changed" : Compared[i].What,
				 Sql);
	}
}

/*
 * Report the world data DMs changed since the world was last made or
 * updated (#138), which the update would overwrite; how many.
 */
static int ReportEdits(long Since)
{
	static const char	*Kind[] =
		{ "room", "description", "monster template", "object template",
		  "encounter table", "treasure table" };
	WorldEditType		E;
	int					K = -1, N = 0, Found, Count = 0;
	char				When[32];
	time_t				T;

	for (;;)
	{
		if (!NextWorldEdit(Since, &K, &N, &E, &Found))
			return -1;
		if (!Found)
			return Count;
		if (!Count++)
			printf("mkgame: changed by DMs since the world was last made or updated:\n");
		T = (time_t)E.Updated;
		strftime(When, sizeof(When), "%Y-%m-%d %H:%M:%S", localtime(&T));
		printf("  %s %d, %s\n", Kind[E.Kind], E.Number, When);
	}
}

/*
 * Move saved characters whose room the new world doesn't have to its start
 * room, naming them; how many, or -1.
 */
static int MoveStranded(void)
{
	sqlite3_stmt	*St, *Mv;
	int				N = 0, Ok = TRUE;

	if (sqlite3_prepare_v2(GameDB, "SELECT name, room FROM players ORDER BY id", -1,
						   &St, NULL) != SQLITE_OK
	||	sqlite3_prepare_v2(GameDB, "UPDATE players SET room = ? WHERE name = ?", -1,
						   &Mv, NULL) != SQLITE_OK)
	{
		DBError(sqlite3_errmsg(GameDB));
		sqlite3_finalize(St);
		return -1;
	}
	while (Ok && sqlite3_step(St) == SQLITE_ROW)
	{
		int		Rm = sqlite3_column_int(St, 1);

		if (Rm >= 1 && Rm <= MaxRoom && Rooms[Rm].Defined)
			continue;
		if (!N++)
			printf("  characters moved to the start room %d:", StartRm);
		printf(" %s (from %d)", (const char *)sqlite3_column_text(St, 0), Rm);
		Ok = sqlite3_bind_int(Mv, 1, StartRm) == SQLITE_OK
		  && sqlite3_bind_text(Mv, 2, (const char *)sqlite3_column_text(St, 0), -1,
							   SQLITE_TRANSIENT) == SQLITE_OK
		  && sqlite3_step(Mv) == SQLITE_DONE && sqlite3_reset(Mv) == SQLITE_OK;
	}
	if (N)
		printf("\n");
	if (!Ok)
		DBError(sqlite3_errmsg(GameDB));
	sqlite3_finalize(Mv);
	sqlite3_finalize(St);
	return Ok ? N : -1;
}

/*
 * Bring the game in GameDir up to this revision of its world (#137): check
 * it is the same world (or adopt it), refuse if DMs have changed world data
 * since the last make or update (unless Discard), back the database up,
 * then replace the world data in one transaction.  DryRun reports what
 * would change and rolls it all back.  Characters, DMs, news, the notice,
 * obituaries, the run counter, the monster speed, the login limits and the
 * rank history are kept.  Returns the exit status.
 */
static int UpdateGame(int DryRun, int Adopt, int Discard)
{
	SettingsType	Set;
	WorldType		Old, W;
	long			Since, Now = (long)time(NULL);
	int				Found, Edits, Ok;
	char			Backup[1200], Stamp[32];
	time_t			T = (time_t)Now;

	if (!DBOpen(GameDir))
	{
		fprintf(stderr, "mkgame: cannot update %s\n", GameDir);
		return 1;
	}
	if (!GetWorld(&Old, &Found) || !LastWorldTime(&Since))
		return 1;
	if (Found && strcmp(Old.Id, WorldId))
	{
		fprintf(stderr, "mkgame: %s is a game of world %s (%s), not %s (%s);"
				" it can only be updated from its own world\n", GameDir,
				*Old.Name ? Old.Name : "(no name)", Old.Id,
				*WorldName ? WorldName : "(no name)", WorldId);
		return 1;
	}
	if (!Found && !Adopt)
	{
		fprintf(stderr, "mkgame: %s doesn't record its world (it was made before"
				" schema version 4); if it is a game of %s, update it with"
				" --adopt\n", GameDir, *WorldName ? WorldName : FileName);
		return 1;
	}
	if (Found && Adopt)
		printf("mkgame: %s already records its world; --adopt changes nothing\n",
			   GameDir);

	if ((Edits = ReportEdits(Since)) < 0)
		return 1;
	if (Edits && !Discard)
	{
		fprintf(stderr, "mkgame: %s: %d change(s) by DMs, listed above, would be"
				" overwritten; copy any worth keeping into %s, then update with"
				" --discard-edits\n", GameDir, Edits, FileName);
		if (!DryRun)
			return 1;
	}
	if (Found && !strcmp(Old.Revision, Revision) && !Edits)
	{
		printf("mkgame: %s is already at revision %s of %s\n", GameDir, Revision,
			   *WorldName ? WorldName : FileName);
		return 0;
	}

	if (!DryRun)
	{
		int		N;

		/* a name no file has, even after an update in the same second */
		strftime(Stamp, sizeof(Stamp), "%Y%m%d-%H%M%S", localtime(&T));
		for (N = 1; N < 100; N++)
		{
			snprintf(Backup, sizeof(Backup), N == 1 ? "%s/%s.backup-%s"
					 : "%s/%s.backup-%s-%d", GameDir, DBName, Stamp, N);
			if (access(Backup, F_OK) && errno == ENOENT)
				break;
		}
		if (!DBBackup(GameDir, Backup))
			return 1;
		printf("mkgame: %s/%s: backed up to %s\n", GameDir, DBName, Backup);
	}

	printf("mkgame: %s %s from %s to revision %s:\n", GameDir,
		   DryRun ? "would be updated" : "updated",
		   Found ? Old.Revision : "an unknown revision", Revision);
	ThisWorld(&W);
	Ok = DBBegin() && SaveOld() && PutWorldData(TRUE)
	  && GetSettings(&Set);
	if (Ok)
	{
		Set.StartRoom = StartRm;		/* the rest is the game's own */
		Set.ReviveRoom = ReviveRm;
		Set.JailRoom = JailRm;
		Ok = PutSettings(&Set) && MoveStranded() >= 0 && PutWorld(&W)
		  && AddWorldEvent(Now, Found ? WUpdated : WAdopted, Revision)
		  && StampWorld(Now, FALSE);
	}
	if (Ok)
		ReportChanges();
	if (!Ok || DryRun || Edits && !Discard)
	{
		DBRollback();
		if (!Ok)
			fprintf(stderr, "mkgame: %s was not updated%s\n", GameDir,
					DryRun ? "" : "; it is as it was, beside its backup");
		DBClose();
		return Ok && !(Edits && !Discard) ? 0 : 1;
	}
	if (!DBCommit())
	{
		DBRollback();
		fprintf(stderr, "mkgame: %s was not updated; it is as it was, beside its"
				" backup\n", GameDir);
		return 1;
	}
	DBClose();
	return 0;
}

/* The SHA-256 of file Path's bytes, as shasum -a 256 prints it (#138) */
static int HashFile(const char *Path, char Hex[SHA256Hex])
{
	SHA256Type	S;
	char		Buf[8192];
	size_t		n;
	FILE		*fp;
	int			Ok;

	if (!(fp = fopen(Path, "rb")))
		return FALSE;
	SHA256Init(&S);
	while ((n = fread(Buf, 1, sizeof(Buf), fp)) > 0)
		SHA256Add(&S, Buf, n);
	Ok = !ferror(fp);
	fclose(fp);
	if (Ok)
		SHA256Hex64(&S, Hex);
	return Ok;
}

static void DBError(const char *Msg)
{
	fprintf(stderr, "mkgame: %s\n", Msg);
}

/*
 * GAMEDIR must exist, be a directory and hold nothing at all, hidden files
 * included, so a new game never mixes with an old one.  It is never
 * created here (#147).
 */
static int EmptyDir(const char *Dir)
{
	struct stat		st;
	struct dirent	*e;
	DIR				*d;
	int				N = 0;

	if (stat(Dir, &st))
	{
		fprintf(stderr, "mkgame: %s: %s; create the game directory first\n",
				Dir, strerror(errno));
		return FALSE;
	}
	if (!S_ISDIR(st.st_mode))
	{
		fprintf(stderr, "mkgame: %s: not a directory\n", Dir);
		return FALSE;
	}
	if (!(d = opendir(Dir)))
	{
		fprintf(stderr, "mkgame: %s: %s\n", Dir, strerror(errno));
		return FALSE;
	}
	while ((e = readdir(d)))
	{
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		if (N++ < 5)
			fprintf(stderr, "%s%s", N == 1 ? "mkgame: " : ", ", e->d_name);
	}
	closedir(d);
	if (N)
	{
		fprintf(stderr, "%s: %s is not empty; mkgame only makes a game in an"
				" empty directory\n", N > 5 ? ", ..." : "", Dir);
		return FALSE;
	}
	return TRUE;
}

static int Usage(void)
{
	fprintf(stderr, "usage: mkgame WORLD GAMEDIR\n"
			"       mkgame -u [-n] [--adopt] [--discard-edits] WORLD GAMEDIR\n");
	return 2;
}

int main(int argc, char **argv)
{
	static const struct option	Long[] =
	{
		{ "adopt", no_argument, NULL, 'a' },
		{ "discard-edits", no_argument, NULL, 'd' },
		{ NULL, 0, NULL, 0 }
	};
	FILE		*fp;
	int			i, NR = 0, NM = 0, NO = 0, Opt;
	int			Update = FALSE, DryRun = FALSE, Adopt = FALSE, Discard = FALSE;

	while ((Opt = getopt_long(argc, argv, "un", Long, NULL)) != -1)
		switch (Opt)
		{
			case 'u':	Update = TRUE;	break;
			case 'n':	DryRun = TRUE;	break;
			case 'a':	Adopt = TRUE;	break;
			case 'd':	Discard = TRUE;	break;
			default:	return Usage();
		}
	if (argc - optind != 2 || !Update && (DryRun || Adopt || Discard))
		return Usage();
	argv += optind - 1;
	DBReport = DBError;
	if (!Update && !EmptyDir(argv[2]))
		return 1;

	FileName = argv[1];
	if (!HashFile(FileName, Revision) || !(fp = fopen(FileName, "r")))
	{
		fprintf(stderr, "mkgame: %s: %s\n", FileName, strerror(errno));
		return 1;
	}
	for (i = 0; i <= RanObjLen; i++)
		OTmpl[i].ObClass = Misc;		/* unused slots: harmless misc */
	ParseWorld(fp);
	fclose(fp);
	for (i = 0; i <= RanObjLen; i++)
		if (!ODefined[i])
			OTmpl[i].ObClass = Misc;
	Validate();

	if (Errors)
	{
		fprintf(stderr, "mkgame: %d error(s), %d warning(s); %s\n", Errors, Warnings,
				Update ? "the game was not updated" : "no game written");
		return 1;
	}

	snprintf(GameDir, sizeof(GameDir), "%s", argv[2]);
	if (Update)
	{
		BuildDB();
		return UpdateGame(DryRun, Adopt, Discard);
	}
	if (!EmptyDir(GameDir) || !DBCreate(GameDir))
	{
		fprintf(stderr, "mkgame: no game written\n");
		return 1;
	}

	BuildDB();
	WriteTables();

	for (i = 1; i <= MaxRoom; i++)
		NR += Rooms[i].Defined;
	for (i = 1; i <= RanMonLen; i++)
		NM += MDefined[i];
	for (i = 1; i <= RanObjLen; i++)
		NO += ODefined[i];
	printf("mkgame: %s -> %s: %d rooms, %d descriptions, %d monster and"
		   " %d object templates, %d warning(s)\n",
		   *WorldName ? WorldName : FileName, GameDir, NR, MaxDescRec,
		   NM, NO, Warnings);
	printf("mkgame: world %s, revision %s\n", WorldId, Revision);
	return 0;
}
