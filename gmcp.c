/*
 * GMCP Room.Info, for Mudlet's mapper (#190, docs/running.md).
 *
 * RoomInfo() tells a player's client where they are: PlacePlayer() calls
 * it whenever a player's room changes, and LOOK calls it too.  A player
 * who can't see in a dark room is sent nothing, as the text tells them
 * nothing; DMs see dark rooms, so they are sent it there.
 *
 * Rooms are numbered as in the world file, or, with scepterd -o, by opaque
 * ids: a keyed permutation of the room number (a Feistel network on
 * SHA-256), small enough for Mudlet's room ids.  The key is RoomKeyFile in
 * the game directory,
 * made the first time it is needed, so a room keeps its id across
 * restarts but the ids give away neither room numbers nor their order.
 */
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/random.h>
#include <unistd.h>

#include "common.h"
#include "scepter.h"
#include "sha256.h"

#define	RoomKeyFile	"roomkey"
#define	KeyLen		32

static int				Opaque;
static unsigned char	Key[KeyLen];
static char				Area[2 * 64 + 3] = "\"\"";	/* the world's name, as JSON */

static const char	*ExitKey[7] = { "n", "s", "e", "w", "u", "d", "out" };
static const char	*RoleName[] =
	{ NULL, "bar", "newsstand", "pawnshop", "store", "smith" };

static int Hex(int c)
{
	return c >= '0' && c <= '9' ? c - '0'
		 : c >= 'a' && c <= 'f' ? c - 'a' + 10
		 : -1;
}

/* Read the room key, making one if the game has none yet. */
static int LoadKey(void)
{
	char	Text[2 * KeyLen + 2];
	int		fd, n, i;

	if ((fd = open(RoomKeyFile, O_RDONLY)) < 0 && errno == ENOENT)
	{
		if (getentropy(Key, KeyLen))
			return FALSE;
		for (i = 0; i < KeyLen; i++)
			snprintf(Text + 2 * i, 3, "%02x", Key[i]);
		Text[2 * KeyLen] = '\n';
		if ((fd = open(RoomKeyFile, O_WRONLY | O_CREAT | O_EXCL, 0600)) < 0)
			return FALSE;
		n = (int)write(fd, Text, 2 * KeyLen + 1);
		if (close(fd) || n != 2 * KeyLen + 1)
		{
			unlink(RoomKeyFile);
			return FALSE;
		}
		fprintf(stderr, "scepterd: made a new room key, %s\n", RoomKeyFile);
		return TRUE;
	}
	if (fd < 0)
		return FALSE;
	n = (int)read(fd, Text, sizeof Text);
	close(fd);
	if (n < 2 * KeyLen || n > 2 * KeyLen + 1
	||	(n == 2 * KeyLen + 1 && Text[2 * KeyLen] != '\n'))
	{
		errno = EINVAL;
		return FALSE;
	}
	for (i = 0; i < KeyLen; i++)
	{
		int		hi = Hex(Text[2 * i]), lo = Hex(Text[2 * i + 1]);

		if (hi < 0 || lo < 0)
		{
			errno = EINVAL;
			return FALSE;
		}
		Key[i] = (unsigned char)(hi << 4 | lo);
	}
	return TRUE;
}

/* Get ready to send Room.Info: with UseOpaque, number rooms by opaque ids. */
void GmcpInit(int UseOpaque)
{
	WorldType	W;
	int			Found;

	if (!GetWorld(&W, &Found) || !Found)
		*W.Name = '\0';
	JsonString(Area, sizeof Area, W.Name);

	Opaque = UseOpaque;
	if (Opaque && !LoadKey())
	{
		fprintf(stderr, "scepterd: %s: %s\n", RoomKeyFile,
				errno == EINVAL ? "not a room key" : strerror(errno));
		exit(1);
	}
}

/* Round Round of the Feistel network: 15 bits from the key, Round and Half */
static long Mix(int Round, long Half)
{
	SHA256Type	S;
	char		Data[32], Digest[SHA256Hex];

	snprintf(Data, sizeof Data, "%d %ld", Round, Half);
	SHA256Init(&S);
	SHA256Add(&S, Key, KeyLen);
	SHA256Add(&S, Data, strlen(Data));
	SHA256Hex64(&S, Digest);
	Digest[4] = '\0';
	return strtol(Digest, NULL, 16) & 0x7fff;
}

/* What the client calls room WRm (a world room number).  The opaque id is a
 * keyed permutation of the 30-bit numbers, so no two rooms share one. */
static long RoomId(int WRm)
{
	long	L = WRm >> 15 & 0x7fff, R = WRm & 0x7fff, T;
	int		i;

	if (!Opaque)
		return WRm;
	for (i = 0; i < 4; i++)
	{
		T = L ^ Mix(i, R);
		L = R;
		R = T;
	}
	return (L << 15 | R) + 1;	/* 1 to 2^30: Mudlet has no room 0 */
}

/* The first sentence of room Rm's brief description, for an unnamed room */
static void FirstSentence(int Rm, char *Str, int Max)
{
	char	Line[256], *p;
	int		n = 0, Cont = TRUE, i;

	*Str = '\0';
	for (i = Room[Rm].DescCode; Cont && i && i <= StrPerSeg; i++)
	{
		Pd(Room[Rm].DescRec, i, 1, TRUE, &Cont, Line);
		for (p = Line; *p && n < Max - 1; p++)
		{
			if (*p == '.' || *p == '!' || *p == '?')
			{
				Cont = FALSE;
				break;
			}
			if (*p != ' ' || n && Str[n - 1] != ' ')
				Str[n++] = *p;
		}
	}
	while (n && Str[n - 1] == ' ')
		n--;
	Str[n] = '\0';
}

/* Add "Key": Id to the exits in Out, leaving room in Max for the closing } */
static void AddExit(char *Out, int Max, const char *Key, int WRm)
{
	char	Name[64], Add[96];
	int		n = (int)strlen(Out), len;

	if (WRm <= 0)
		return;
	JsonString(Name, sizeof Name, Key);
	len = snprintf(Add, sizeof Add, "%s%s: %ld", n > 1 ? ", " : "", Name, RoomId(WRm));
	if (len > 0 && len < (int)sizeof Add && n + len < Max - 1)
		strcpy(Out + n, Add);
}

/* Send Plyr's client GMCP Room.Info for the room they are in. */
void RoomInfo(UserPoint Plyr)
{
	RoomType	*R = &Room[Plyr->RmCode];
	ObjectPoint	Obj;
	const char	*Env, *Name;
	char		Str[RoomNameLen + 1], JName[2 * RoomNameLen + 3];
	char		Exits[2048], Msg[4096];
	int			i;

	if (R->Dark && !Plyr->SSJ)
		return;

	if (!(Name = RoomName(Plyr->RmCode)))
	{
		FirstSentence(Plyr->RmCode, Str, sizeof Str);
		Name = Str;
	}
	JsonString(JName, sizeof JName, Name);

	i = RoomRole(W(Plyr->RmCode));
	Env = i > 0 && i < (int)(sizeof RoleName / sizeof *RoleName) ? RoleName[i]
		: R->Safe ? "safe"
		: R->AntiMagic ? "anti-magic"
		: R->Dark ? "dark"
		: "plain";

	/* the obvious exits, as RoomDisplay() lists them, then doors and
	 * portals the player can see, as the GO command that takes them */
	strcpy(Exits, "{");
	for (i = 0; i < 7; i++)
		if (i == 6 ? R->Out : R->Adjoin[i])
			AddExit(Exits, sizeof Exits, ExitKey[i], i == 6 ? R->Out : R->Adjoin[i]);
	for (Obj = R->RmObjectTail; Obj; Obj = Obj->Next)
		if ((Obj->ObClass == Portal || Obj->ObClass == Door)
		&&	(!Obj->Invisible || Plyr->SSJ))
		{
			char	Go[32];

			snprintf(Go, sizeof Go, "go %.*s", (int)strcspn(Obj->Name, ", "), Obj->Name);
			JsonString(Str, sizeof Str, Go);
			if (strstr(Exits, Str))
				continue;	/* GO takes the first of two with one name */
			AddExit(Exits, sizeof Exits, Go, Obj->ObClass == Door
					? Obj->Object.Door.DToWhere : Obj->Object.Portal.ToWhere);
		}
	strcat(Exits, "}");

	snprintf(Msg, sizeof Msg,
		"{\"num\": %ld, \"name\": %s, \"area\": %s, \"environment\": \"%s\", \"exits\": %s}",
		RoomId(W(Plyr->RmCode)), JName, Area, Env, Exits);
	QGmcp(Plyr->Trm, "Room.Info", Msg);
}
