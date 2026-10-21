/*
 * Special rooms: shops, training halls and the start/revive/jail rooms.
 *
 * The QNX version fixed these by room number.  They now come from the
 * world (see docs/world-format.md); mkgame writes them to dat/special,
 * one setting per line:
 *
 *	start ROOM				revive ROOM				jail ROOM
 *	role ROOM bar|newsstand|pawnshop|store|smith
 *	greeting ROOM text...
 *	stock ROOM OBJ...		staggers ROOM ROOM...	trains ROOM CLASS...
 */
#include "scepter.h"

#define	MaxSpecial	100
#define	MaxStock	20
#define	MaxStagger	20

typedef struct
{
	RmCodeType	Room;
	RoleType	Role;
	char		*Greeting;
	short		NStock, Stock[MaxStock];
	short		NStagger, Stagger[MaxStagger];
	unsigned	Trains;				/* bit (1 << ChType) */
} SpecialType;

static SpecialType	Special[MaxSpecial];
static int			NSpecial;

int		StartRoom = 1;
int		ReviveRoom = 1;
int		JailRoom = 1;

static char	*ClassNames[] =
{
	"fighter", "thief", "magic-user", "dm",
	"cleric", "paladin", "ranger", "barbarian", NULL
};

static char	*RoleNames[] =
{
	"none", "bar", "newsstand", "pawnshop", "store", "smith", NULL
};

static SpecialType *Find(int WRm)
{
	int		i;

	for (i = 0; i < NSpecial; i++)
		if (Special[i].Room == WRm)
			return &Special[i];
	return NULL;
}

static SpecialType *Add(int WRm)
{
	SpecialType	*p = Find(WRm);

	if (!p)
	{
		if (NSpecial == MaxSpecial)
			Abort(" Too many special rooms in SPECIAL.");
		p = &Special[NSpecial++];
		p->Room = (RmCodeType)WRm;
	}
	return p;
}

static int Lookup(char *Word, char **Names)
{
	int		i;

	for (i = 0; Names[i]; i++)
		if (!strcmp(Word, Names[i]))
			return i;
	return -1;
}

void LoadSpecial(void)
{
	FILE	*fp;
	char	Line[512];

	if (!(fp = fopen(SPECIAL, "r")))
		Abort(" File open error on SPECIAL.");

	while (fgets(Line, sizeof(Line), fp))
	{
		char		Key[16], *p;
		int			Rm, n = 0, Pos;
		SpecialType	*Sp;

		Line[strcspn(Line, "\r\n")] = '\0';
		if (!*Line || *Line == '#')
			continue;
		if (sscanf(Line, "%15s %d%n", Key, &Rm, &Pos) < 2
		||	Rm < 1 || Rm > NumRooms)
			Abort(" Bad line in SPECIAL.");
		p = Line + Pos;

		if (!strcmp(Key, "start"))
			StartRoom = Rm;
		else if (!strcmp(Key, "revive"))
			ReviveRoom = Rm;
		else if (!strcmp(Key, "jail"))
			JailRoom = Rm;
		else if (!strcmp(Key, "greeting"))
		{
			Sp = Add(Rm);
			while (*p == ' ')
				p++;
			free(Sp->Greeting);
			if (!(Sp->Greeting = malloc(strlen(p) + 1)))
				Abort(" Out of memory reading SPECIAL.");
			strcpy(Sp->Greeting, p);
		}
		else if (!strcmp(Key, "role"))
		{
			char	Word[16];

			Sp = Add(Rm);
			if (sscanf(p, "%15s", Word) != 1
			||	(n = Lookup(Word, RoleNames)) <= 0)
				Abort(" Bad role in SPECIAL.");
			Sp->Role = (RoleType)n;
		}
		else if (!strcmp(Key, "stock") || !strcmp(Key, "staggers"))
		{
			int		IsStock = !strcmp(Key, "stock");
			int		Hi = IsStock ? RanObjLen : NumRooms;

			Sp = Add(Rm);
			while (sscanf(p, "%d%n", &n, &Pos) == 1)
			{
				p += Pos;
				if (n < 1 || n > Hi)
					Abort(" Bad number in SPECIAL.");
				if (IsStock && Sp->NStock < MaxStock)
					Sp->Stock[Sp->NStock++] = (short)n;
				else if (!IsStock && Sp->NStagger < MaxStagger)
					Sp->Stagger[Sp->NStagger++] = (short)n;
			}
		}
		else if (!strcmp(Key, "trains"))
		{
			char	Word[16];

			Sp = Add(Rm);
			while (sscanf(p, "%15s%n", Word, &Pos) == 1)
			{
				p += Pos;
				if ((n = Lookup(Word, ClassNames)) < 0)
					Abort(" Bad class in SPECIAL.");
				Sp->Trains |= 1u << n;
			}
		}
		else
			Abort(" Unknown setting in SPECIAL.");
	}
	fclose(fp);
}

RoleType RoomRole(int WRm)
{
	SpecialType	*p = Find(WRm);

	return p ? p->Role : RoleNone;
}

char *RoomGreeting(int WRm)
{
	SpecialType	*p = Find(WRm);

	return p ? p->Greeting : NULL;
}

/* Object template number of item *Item* (1-based) for sale in *WRm*. */
int StockItem(int WRm, int Item)
{
	SpecialType	*p = Find(WRm);

	return p && Item >= 1 && Item <= p->NStock ? p->Stock[Item - 1] : 0;
}

int StockCount(int WRm)
{
	SpecialType	*p = Find(WRm);

	return p ? p->NStock : 0;
}

/* Where a drunk staggers to from bar *WRm*. */
int StaggerRoom(int WRm)
{
	SpecialType	*p = Find(WRm);

	return p && p->NStagger ? p->Stagger[Rnd(p->NStagger) - 1] : StartRoom;
}

/* Room *Item* (1-based) of the rooms a drunk staggers to from *WRm*. */
int StaggerItem(int WRm, int Item)
{
	SpecialType	*p = Find(WRm);

	return p && Item >= 1 && Item <= p->NStagger ? p->Stagger[Item - 1] : 0;
}

int StaggerCount(int WRm)
{
	SpecialType	*p = Find(WRm);

	return p ? p->NStagger : 0;
}

int TrainsClass(int WRm, ChType Class)
{
	SpecialType	*p = Find(WRm);

	return p && (p->Trains & (1u << Class));
}
