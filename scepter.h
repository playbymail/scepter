#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

/*#if defined(__HIGHC__) || defined(M_XENIX) || defined(mc68k)*/
# include <string.h>
/*# include <memory.h>*/
# define FOPEN	"r+"
/*#else*/	/* QNX? */
/*# define FOPEN	"rwo"*/
/*#endif*/

#define	FALSE	0
#define	TRUE	1

#define	UNUSED(x) x = x

#define	SavedItems	8
#define	MaxNames	40
#define	StrPerSeg	10

#define	CmdListLen	112		/* number of commands */

#define	LEncounter	200		/* encounters in EList */
#define	ObjListLen	200		/* treasures in TList */
#define	RanMonLen	200		/* monsters in MList */
#define	RanObjLen	200		/* objects in OList */
#define	MaxEvents	20		/* size of Event list */

#define	MaxUsers		12	/* Maximum number of users */
#define	MaxPlusOne		13	/* MaxUsers + 1 */
#define	RmLimit			130	/* MaxPlusOne * 10 */
#define	MaxSegs			199
#define	RoomNameLen		60	/* longest room name (#81) */
#define	MaxObjs			10	/* max number of objects saved in user record */
#define	MinPWLen		8	/* shortest new password */
#define	MaxPWLen		72	/* longest password: bcrypt ignores the rest */
#define	PWHashLen		64	/* bcrypt hash (60 characters) and NUL */
#define	LockedPW		"*"	/* a PWHash no password matches (#50) */
#define	PWCost			4	/* bcrypt cost: the minimum, for a small server */

#define	UpdatePeriod	3600	/* one hour */
#define	QuitWait		20
#define	HealWait		30

/* global types */
typedef unsigned char	uchar;
typedef signed char		schar;
typedef long			TimeType;
typedef short			RmCodeType;
typedef RmCodeType		AdjoinType[6];
typedef char			Alfa[11];
typedef unsigned short	TermIDType[20];
typedef char			*CmdListType[CmdListLen + 1];
typedef uchar			CmdNumType[CmdListLen + 1];
typedef Alfa			NameTList[MaxNames + 1];

/* spell types */
#define	OnUser		1
#define	OnPlayer	2
#define	OnMon		4
#define	OnObject	8

/* spells */
#define SPELL_NULL       0
#define SPELL_VIGOR      1
#define SPELL_HEAL       2
#define SPELL_FIREBALL   3
#define SPELL_LIGHTNING  4
#define SPELL_HURT       5
#define SPELL_CUREPOISON 6
#define SPELL_DISINTEGRA 7
#define SPELL_BEFUDDLE   8
#define SPELL_TELEPORT   9
#define SPELL_WISH       10
#define SPELL_PASSDOOR   11
#define SPELL_ENCHANT    12
#define SPELL_BLESS      13
#define SPELL_PROTECTION 14
#define SPELL_CURSE      15
#define SPELL_POISON     16
#define SPELL_INTOXICATE 17
#define SPELL_PSI_BLAST  18
#define SPELL_POLYMORPH  19
#define SPELL_SUMMON     20
#define SPELL_ANIMATE    21
#define SPELL_CHARM      22
#define SPELL_ILLUMINATE 23

typedef enum
	{Local, Yell, Others, All, BrOthers, BrAll, NoBlock, SysMsg} TalkHowType;
typedef enum
	{A, An, Some, The, None} ArticleType;
typedef enum
	{Male, Female} SexType;
typedef enum
	{DFlag, DNum, DWord, DOther} ParmType;
typedef enum
	{SInit, SLogin, SNormal} StatusType;
typedef enum	/* a character's DM rank, from SSJ, Assoc and Master (#139) */
	{RNone, RAssoc, RDM, RMaster} RankType;
typedef enum
	{Sharp, Thrust, Blunt, Long} SkillType;
typedef enum
	{ToNone, ToMonster, ToPlayer} ToWhatType;
typedef enum
	{Portal, Treasure, Weap, Armor, Shield, Coins, Scroll, Chest, Door,
	 MagDevice, Teleport, Keys, Card, Ring, Misc, ObjDummy} ObClassType;
typedef enum
	{Fighter, Thief, MagicUser, DM, Cleric,
	 Paladin, Ranger, Barbarian} ChType;
typedef enum
	{XInit, XName, XCmd, XEdit, XPassword, XNewPW, XChangePW, XNewClass,
	 XSex, XStats, XNotice, XDead, XNews, XSell, XRepair, XWish, XSpell,
	 XSure, XParley, XSkill, XDesc, XOldPW, XAgainPW, XRes3} EntryType;

typedef enum
	{RoleNone, RoleBar, RoleNewsstand, RolePawnshop, RoleStore, RoleSmith}
	RoleType;

typedef enum
{
	E_NULL = 0,
	E_MODIFY, E_DELETE, E_DISPLAY, E_CREATE, E_COPY, E_MOVE,
	E_QUIT, E_XQ, E_END
} EdFunc;

typedef struct
{
	char		*PName;
	ParmType	PType;
} PLType;

typedef struct
{
	int		SplHash;
	short	SplLen;
	short	SplMp;
	short	SplLvl;
	short	SplInt;
	int		SplType;
} SpellType;

union tObject
{
	struct tPortal
	{
		RmCodeType	ToWhere;
	} Portal;

	struct tWeap
	{
		short		MinHp, MaxHp;
		short		Strikes;
		short		WeaPlus;
		SkillType	WeapType;
	} Weap;

	struct tShield
	{
		short		ShPlus;
		short		ShHits;
	} Shield;

	struct tArmor
	{
		short		ArmPlus;
		short		ArmHits;
	} Armor;

	struct tCoins
	{
		short		Multiplier;
	} Coins;

	struct tScroll
	{
		short		Spell;
	} Scroll;

	struct tChest
	{
		char		Closed;
		char		Trap;
		short		Locked;
		char		NumInside;
		struct tObjectType	*ObjectTail;
	} Chest;

	struct tDoor
	{
		RmCodeType	DToWhere;
		unsigned	DClosed	: 1;
		unsigned	DSpring	: 1;
		unsigned	DToll	: 1;
		short		DTrap;
		short		DLocked;
	} Door;

	struct tKeys
	{
		short		UnLock;
		short		NumUses;
	} Keys;

	struct tMagDevice
	{
		short		MSpell;
		short		NumCharges;
		char		MLevel;
	} MagDevice;

	struct tTeleport
	{
		RmCodeType	TToWhere;
		short		TActiveRm;
	} Teleport;

	struct tRing
	{
		short		RSpell;
		char		RLevel;
	} Ring;
};

typedef struct tObjectType
{
	char		Name[21];
	struct tObjectType	*Next;
	ArticleType	Article;
	short		Weight;
	short		Price;
	uchar		DescCode, DescRec;
	unsigned	Carry		: 1;
	unsigned	Magic		: 1;
	unsigned	Permanent	: 1;
	unsigned	Invisible	: 1;
	unsigned	Cursed		: 1;
	ObClassType	ObClass;
	union tObject	Object;
} ObjectType, *ObjectPoint;

typedef struct tMonsterType
{
	char		Name[21];
	struct tMonsterType	*Next;
	struct tMonsterType	*DefMon;
	struct tUserType	*DefPlayer;
	struct tUserType	*CharmPlayer;
	ObjectPoint	ObjectTail;
	ArticleType	Article;
	uchar		DescRec, DescCode;
	schar		AtkSpeed;
	schar		AC;
	short		Hits, MaxHits;
	char		Lvl;
	char		Num;
	char		MReact, MParley;
	uchar		WhichObj;
	TimeType	Time;
	long		Filler;
	unsigned	Defend		: 1;
	unsigned	Block		: 1;
	unsigned	Follow		: 1;
	unsigned	Guard		: 1;
	unsigned	AtkLastAggr	: 1;
	unsigned	SlowReact	: 1;
	unsigned	FastReact	: 1;
	unsigned	Invisible	: 1;
	unsigned	Regenerate	: 1;
	unsigned	Drain		: 1;
	unsigned	Poison		: 1;
	unsigned	AntiMagic	: 1;
	unsigned	Undead		: 1;
	unsigned	MoralReact	: 1;
	unsigned	Flee		: 1;
	unsigned	Assistance	: 1;
	unsigned	MonSpells	: 1;
	unsigned	Top			: 1;
	unsigned	Permanent	: 1;
	unsigned	Magic		: 1;
	unsigned	SummonHelp	: 1;
	unsigned	ImmoralReact	: 1;
	unsigned	Unique		: 1;
	unsigned	Watch		: 1;
	unsigned	Nice		: 1;
} MonsterType, *MonsterPoint;

typedef struct tUserType	UserType, *UserPoint;

struct tUserType
{
	/* the first part is the data which gets saved */
	Alfa		Name;
	short		Lvl;
	ChType		Class;
	short		Hits, MaxHits;
	short		Fatigue, MaxFatigue;
	short		Magic, MaxMagic;
	long		Experience;
	RmCodeType	RmCode;
	short		Str, Int, Dex, Pty, Con;
	short		LastAccess;		/* mmmmdddddd */
	long		PW;					/* unused: the old hash, dropped (#50) */
	long		Money;
	SexType		Sex;
	short		SSharp, SThrust, SBlunt, SLong;
	short		Assoc;			/* if non-zero, restricts assoc-dm editing */
	long		Spells;			/* known spells */
	unsigned	SSJ			: 1;
	unsigned	Master		: 1;
	unsigned	PlayTester	: 1;
	unsigned	AGuild		: 1;
	unsigned	TGuild		: 1;
	unsigned	Evil		: 1;
	unsigned	Invisible	: 1;
	unsigned	NonExistant	: 1;
	unsigned	SkillNew	: 1;
	unsigned	SpellNew	: 1;
	unsigned	Poisoned	: 1;
	unsigned	Hidden		: 1;
	unsigned	It			: 1;
	unsigned	Suck		: 1;
	unsigned	Brief		: 1;
	unsigned	Echo		: 1;
	unsigned	MesBlock	: 1;
	unsigned	Dead		: 1;
	unsigned	Plague		: 1;
	char		PWHash[PWHashLen];	/* bcrypt (#82); "*" or empty: locked */

	/* The rest is data which is only applicable during play. */
	/* All these fields should be initialised at load time. */
	StatusType	Status;
	short		Trm;
	long		XPid;
	short		Weight;
	short		AC;
	ObjectPoint	ObjectTail;
	UserPoint	Next;			/* next in room */
	UserPoint	DefPlayer;
	UserPoint	Follow;
	UserPoint	NextUser;		/* next in game */
	ObjectPoint	USWeap, USArm, USShield, USRingL, USRingR;
	MonsterPoint	DefMon;
	EntryType	Entry;
	TimeType	LastInput;
	TimeType	LastAtk;
	TimeType	LastHeal;
	TimeType	HitAtTime;
	TimeType	Drunk;
	Alfa		LastCmd;
	Alfa		WData;
	long		Data;
	ObjectPoint	Pending;		/* item offered to REPAIR or PAWN (#33) */
	int			ParleyObj;		/* OList index offered by PARLEY (XParley) */
	char		NewPWHash[PWHashLen];	/* CHANGEPW: the new password, to confirm */
};

/* NOTE:  tUserSave must be EXACTLY the same as the first part of tUserType */

typedef struct tUserSave
{
	Alfa		Name;
	short		Lvl;
	ChType		Class;
	short		Hits, MaxHits;
	short		Fatigue, MaxFatigue;
	short		Magic, MaxMagic;
	long		Experience;
	RmCodeType	RmCode;
	short		Str, Int, Dex, Pty, Con;
	short		LastAccess;		/* mmmmdddddd */
	long		PW;					/* unused: the old hash, dropped (#50) */
	long		Money;
	SexType		Sex;
	short		SSharp, SThrust, SBlunt, SLong;
	short		Assoc;			/* if non-zero, restricts assoc-dm editing */
	long		Spells;			/* known spells */
	unsigned	SSJ			: 1;
	unsigned	Master		: 1;
	unsigned	PlayTester	: 1;
	unsigned	AGuild		: 1;
	unsigned	TGuild		: 1;
	unsigned	Evil		: 1;
	unsigned	Invisible	: 1;
	unsigned	NonExistant	: 1;
	unsigned	SkillNew	: 1;
	unsigned	SpellNew	: 1;
	unsigned	Poisoned	: 1;
	unsigned	Hidden		: 1;
	unsigned	It			: 1;
	unsigned	Suck		: 1;
	unsigned	Brief		: 1;
	unsigned	Echo		: 1;
	unsigned	MesBlock	: 1;
	unsigned	Dead		: 1;
	unsigned	Plague		: 1;
	char		PWHash[PWHashLen];	/* bcrypt (#82); "*" or empty: locked */
} UserSave;

/* The game's settings, as saved (dbstore.c), and the start rooms */
typedef struct
{
	char		Notice[256];
	char		News[5][256];
	int			ObitLvl[3];
	Alfa		ObitName[3];
	char		ObitClass[3][13];
	char		Obit[3][256];
	char		TaskClosed;			/* 'O' open, 'C' closed, 'T' testers only */
	int			Runs;				/* the run counter of the next run */
	int			MSpeed;
	int			StartRoom, ReviveRoom, JailRoom;
} SettingsType;

/* The login limits on an address and on a character (log.c, #93) */
typedef struct
{
	int			Fails;				/* since Start */
	int			Blocks;				/* refusals, for the doubling */
	long		Start, Until;
} LoginHostType;

typedef struct
{
	int			Fails;
	long		Last;
} LoginNameType;

/* the world a game was made from, its one row of world (#138) */
typedef struct
{
	char		Id[37];				/* a UUID, in small letters */
	char		Name[64];
	char		Revision[65];		/* the SHA-256 of the world file, in hex */
} WorldType;

typedef enum	/* a row of world_history: what happened to the world */
	{WMade, WUpdated, WAdopted} WorldEventKind;

/* a row of world_history (#138) */
typedef struct
{
	long		Time;
	int			Event;				/* a WorldEventKind */
	char		Revision[65];
	int			Schema;				/* the DBVersion that wrote it */
} WorldEventType;

typedef enum	/* world data a DM can edit, and an update replaces (#138) */
	{WRoom, WDescription, WMonster, WObject, WEncounter, WTreasure} WorldKind;

/* a row of world data changed since the world was last made or updated */
typedef struct
{
	int			Kind;				/* a WorldKind */
	int			Number;				/* the room, record, template or table row */
	long		Updated;
} WorldEditType;

/* a row of rank_changes (#139) */
typedef struct
{
	long		Time;
	Alfa		Player;				/* in capitals */
	int			Old, New;			/* a RankType; -1: made, or removed */
	char		By[64];				/* a character, or an account on the host */
	int			Tool;				/* by dmadmin, not in the game */
} RankChangeType;

typedef struct
{
	AdjoinType		Adjoin;
	RmCodeType		Out;
	uchar			DescRec, DescCode;
	uchar			WhichEncounter, EncounterTime;
	uchar			LastDir;
	char			Name[RoomNameLen + 1];	/* "": unnamed (#81) */
	MonsterPoint	RmMonsterTail;
	UserPoint		RmPlayerTail;
	ObjectPoint		RmObjectTail;
	unsigned		NotifyDM	: 1;
	unsigned		Safe		: 1;
	unsigned		AntiMagic	: 1;
	unsigned		Dark		: 1;
} RoomType;



/* global variables */

extern RoomType	Room[RmLimit];
extern int		SlotTbl[MaxPlusOne];
extern int		Active[MaxPlusOne];
extern RoomType	ProtoRoom;

extern int		NUsers;
extern TimeType	LastUpdate;

extern int		NumSegs;
extern short	Today;		/* mmmmdddddd */
extern int		NumRooms;

extern char		*DirList[7];
extern char		*CName[8];
extern char		*Pro[2];

extern UserPoint	User, UserTail;
extern UserType	ProtoUser;

extern CmdListType	CmdList;
extern CmdNumType	CmdNum;
extern PLType		Parms[43];

extern int		Term;

extern char		Buf[256];
extern int		Loc, LenBuf;
extern int		CmdCode;

extern TermIDType	TermList;
extern int			Pid;

extern char		B1[256], B2[256], B3[256], C1[256];

extern char		*Event[MaxEvents];
extern int			LenEvent;

extern MonsterType	ProtoMonster;

extern int			FolCount;
extern ObjectType	ProtoObject;

extern NameTList	SpellList;
extern SpellType	SpellClass[MaxNames];
extern int			Spellen;

extern char		Notice[256];
extern char		NewsBuf[5][256];
extern int		TLvl[3];
extern Alfa		TName[3];
extern char		TClass[3][13];
extern char		TBuf[3][256];
extern char		TaskClosed;
extern int		MSpeed;
extern int		NumRun;

extern char		SFObj[21], STObj[21];
extern Alfa		NFPlyr, NTPlyr, NFObj, NTObj;
extern int		FCash, TCash;
extern TimeType	TradeTime;

extern TimeType	RealTime;

extern char				DescBuf[StrPerSeg][80];
extern int				CurrentRec;

/* function declarations */
#include "gamedb.h"
#include "proto.h"

/* data files */
extern char	ERRLOG[];
extern char	PASSWD[];

/* special.c */
extern int	StartRoom, ReviveRoom, JailRoom;

#define	EncIndex(r, c)	GetETIndex(0, r, c)
#define	ObjIndex(r, c)	GetETIndex(1, r, c)
#define	PutEncIndex(r, c, v)	PutETIndex(0, r, c, v)
#define	PutObjIndex(r, c, v)	PutETIndex(1, r, c, v)
