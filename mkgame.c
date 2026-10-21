/*
 * mkgame -- compile a text world into the data files for a new game.
 *
 *	mkgame WORLD GAMEDIR
 *
 * WORLD is a world file (see docs/world-format.md).  GAMEDIR must be an
 * existing, empty directory; mkgame writes the game database (gamedb.c)
 * and the data files scepterd reads into it, and never creates or deletes
 * a directory or a file that was there before.  Run the server with
 * "scepterd GAMEDIR".  The world file itself is never written, so one
 * world can seed any number of games.
 *
 * Records are written with the engine's own structures (scepter.h), so
 * mkgame must be built from the same headers as scepterd.
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <sys/stat.h>

#include "common.h"
#include "scepter.h"

#define	NumSegsInit	200					/* main.c: NumSegs */
#define	MaxRoom		(NumSegsInit * 10 - 1)
#define	MaxDesc		255					/* DescRec is a uchar */
#define	DBSize		1009				/* db.c: hash table size */
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

	/* special-room settings, written to special (special.c) */
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

/* ---------- block keys ---------- */

static void WorldKey(char *k, char *v)
{
	int		n;

	if (!strcmp(k, "name"))
		snprintf(WorldName, sizeof(WorldName), "%s", v);
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

	if (!ReviveRm)
		ReviveRm = StartRm;
	if (!JailRm)
		JailRm = StartRm;
	CheckRoomRef(StartLine, "start-room", StartRm);
	CheckRoomRef(ReviveLine, "revive-room", ReviveRm);
	CheckRoomRef(JailLine, "jail-room", JailRm);
}

/* ---------- output ---------- */

static char		GameDir[1024];

static FILE *Create(const char *Name)
{
	char	Path[1100];
	FILE	*fp;

	snprintf(Path, sizeof(Path), "%s/%s", GameDir, Name);
	if (!(fp = fopen(Path, "wb")))
	{
		fprintf(stderr, "mkgame: %s: %s\n", Path, strerror(errno));
		exit(1);
	}
	return fp;
}

static void Close(FILE *fp, const char *Name)
{
	if (ferror(fp) | fclose(fp))
	{
		fprintf(stderr, "mkgame: error writing %s/%s\n", GameDir, Name);
		exit(1);
	}
}

/* A data file: its header (datafile.c), then N records of Kind. */
static void WriteBlob(const char *Name, int Kind, const void *Data, size_t N)
{
	FILE	*fp = Create(Name);

	if (!WriteDataHead(fp, Kind)
	||	N && fwrite(Data, DataKind[Kind].RecSize, N, fp) != N)
		Errors++;
	Close(fp, Name);
}

/* db.c: HashObj() */
static int DBHash(int Room, int Type)
{
	return (int)(((long)Room * 73 + Type) % DBSize);
}

static int DBSlot(RmCodeType *RoomOf(int), int Room, int Type)
{
	int		n = DBHash(Room, Type), First = n;

	while (*RoomOf(n))
	{
		if ((n = (n + 1) % DBSize) == First)
		{
			fprintf(stderr, "mkgame: database full\n");
			exit(1);
		}
	}
	return n;
}

static ObjRec	DBObj[DBSize];
static MonRec	DBMon[DBSize];

static RmCodeType *ObjRoomOf(int n)	{ return &DBObj[n].Room; }
static RmCodeType *MonRoomOf(int n)	{ return &DBMon[n].Room; }

static void PutObjRec(int Room, int Type, ONode **Nodes, int N)
{
	ObjRec	*Rec = &DBObj[DBSlot(ObjRoomOf, Room, Type)];
	int		i;

	Rec->Room = (RmCodeType)Room;
	Rec->DataType = (char)Type;
	Rec->Num = (char)N;
	for (i = 0; i < N; i++)
	{
		Rec->Obj[i] = Nodes[i]->Obj;
		Rec->Obj[i].Next = NULL;
		if (Rec->Obj[i].ObClass == Chest)
			Rec->Obj[i].Object.Chest.ObjectTail = NULL;
	}
}

static int Pack(ONode **From, ONode **To)
{
	int		i, n = 0;

	for (i = 1; i <= SavedItems; i++)
		if (From[i])
			To[n++] = From[i];
	return n;
}

static void BuildDB(void)
{
	int		Rm, i, j;

	for (Rm = 1; Rm <= MaxRoom; Rm++)
	{
		RoomDef	*R = &Rooms[Rm];
		ONode	*Objs[SavedItems];
		MNode	*Mons[SavedItems];
		int		NObj, NMon = 0, ChestNo = 1;

		if (!R->Defined)
			continue;

		NObj = Pack(R->Objs, Objs);
		if (NObj)
		{
			for (i = 0; i < NObj; i++)
				if (Objs[i]->Obj.ObClass == Chest)
				{
					int		n = 0;

					for (j = 1; j <= SavedItems; j++)
						n += Objs[i]->Kids[j] != NULL;
					Objs[i]->Obj.Object.Chest.NumInside = (char)n;
				}
			PutObjRec(Rm, 0, Objs, NObj);

			for (i = 0; i < NObj; i++)
			{
				ONode	*Kids[SavedItems];
				int		NKids;

				if (Objs[i]->Obj.ObClass != Chest)
					continue;
				NKids = Pack(Objs[i]->Kids, Kids);
				/* the Nth chest's contents are record N (ReadUObj) */
				if (NKids)
					PutObjRec(Rm, ChestNo, Kids, NKids);
				ChestNo++;
			}
		}

		for (i = 1; i <= SavedItems; i++)
			if (R->Mons[i])
				Mons[NMon++] = R->Mons[i];
		if (NMon)
		{
			MonRec	*Rec = &DBMon[DBSlot(MonRoomOf, Rm, 0)];

			Rec->Room = (RmCodeType)Rm;
			Rec->DataType = 0;
			Rec->Num = (char)NMon;
			for (i = 0; i < NMon; i++)
			{
				int		Same = 0, Before = 0;

				Rec->Mon[i] = Mons[i]->Mon;
				/* number duplicates as FixMonCount() (mon.c) does */
				for (j = 0; j < NMon; j++)
					if (!strcmp(Mons[j]->Mon.Name, Mons[i]->Mon.Name))
					{
						Same++;
						if (j < i)
							Before++;
					}
				Rec->Mon[i].Num = (char)(Same - Before > 9 ? 9 : Same - Before);
				Rec->Mon[i].Top = Before == 0;

				/* inventory: record type 9 + N, as in db.c */
				{
					ONode	*Inv[SavedItems];
					int		NInv = Pack(Mons[i]->Inv, Inv);

					if (NInv)
						PutObjRec(Rm, 9 + i, Inv, NInv);
				}
			}
		}
	}
}

/*
 * The game database's tables, in one transaction (dbstore.c).  Every
 * template slot is written, unused ones too, as the game has always had
 * them, every description record up to the highest, and every room of
 * every segment, room 0 and empty rooms included.
 */
static void WriteTables(void)
{
	int		i, Ok;

	if (!DBOpen(GameDir) || !DBBegin())
	{
		fprintf(stderr, "mkgame: cannot write %s/%s\n", GameDir, DBName);
		exit(1);
	}
	Ok = TRUE;
	for (i = 0; Ok && i <= MaxDescRec; i++)
		Ok = PutDesc(i, Desc[i]);
	for (i = 0; Ok && i <= RanMonLen; i++)
		Ok = PutMonTemplate(i, &MTmpl[i]);
	for (i = 0; Ok && i <= RanObjLen; i++)
		Ok = PutObjTemplate(i, &OTmpl[i]);
	for (i = 0; Ok && i <= MaxRoom; i++)
		Ok = PutRoom(i, &Rooms[i].Rm);
	if (!Ok || !DBCommit())
	{
		DBRollback();
		fprintf(stderr, "mkgame: cannot write %s/%s\n", GameDir, DBName);
		exit(1);
	}
	DBClose();
}

static void WriteGame(void)
{
	FILE	*fp;
	int		i, j;

	WriteBlob("dbobj", DObjDB, DBObj, DBSize);
	WriteBlob("dbmon", DMonDB, DBMon, DBSize);
	WriteBlob("eperson", DPerson, NULL, 0);

	WriteTables();

	/* special rooms, read by LoadSpecial() (special.c) */
	fp = Create("special");
	fprintf(fp, "# special rooms for world %s, written by mkgame\n",
			*WorldName ? WorldName : FileName);
	fprintf(fp, "start %d\nrevive %d\njail %d\n", StartRm, ReviveRm, JailRm);
	for (i = 1; i <= MaxRoom; i++)
	{
		RoomDef	*R = &Rooms[i];

		if (!R->Defined)
			continue;
		if (R->Role)
			fprintf(fp, "role %d %s\n", i, RoleNames[R->Role]);
		if (R->Greeting)
			fprintf(fp, "greeting %d %s\n", i, R->Greeting);
		if (R->NStock)
		{
			fprintf(fp, "stock %d", i);
			for (j = 0; j < R->NStock; j++)
				fprintf(fp, " %d", R->Stock[j]);
			fputc('\n', fp);
		}
		if (R->NStagger)
		{
			fprintf(fp, "staggers %d", i);
			for (j = 0; j < R->NStagger; j++)
				fprintf(fp, " %d", R->Stagger[j]);
			fputc('\n', fp);
		}
		if (R->Trains)
		{
			fprintf(fp, "trains %d", i);
			for (j = 0; ClassNames[j]; j++)
				if (R->Trains & (1u << j))
					fprintf(fp, " %s", ClassNames[j]);
			fputc('\n', fp);
		}
	}
	Close(fp, "special");

	/* same layout UpdateF() (cmd3.c) writes */
	fp = Create("efile");
	fprintf(fp, "%s\n", WNotice);
	for (i = 0; i < 5; i++)
		fprintf(fp, "%s\n", WNews[i]);
	for (i = 0; i < 3; i++)
		fprintf(fp, "0  \n\n");
	fprintf(fp, "O 0 %d\n", MonSpeed);
	for (i = 1; i <= LEncounter; i++)
		for (j = 0; j < 8; j++)
			fprintf(fp, "%d%c", Enc[i][j], j == 7 ? '\n' : ' ');
	for (i = 1; i <= ObjListLen; i++)
		for (j = 0; j < 8; j++)
			fprintf(fp, "%d%c", Tre[i][j], j == 7 ? '\n' : ' ');
	Close(fp, "efile");
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

int main(int argc, char **argv)
{
	FILE		*fp;
	int			i, NR = 0, NM = 0, NO = 0;

	if (argc != 3)
	{
		fprintf(stderr, "usage: mkgame WORLD GAMEDIR\n");
		return 2;
	}
	DBReport = DBError;
	if (!EmptyDir(argv[2]))
		return 1;

	FileName = argv[1];
	if (!(fp = fopen(FileName, "r")))
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
		fprintf(stderr, "mkgame: %d error(s), %d warning(s); no game written\n",
				Errors, Warnings);
		return 1;
	}

	snprintf(GameDir, sizeof(GameDir), "%s", argv[2]);
	if (!EmptyDir(GameDir) || !DBCreate(GameDir))
	{
		fprintf(stderr, "mkgame: no game written\n");
		return 1;
	}

	BuildDB();
	WriteGame();

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
	return 0;
}
