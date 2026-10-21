#include "common.h"
#include "scepter.h"

struct tUserFRec	UserFRec;

/* global variables */
FILE		*EDBMon;
FILE		*EDBObj;
FILE		*EFile;

RoomType	Room[RmLimit];
int			SlotTbl[MaxPlusOne];
int			Active[MaxPlusOne];
RoomType	ProtoRoom;

int			NUsers;
TimeType	LastUpdate;

int			NumSegs;
short		Today;		/* mmmmdddddd */
int			NumRooms;

char		*DirList[7] =
{
	"north", "south", "east", "west", "up", "down", "out"
};
char		*CName[8] =
{
	"Fighter", "Thief", "Magic-User", "Caretaker",
	"Cleric", "Paladin", "Ranger", "Barbarian"
};
char		*Pro[2] = {"him", "her"};

UserPoint	User, UserTail;
UserType	ProtoUser;

CmdListType	CmdList;
CmdNumType	CmdNum;
PLType		Parms[43];	/* at least as big as biggest list used in editor */

int			Term;

char		Buf[256];
int			Loc, LenBuf;
int			CmdCode;

TermIDType	TermList;
int			Pid;

char		B1[256], B2[256], B3[256], C1[256];

char		*Event[MaxEvents];
int			LenEvent;

MonsterType	ProtoMonster;

int			FolCount;
ObjectType	ProtoObject;

NameTList	SpellList;
SpellType	SpellClass[MaxNames];
int			Spellen;

char		Notice[256];
char		NewsBuf[5][256];
int			TLvl[3];
Alfa		TName[3];
char		TClass[3][13];
char		TBuf[3][256];
char		TaskClosed;
int			MSpeed;
int			NumRun;

char		SFObj[21], STObj[21];
Alfa		NFPlyr, NTPlyr, NFObj, NTObj;
int			FCash, TCash;
TimeType	TradeTime;

TimeType	RealTime;

char				DescBuf[StrPerSeg][80];
int					CurrentRec;

/* function declarations */
#include "proto.h"

/* data files */
char	DBMON[]		= WORKDIR "/dbmon";
char	DBOBJ[]		= WORKDIR "/dbobj";
char	EFILE[]		= WORKDIR "/efile";
char	ERRLOG[]	= WORKDIR "/errlog";
char	PASSWD[]	= "/etc/passwd";
char	SPECIAL[]	= WORKDIR "/special";
