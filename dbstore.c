/*
 * Game data as rows of the game database (gamedb.c, docs/data-files.md).
 *
 * Each struct the game saves has a pair of functions here: Bind* puts its
 * fields into a statement's parameters, and Col* reads them back from a
 * row.  Both refuse a value the schema can't hold or the struct can't take
 * (an enum out of range, a number too big for its field, a name too long),
 * naming the field, so invalid data never reaches the store or the game.
 * The Get* and Put* functions read and write whole records; each Put* is
 * one transaction, or part of the caller's if one is open.
 *
 * Every function returns FALSE after reporting a problem through DBReport.
 * This file uses no game state, so mkgame links it alone.
 */
#include <limits.h>
#include <string.h>

#include "common.h"
#include "scepter.h"

/* ---------- names of enum values, as stored ---------- */

static const char	*ArticleName[] = { "a", "an", "some", "the", "none", NULL };
static const char	*KindName[] =
{
	"portal", "treasure", "weapon", "armor", "shield", "coins", "scroll",
	"chest", "door", "device", "teleport", "keys", "card", "ring", "misc",
	"dummy", NULL
};
static const char	*SkillName[] = { "sharp", "thrust", "blunt", "pole", NULL };

/* ---------- reporting ---------- */

static void Bad(const char *Where, const char *Field, const char *Why, long Value)
{
	char	Msg[256];

	snprintf(Msg, sizeof(Msg), "%s: %s: %s (%ld)", Where, Field, Why, Value);
	DBReport(Msg);
}

static int Failed(const char *Where, const char *Doing)
{
	char	Msg[512];

	snprintf(Msg, sizeof(Msg), "%s: cannot %s: %s", Where, Doing,
			 sqlite3_errmsg(GameDB));
	DBReport(Msg);
	return FALSE;
}

/* ---------- binding parameters ---------- */

static int BindInt(sqlite3_stmt *St, int At, long Value)
{
	return sqlite3_bind_int64(St, At, Value) == SQLITE_OK;
}

static int BindFlag(sqlite3_stmt *St, int At, int Value, const char *Where,
					const char *Field)
{
	if (Value != 0 && Value != 1)
	{
		Bad(Where, Field, "must be 0 or 1", Value);
		return FALSE;
	}
	return BindInt(St, At, Value);
}

/* Text up to its NUL, or all Size bytes */
static int BindText(sqlite3_stmt *St, int At, const char *Text, size_t Size)
{
	return sqlite3_bind_text(St, At, Text, (int)strnlen(Text, Size),
							 SQLITE_TRANSIENT) == SQLITE_OK;
}

static int BindEnum(sqlite3_stmt *St, int At, int Value, const char **Names,
					const char *Where, const char *Field)
{
	int		N;

	for (N = 0; Names[N]; N++)
		;
	if (Value < 0 || Value >= N)
	{
		Bad(Where, Field, "no such value", Value);
		return FALSE;
	}
	return sqlite3_bind_text(St, At, Names[Value], -1, SQLITE_STATIC) == SQLITE_OK;
}

/* ---------- reading columns ---------- */

/* Column Col as a number from Lo to Hi; NULL or out of range is refused */
static int ColInt(sqlite3_stmt *St, int Col, long Lo, long Hi, long *Value,
				  const char *Where)
{
	sqlite3_int64	V;

	if (sqlite3_column_type(St, Col) != SQLITE_INTEGER)
	{
		Bad(Where, sqlite3_column_name(St, Col), "not a number", 0);
		return FALSE;
	}
	V = sqlite3_column_int64(St, Col);
	if (V < Lo || V > Hi)
	{
		Bad(Where, sqlite3_column_name(St, Col), "out of range", (long)V);
		return FALSE;
	}
	*Value = (long)V;
	return TRUE;
}

/* Column Col as text in a Size-byte field: NUL-terminated if it fits */
static int ColText(sqlite3_stmt *St, int Col, char *Text, size_t Size,
				   int NulNeeded, const char *Where)
{
	const unsigned char	*V = sqlite3_column_text(St, Col);
	size_t				Len = (size_t)sqlite3_column_bytes(St, Col);

	if (!V)
	{
		Bad(Where, sqlite3_column_name(St, Col), "missing", 0);
		return FALSE;
	}
	if (Len > Size - (NulNeeded ? 1 : 0))
	{
		Bad(Where, sqlite3_column_name(St, Col), "too long", (long)Len);
		return FALSE;
	}
	memset(Text, 0, Size);
	memcpy(Text, V, Len);
	return TRUE;
}

static int ColEnum(sqlite3_stmt *St, int Col, const char **Names, int *Value,
				   const char *Where)
{
	const char	*V = (const char *)sqlite3_column_text(St, Col);
	int			N;

	for (N = 0; V && Names[N]; N++)
		if (!strcmp(V, Names[N]))
		{
			*Value = N;
			return TRUE;
		}
	Bad(Where, sqlite3_column_name(St, Col), "no such value", 0);
	return FALSE;
}

/* Read column Col into a field of any integer type, checking its range */
#define	GET(Col, Field, Lo, Hi) \
	(ColInt(St, At + (Col), (Lo), (Hi), &V, Where) ? ((Field) = V, TRUE) : FALSE)
#define	GETFLAG(Col, Field)	GET(Col, Field, 0, 1)

/* ---------- objects ---------- */

/* Object columns, in the order of ObjCols (gamedb.h) */
enum
{
	OName, OArticle, OWeight, OPrice, ODescCode, ODescRec, OCarry, OMagic,
	OPermanent, OInvisible, OCursed, OKind, OToWhere, OMinHp, OMaxHp,
	OStrikes, OWeaPlus, OWeapType, OShPlus, OShHits, OArmPlus, OArmHits,
	OMultiplier, OSpell, OClosed, OTrap, OLocked, ONumInside, ODToWhere,
	ODClosed, ODSpring, ODToll, ODTrap, ODLocked, OUnLock, ONumUses, OMSpell,
	ONumCharges, OMLevel, OTToWhere, OTActiveRm, ORSpell, ORLevel, NObjCols
};

/*
 * Bind the object columns of O to parameters At to At + NObjCols - 1.  Only
 * the fields of O's own kind are stored; the others are NULL.
 */
int BindObj(sqlite3_stmt *St, int At, ObjectPoint O, const char *Where)
{
	union tObject	*U = &O->Object;
	int				Ok = TRUE, C;

	for (C = OToWhere; C < NObjCols; C++)
		Ok &= sqlite3_bind_null(St, At + C) == SQLITE_OK;
	Ok = Ok
	&&	BindText(St, At + OName, O->Name, sizeof(O->Name))
	&&	BindEnum(St, At + OArticle, O->Article, ArticleName, Where, "Article")
	&&	BindInt(St, At + OWeight, O->Weight)
	&&	BindInt(St, At + OPrice, O->Price)
	&&	BindInt(St, At + ODescCode, O->DescCode)
	&&	BindInt(St, At + ODescRec, O->DescRec)
	&&	BindInt(St, At + OCarry, O->Carry)
	&&	BindInt(St, At + OMagic, O->Magic)
	&&	BindInt(St, At + OPermanent, O->Permanent)
	&&	BindInt(St, At + OInvisible, O->Invisible)
	&&	BindInt(St, At + OCursed, O->Cursed)
	&&	BindEnum(St, At + OKind, O->ObClass, KindName, Where, "ObClass");
	if (!Ok)
		return FALSE;

	switch (O->ObClass)
	{
		case Portal:
			return BindInt(St, At + OToWhere, U->Portal.ToWhere);
		case Weap:
			return BindInt(St, At + OMinHp, U->Weap.MinHp)
				&& BindInt(St, At + OMaxHp, U->Weap.MaxHp)
				&& BindInt(St, At + OStrikes, U->Weap.Strikes)
				&& BindInt(St, At + OWeaPlus, U->Weap.WeaPlus)
				&& BindEnum(St, At + OWeapType, U->Weap.WeapType, SkillName,
							Where, "WeapType");
		case Shield:
			return BindInt(St, At + OShPlus, U->Shield.ShPlus)
				&& BindInt(St, At + OShHits, U->Shield.ShHits);
		case Armor:
			return BindInt(St, At + OArmPlus, U->Armor.ArmPlus)
				&& BindInt(St, At + OArmHits, U->Armor.ArmHits);
		case Coins:
			return BindInt(St, At + OMultiplier, U->Coins.Multiplier);
		case Scroll:
			return BindInt(St, At + OSpell, U->Scroll.Spell);
		case Chest:
			return BindFlag(St, At + OClosed, U->Chest.Closed, Where, "Closed")
				&& BindInt(St, At + OTrap, U->Chest.Trap)
				&& BindInt(St, At + OLocked, U->Chest.Locked)
				&& BindInt(St, At + ONumInside, U->Chest.NumInside);
		case Door:
			return BindInt(St, At + ODToWhere, U->Door.DToWhere)
				&& BindInt(St, At + ODClosed, U->Door.DClosed)
				&& BindInt(St, At + ODSpring, U->Door.DSpring)
				&& BindInt(St, At + ODToll, U->Door.DToll)
				&& BindInt(St, At + ODTrap, U->Door.DTrap)
				&& BindInt(St, At + ODLocked, U->Door.DLocked);
		case Keys:
			return BindInt(St, At + OUnLock, U->Keys.UnLock)
				&& BindInt(St, At + ONumUses, U->Keys.NumUses);
		case MagDevice:
			return BindInt(St, At + OMSpell, U->MagDevice.MSpell)
				&& BindInt(St, At + ONumCharges, U->MagDevice.NumCharges)
				&& BindInt(St, At + OMLevel, U->MagDevice.MLevel);
		case Teleport:
			return BindInt(St, At + OTToWhere, U->Teleport.TToWhere)
				&& BindInt(St, At + OTActiveRm, U->Teleport.TActiveRm);
		case Ring:
			return BindInt(St, At + ORSpell, U->Ring.RSpell)
				&& BindInt(St, At + ORLevel, U->Ring.RLevel);
		default:
			return TRUE;
	}
}

/* Read O from the object columns At to At + NObjCols - 1 of the row */
int ColObj(sqlite3_stmt *St, int At, ObjectPoint O, const char *Where)
{
	union tObject	*U = &O->Object;
	long			V;
	int				Article, Kind, Skill;

	memset(O, 0, sizeof(*O));
	if (!ColText(St, At + OName, O->Name, sizeof(O->Name), TRUE, Where)
	||	!ColEnum(St, At + OArticle, ArticleName, &Article, Where)
	||	!ColEnum(St, At + OKind, KindName, &Kind, Where)
	||	!GET(OWeight, O->Weight, SHRT_MIN, SHRT_MAX)
	||	!GET(OPrice, O->Price, SHRT_MIN, SHRT_MAX)
	||	!GET(ODescCode, O->DescCode, 0, UCHAR_MAX)
	||	!GET(ODescRec, O->DescRec, 0, UCHAR_MAX)
	||	!GETFLAG(OCarry, O->Carry)
	||	!GETFLAG(OMagic, O->Magic)
	||	!GETFLAG(OPermanent, O->Permanent)
	||	!GETFLAG(OInvisible, O->Invisible)
	||	!GETFLAG(OCursed, O->Cursed))
		return FALSE;
	O->Article = (ArticleType)Article;
	O->ObClass = (ObClassType)Kind;

	switch (O->ObClass)
	{
		case Portal:
			return GET(OToWhere, U->Portal.ToWhere, SHRT_MIN, SHRT_MAX);
		case Weap:
			if (!ColEnum(St, At + OWeapType, SkillName, &Skill, Where))
				return FALSE;
			U->Weap.WeapType = (SkillType)Skill;
			return GET(OMinHp, U->Weap.MinHp, SHRT_MIN, SHRT_MAX)
				&& GET(OMaxHp, U->Weap.MaxHp, SHRT_MIN, SHRT_MAX)
				&& GET(OStrikes, U->Weap.Strikes, SHRT_MIN, SHRT_MAX)
				&& GET(OWeaPlus, U->Weap.WeaPlus, SHRT_MIN, SHRT_MAX);
		case Shield:
			return GET(OShPlus, U->Shield.ShPlus, SHRT_MIN, SHRT_MAX)
				&& GET(OShHits, U->Shield.ShHits, SHRT_MIN, SHRT_MAX);
		case Armor:
			return GET(OArmPlus, U->Armor.ArmPlus, SHRT_MIN, SHRT_MAX)
				&& GET(OArmHits, U->Armor.ArmHits, SHRT_MIN, SHRT_MAX);
		case Coins:
			return GET(OMultiplier, U->Coins.Multiplier, SHRT_MIN, SHRT_MAX);
		case Scroll:
			return GET(OSpell, U->Scroll.Spell, SHRT_MIN, SHRT_MAX);
		case Chest:
			return GETFLAG(OClosed, U->Chest.Closed)
				&& GET(OTrap, U->Chest.Trap, CHAR_MIN, CHAR_MAX)
				&& GET(OLocked, U->Chest.Locked, SHRT_MIN, SHRT_MAX)
				&& GET(ONumInside, U->Chest.NumInside, CHAR_MIN, CHAR_MAX);
		case Door:
			return GET(ODToWhere, U->Door.DToWhere, SHRT_MIN, SHRT_MAX)
				&& GETFLAG(ODClosed, U->Door.DClosed)
				&& GETFLAG(ODSpring, U->Door.DSpring)
				&& GETFLAG(ODToll, U->Door.DToll)
				&& GET(ODTrap, U->Door.DTrap, SHRT_MIN, SHRT_MAX)
				&& GET(ODLocked, U->Door.DLocked, SHRT_MIN, SHRT_MAX);
		case Keys:
			return GET(OUnLock, U->Keys.UnLock, SHRT_MIN, SHRT_MAX)
				&& GET(ONumUses, U->Keys.NumUses, SHRT_MIN, SHRT_MAX);
		case MagDevice:
			return GET(OMSpell, U->MagDevice.MSpell, SHRT_MIN, SHRT_MAX)
				&& GET(ONumCharges, U->MagDevice.NumCharges, SHRT_MIN, SHRT_MAX)
				&& GET(OMLevel, U->MagDevice.MLevel, CHAR_MIN, CHAR_MAX);
		case Teleport:
			return GET(OTToWhere, U->Teleport.TToWhere, SHRT_MIN, SHRT_MAX)
				&& GET(OTActiveRm, U->Teleport.TActiveRm, SHRT_MIN, SHRT_MAX);
		case Ring:
			return GET(ORSpell, U->Ring.RSpell, SHRT_MIN, SHRT_MAX)
				&& GET(ORLevel, U->Ring.RLevel, CHAR_MIN, CHAR_MAX);
		default:
			return TRUE;
	}
}

/* ---------- monsters ---------- */

/* Monster columns, in the order of MonCols (gamedb.h) */
enum
{
	MName, MArticle, MDescRec, MDescCode, MAtkSpeed, MAC, MHits, MMaxHits,
	MLvl, MNum, MMReact, MMParley, MWhichObj, MDefend, MBlock, MFollow,
	MGuard, MAtkLastAggr, MSlowReact, MFastReact, MInvisible, MRegenerate,
	MDrain, MPoison, MAntiMagic, MUndead, MMoralReact, MFlee, MAssistance,
	MMonSpells, MTop, MPermanent, MMagic, MSummonHelp, MImmoralReact,
	MUnique, MWatch, MNice, NMonCols
};

/* Bind the monster columns of M to parameters At to At + NMonCols - 1 */
int BindMon(sqlite3_stmt *St, int At, MonsterPoint M, const char *Where)
{
	return BindText(St, At + MName, M->Name, sizeof(M->Name))
		&& BindEnum(St, At + MArticle, M->Article, ArticleName, Where, "Article")
		&& BindInt(St, At + MDescRec, M->DescRec)
		&& BindInt(St, At + MDescCode, M->DescCode)
		&& BindInt(St, At + MAtkSpeed, M->AtkSpeed)
		&& BindInt(St, At + MAC, M->AC)
		&& BindInt(St, At + MHits, M->Hits)
		&& BindInt(St, At + MMaxHits, M->MaxHits)
		&& BindInt(St, At + MLvl, M->Lvl)
		&& BindInt(St, At + MNum, M->Num)
		&& BindInt(St, At + MMReact, M->MReact)
		&& BindInt(St, At + MMParley, M->MParley)
		&& BindInt(St, At + MWhichObj, M->WhichObj)
		&& BindInt(St, At + MDefend, M->Defend)
		&& BindInt(St, At + MBlock, M->Block)
		&& BindInt(St, At + MFollow, M->Follow)
		&& BindInt(St, At + MGuard, M->Guard)
		&& BindInt(St, At + MAtkLastAggr, M->AtkLastAggr)
		&& BindInt(St, At + MSlowReact, M->SlowReact)
		&& BindInt(St, At + MFastReact, M->FastReact)
		&& BindInt(St, At + MInvisible, M->Invisible)
		&& BindInt(St, At + MRegenerate, M->Regenerate)
		&& BindInt(St, At + MDrain, M->Drain)
		&& BindInt(St, At + MPoison, M->Poison)
		&& BindInt(St, At + MAntiMagic, M->AntiMagic)
		&& BindInt(St, At + MUndead, M->Undead)
		&& BindInt(St, At + MMoralReact, M->MoralReact)
		&& BindInt(St, At + MFlee, M->Flee)
		&& BindInt(St, At + MAssistance, M->Assistance)
		&& BindInt(St, At + MMonSpells, M->MonSpells)
		&& BindInt(St, At + MTop, M->Top)
		&& BindInt(St, At + MPermanent, M->Permanent)
		&& BindInt(St, At + MMagic, M->Magic)
		&& BindInt(St, At + MSummonHelp, M->SummonHelp)
		&& BindInt(St, At + MImmoralReact, M->ImmoralReact)
		&& BindInt(St, At + MUnique, M->Unique)
		&& BindInt(St, At + MWatch, M->Watch)
		&& BindInt(St, At + MNice, M->Nice);
}

/* Read M from the monster columns At to At + NMonCols - 1 of the row */
int ColMon(sqlite3_stmt *St, int At, MonsterPoint M, const char *Where)
{
	long	V;
	int		Article;

	memset(M, 0, sizeof(*M));
	if (!ColText(St, At + MName, M->Name, sizeof(M->Name), TRUE, Where)
	||	!ColEnum(St, At + MArticle, ArticleName, &Article, Where))
		return FALSE;
	M->Article = (ArticleType)Article;
	return GET(MDescRec, M->DescRec, 0, UCHAR_MAX)
		&& GET(MDescCode, M->DescCode, 0, UCHAR_MAX)
		&& GET(MAtkSpeed, M->AtkSpeed, SCHAR_MIN, SCHAR_MAX)
		&& GET(MAC, M->AC, SCHAR_MIN, SCHAR_MAX)
		&& GET(MHits, M->Hits, SHRT_MIN, SHRT_MAX)
		&& GET(MMaxHits, M->MaxHits, SHRT_MIN, SHRT_MAX)
		&& GET(MLvl, M->Lvl, CHAR_MIN, CHAR_MAX)
		&& GET(MNum, M->Num, CHAR_MIN, CHAR_MAX)
		&& GET(MMReact, M->MReact, CHAR_MIN, CHAR_MAX)
		&& GET(MMParley, M->MParley, CHAR_MIN, CHAR_MAX)
		&& GET(MWhichObj, M->WhichObj, 0, UCHAR_MAX)
		&& GETFLAG(MDefend, M->Defend)
		&& GETFLAG(MBlock, M->Block)
		&& GETFLAG(MFollow, M->Follow)
		&& GETFLAG(MGuard, M->Guard)
		&& GETFLAG(MAtkLastAggr, M->AtkLastAggr)
		&& GETFLAG(MSlowReact, M->SlowReact)
		&& GETFLAG(MFastReact, M->FastReact)
		&& GETFLAG(MInvisible, M->Invisible)
		&& GETFLAG(MRegenerate, M->Regenerate)
		&& GETFLAG(MDrain, M->Drain)
		&& GETFLAG(MPoison, M->Poison)
		&& GETFLAG(MAntiMagic, M->AntiMagic)
		&& GETFLAG(MUndead, M->Undead)
		&& GETFLAG(MMoralReact, M->MoralReact)
		&& GETFLAG(MFlee, M->Flee)
		&& GETFLAG(MAssistance, M->Assistance)
		&& GETFLAG(MMonSpells, M->MonSpells)
		&& GETFLAG(MTop, M->Top)
		&& GETFLAG(MPermanent, M->Permanent)
		&& GETFLAG(MMagic, M->Magic)
		&& GETFLAG(MSummonHelp, M->SummonHelp)
		&& GETFLAG(MImmoralReact, M->ImmoralReact)
		&& GETFLAG(MUnique, M->Unique)
		&& GETFLAG(MWatch, M->Watch)
		&& GETFLAG(MNice, M->Nice);
}

/* ---------- transactions ---------- */

/* Start a transaction unless the caller has one; *Own says which */
static int Start(int *Own)
{
	*Own = sqlite3_get_autocommit(GameDB);
	return !*Own || DBBegin();
}

/* Finish what Start() began: commit if Ok, otherwise roll back */
static int Finish(int Own, int Ok)
{
	if (!Own)
		return Ok;
	if (Ok && DBCommit())
		return TRUE;
	DBRollback();
	return FALSE;
}

/* Run a statement that returns no rows */
static int Done(sqlite3_stmt *St, const char *Where, const char *Doing)
{
	return sqlite3_step(St) == SQLITE_DONE || Failed(Where, Doing);
}

/* ---------- templates ---------- */

/* Monster template N into M; *Found is FALSE, and M zero, if there is none */
int GetMonTemplate(int N, MonsterPoint M, int *Found)
{
	static sqlite3_stmt	*St;
	char				Where[48];
	int					Rc;

	snprintf(Where, sizeof(Where), "monster template %d", N);
	*Found = FALSE;
	memset(M, 0, sizeof(*M));
	if (!DBPrepare(&St, "SELECT " MonCols " FROM monster_templates WHERE number = ?")
	||	!BindInt(St, 1, N))
		return Failed(Where, "read");
	if ((Rc = sqlite3_step(St)) == SQLITE_ROW)
		*Found = Rc = ColMon(St, 0, M, Where);
	else
	if (!(Rc = Rc == SQLITE_DONE))
		Failed(Where, "read");
	sqlite3_reset(St);			/* ends the read */
	return Rc;
}

int PutMonTemplate(int N, MonsterPoint M)
{
	static sqlite3_stmt	*St;
	char				Where[48];
	int					Own;

	snprintf(Where, sizeof(Where), "monster template %d", N);
	if (!Start(&Own))
		return FALSE;
	return Finish(Own, DBPrepare(&St, "INSERT OR REPLACE INTO monster_templates"
								 " (number, " MonCols ") VALUES (?, " MonVals ")")
				  && BindInt(St, 1, N) && BindMon(St, 2, M, Where)
				  && Done(St, Where, "save"));
}

/* Object template N into O; *Found is FALSE, and O zero, if there is none */
int GetObjTemplate(int N, ObjectPoint O, int *Found)
{
	static sqlite3_stmt	*St;
	char				Where[48];
	int					Rc;

	snprintf(Where, sizeof(Where), "object template %d", N);
	*Found = FALSE;
	memset(O, 0, sizeof(*O));
	if (!DBPrepare(&St, "SELECT " ObjCols " FROM object_templates WHERE number = ?")
	||	!BindInt(St, 1, N))
		return Failed(Where, "read");
	if ((Rc = sqlite3_step(St)) == SQLITE_ROW)
		*Found = Rc = ColObj(St, 0, O, Where);
	else
	if (!(Rc = Rc == SQLITE_DONE))
		Failed(Where, "read");
	sqlite3_reset(St);			/* ends the read */
	return Rc;
}

int PutObjTemplate(int N, ObjectPoint O)
{
	static sqlite3_stmt	*St;
	char				Where[48];
	int					Own;

	snprintf(Where, sizeof(Where), "object template %d", N);
	if (!Start(&Own))
		return FALSE;
	return Finish(Own, DBPrepare(&St, "INSERT OR REPLACE INTO object_templates"
								 " (number, " ObjCols ") VALUES (?, " ObjVals ")")
				  && BindInt(St, 1, N) && BindObj(St, 2, O, Where)
				  && Done(St, Where, "save"));
}

/* ---------- descriptions ---------- */

/*
 * Description record Rec into Lines (StrPerSeg lines of 80 bytes, each
 * NUL-terminated unless it is 80 bytes long).  *Found is FALSE, and Lines
 * zero, if there is no such record.
 */
int GetDesc(int Rec, char Lines[StrPerSeg][80], int *Found)
{
	static sqlite3_stmt	*St;
	char				Where[48];
	long				Line;
	int					Rc;

	snprintf(Where, sizeof(Where), "description record %d", Rec);
	*Found = FALSE;
	memset(Lines, 0, StrPerSeg * 80);
	if (!DBPrepare(&St, "SELECT line, text FROM descriptions WHERE record = ?")
	||	!BindInt(St, 1, Rec))
		return Failed(Where, "read");
	while ((Rc = sqlite3_step(St)) == SQLITE_ROW)
	{
		if (!ColInt(St, 0, 1, StrPerSeg, &Line, Where)
		||	!ColText(St, 1, Lines[Line - 1], 80, FALSE, Where))
		{
			sqlite3_reset(St);
			return FALSE;
		}
		*Found = TRUE;
	}
	if (Rc != SQLITE_DONE)
		Failed(Where, "read");
	sqlite3_reset(St);			/* ends the read */
	return Rc == SQLITE_DONE;
}

/* Store all StrPerSeg lines of description record Rec */
int PutDesc(int Rec, char Lines[StrPerSeg][80])
{
	static sqlite3_stmt	*St;
	char				Where[48];
	int					Own, Line, Ok = TRUE;

	snprintf(Where, sizeof(Where), "description record %d", Rec);
	if (!Start(&Own))
		return FALSE;
	for (Line = 0; Ok && Line < StrPerSeg; Line++)
		Ok = DBPrepare(&St, "INSERT OR REPLACE INTO descriptions (record, line, text)"
					   " VALUES (?, ?, ?)")
		  && BindInt(St, 1, Rec) && BindInt(St, 2, Line + 1)
		  && BindText(St, 3, Lines[Line], 80)
		  && Done(St, Where, "save");
	return Finish(Own, Ok);
}
