#include <arpa/inet.h>
#include <netinet/in.h>

#include "scepter.h"

static void	AnnounceLogin(void);
static void	SetStats(ChType Cl, int Vit, int Fat, int Mag, int Mony);

/*
 * Limits on guessing passwords (#82).  An address with HostFails wrong
 * passwords within HostWindow seconds is refused for HostBlock seconds,
 * doubling each time up to HostBlockMax; the doubling is forgotten after
 * HostForget seconds without a refusal.  A character with NameFails wrong
 * passwords from anywhere, the last within NameWindow seconds, may be tried
 * only once every NameDelay seconds: slow enough to stop guessing, without
 * letting anyone lock the owner out.
 *
 * The counts are rows of the game database (dbstore.c, #93), so a restart
 * keeps them and no number of addresses or names can push one out.  Rows
 * that can no longer matter are pruned as new failures come in.
 */
#define	HostFails		5
#define	HostWindow		600
#define	HostBlock		600
#define	HostBlockMax	7200
#define	HostForget		86400
#define	NameFails		10
#define	NameWindow		3600
#define	NameDelay		30

/*
 * The key an address's limits are kept under: an IPv4 address as it is,
 * and an IPv6 address by its /64, since one subscriber is normally given a
 * whole /64 to choose addresses from.  An IPv4 address mapped into IPv6
 * counts as IPv4; anything else (the console) is its own key.
 */
static void HostKey(const char *Host, char *Key, size_t Len)
{
	struct in6_addr	A;
	char			Text[INET6_ADDRSTRLEN];

	if (inet_pton(AF_INET6, Host, &A) != 1)
		snprintf(Key, Len, "%s", Host);
	else
	if (IN6_IS_ADDR_V4MAPPED(&A)
	&&	inet_ntop(AF_INET, &A.s6_addr[12], Text, sizeof(Text)))
		snprintf(Key, Len, "%s", Text);
	else
	{
		memset(&A.s6_addr[8], 0, 8);
		if (!inet_ntop(AF_INET6, &A, Text, sizeof(Text)))
			snprintf(Text, sizeof(Text), "%s", Host);
		snprintf(Key, Len, "%s/64", Text);
	}
}

/* Name as its limits are kept: in capitals, as characters are saved */
static void NameKey(const char *Name, char *Key)
{
	snprintf(Key, sizeof(Alfa), "%s", Name);
	CapAlfa(Key);
}

/* The limits on Host and Name; a failure to read them stops the server */
static void GetLimits(const char *Host, const char *Name, char *HKey, char *NKey,
					  LoginHostType *H, LoginNameType *N)
{
	HostKey(Host, HKey, 64);
	NameKey(Name, NKey);
	if (!GetLoginHost(HKey, H) || !GetLoginName(NKey, N))
		Abort(" Cannot read the login limits.");
}

/* Seconds before Name may try a password from Host; 0 if it may now */
int LoginWait(const char *Host, char *Name)
{
	LoginHostType	H;
	LoginNameType	N;
	char			HKey[64];
	Alfa			NKey;

	GetLimits(Host, Name, HKey, NKey, &H, &N);
	if (H.Until > RealTime)
		return H.Until - RealTime;
	if (N.Fails >= NameFails && RealTime - N.Last < NameDelay)
		return N.Last + NameDelay - RealTime;
	return 0;
}

void LoginFailed(const char *Host, char *Name)
{
	LoginHostType	H;
	LoginNameType	N;
	char			HKey[64];
	Alfa			NKey;
	int				Own;

	if (!DBStart(&Own))
		Abort(" Cannot save the login limits.");
	GetLimits(Host, Name, HKey, NKey, &H, &N);
	if (RealTime - H.Start >= HostWindow)
	{
		H.Fails = 0;
		H.Start = RealTime;
	}
	if (RealTime - H.Until >= HostForget)
		H.Blocks = 0;
	if (RealTime - N.Last >= NameWindow)
		N.Fails = 0;
	N.Fails++;
	N.Last = RealTime;

	snprintf(B1, sizeof(B1), " Wrong password for %s from %s.", Name, Host);
	if (++H.Fails >= HostFails)
	{
		long	Block = MinL(HostBlockMax, (long)HostBlock << Min(H.Blocks, 4));

		H.Until = RealTime + Block;
		H.Blocks++;
		H.Fails = 0;
		snprintf(B1, sizeof(B1), " Wrong password for %s from %s;"
				 " address refused for %ld minutes.", Name, Host, Block / 60);
	}
	if (!DBFinish(Own, PutLoginHost(HKey, &H) && PutLoginName(NKey, &N)
				  && PruneLogins(RealTime - HostForget, RealTime - HostWindow,
								 RealTime - NameWindow)))
		Abort(" Cannot save the login limits.");
	ErrLog(B1);
}

void LoginOK(const char *Host, char *Name)
{
	LoginHostType	H;
	LoginNameType	N;
	char			HKey[64];
	Alfa			NKey;
	int				Own;

	if (!DBStart(&Own))
		Abort(" Cannot save the login limits.");
	GetLimits(Host, Name, HKey, NKey, &H, &N);
	H.Fails = 0;
	N.Fails = 0;
	if (!DBFinish(Own, (!H.Start && !H.Until || PutLoginHost(HKey, &H))
				  && (!N.Last || PutLoginName(NKey, &N))))
		Abort(" Cannot save the login limits.");
}

/* Tell the player how long LoginWait says to wait */
void TooManyPW(int Wait)
{
	if (Wait < 120)
		snprintf(B1, sizeof(B1), "0Too many wrong passwords.  Try again in %d seconds.", Wait);
	else
		snprintf(B1, sizeof(B1), "0Too many wrong passwords.  Try again in %d minutes.",
				(Wait + 59) / 60);
	QOut(Term, B1);
}

static void AnnounceLogin(void)
{
	User->Status = SNormal;
	*User->LastCmd = '\0';
	if (User->LastAccess != Today)
	{
		User->LastAccess = Today;
		if (User->TGuild || User->AGuild)
		{
			User->Money = MaxL(0L, User->Money - User->Lvl * 20);
			if (!User->Money)
			{
				QOut(Term, "0You have not the funds to pay your dues!");
				QOut(Term, "0You have been expelled from the guild.");
				User->AGuild = User->TGuild = FALSE;
			}
			else
			{
				snprintf(B1, sizeof(B1), "0Daily %d guild fee paid in full.  Thank you!",
						User->Lvl * 20);
				QOut(Term, B1);
			}
		}
	}

	CmdCode = 0;
	snprintf(B1, sizeof(B1), "0### Please welcome %s, the %s.",
			User->Name, CName[(int) User->Class]);
	AllMsg(B1);

	if (!User->SSJ && (User->Invisible || User->NonExistant))
		User->NonExistant = User->Invisible = FALSE;
	if (User->Invisible)
	{
		snprintf(B1, sizeof(B1), "0You are currently %s.",
				User->NonExistant ? "nonexistant" : "invisible");
		QOut(Term, B1);
	}

	RoomDisplay(User->RmCode, User->Brief);
}

UserPoint Login(void)
{
	UserPoint	NewUser;

	if (!(NewUser = malloc(sizeof(UserType))))
	{
		ErrLog(" Out of memory for a new connection.");
		QOut(Term, "2The game is out of memory.  Please try again later.");
		return NULL;
	}
	NUsers++;
	*NewUser = ProtoUser;
	snprintf(B1, sizeof(B1), "0Scepter Fantasy Role-Playing Game  Ver 2.4.3  (Run %d)",
			NumRun);
	QOut(Term, B1);

	if (*Notice)
	{
		snprintf(B1, sizeof(B1), "0Notice:  %s", Notice);
		QOut(Term, B1);
	}

	NewUser->Entry		= XName;
	NewUser->Status		= SLogin;
	NewUser->Trm		= Term;
	NewUser->XPid		= Pid;
	NewUser->LastCmd[0]	= '\0';
	NewUser->NextUser	= UserTail;
	UserTail = NewUser;

	return NewUser;
}

void EnterSex(void)
{
	char	Ch;
	int		Num;
	Alfa	Word;

	GetWord(Word, &Num);
	Ch = Cap(*Word);
	User->Entry = XCmd;
	if (Ch == 'M')
		User->Sex = Male;
	else
	if (Ch == 'F')
		User->Sex = Female;
	else
	{
		User->Entry = XSex;
		Loc = LenBuf + 1;
		QOut(Term, "0Try again.");
		return;
	}

	PlacePlayer(User, StartRoom);
	Loc = LenBuf + 1;
	AnnounceLogin();
}

void NewPW(void)
{
	char	PassWord[MaxPWLen + 1];
	int		Len;

	if (!(Len = GetPW(PassWord)))
		return;
	if (Len < MinPWLen)
	{
		snprintf(B1, sizeof(B1), "0Passwords must be %d to %d characters long.", MinPWLen,
				MaxPWLen);
		QOut(Term, B1);
	}
	else if (SetPW(User->PWHash, PassWord))
		User->Entry = XSex;
}

void EnterStats(void)
{
	int		ILoop, Sum;
	int		Stat[5];

	for (Sum = ILoop = 0; ILoop < 5; ILoop++)
	{
		int		Num;
		Alfa	Dummy;

		GetWord(Dummy, &Num);
		Stat[ILoop] = Num;
		Sum += Num;
		if (Sum > 55 || Num < 5 || Num > 18)
		{
			QOut(Term, Sum > 55 ?
					"0Error.  Your average is greater than 11."
				  : "0Error.  A number is more than 18 or less than 5.");
			User->Entry = XStats;
			Loc	= LenBuf + 1;
			return;
		}
	}

	User->Str = Stat[0];
	User->Int = Stat[1];
	User->Dex = Stat[2];
	User->Pty = Stat[3];
	User->Con = Stat[4];
	User->Entry = XSkill;

	if (Loc >= LenBuf)
	{
		QOut(Term, "0Which weapon is your most skillful?");
		QOut(Term,
			 "0Choose from:  Sharp weapons, thrusting weapons, blunt weapons,");
		QOut(Term, "0or pole weapons.");
	}
}

void EnterSkill(void)
{
	int		Ch, Num;
	Alfa	Word;

	User->Entry = XNewPW;
	GetWord(Word, &Num);
	Ch = Cap(Word[0]);
	switch (Ch)
	{
		case 'S': User->SSharp	= 1; break;
		case 'T': User->SThrust	= 1; break;
		case 'B': User->SBlunt	= 1; break;
		case 'P': User->SLong	= 1; break;
		default:
			User->Entry = XSkill;
			Loc = LenBuf + 1;
			QOut(Term, "0No such skill.  Try again.");
			return;
	}

	if (Loc >= LenBuf)
	{
		snprintf(B1, sizeof(B1), "0Choose a password of %d to %d characters.  Spaces are allowed,",
				MinPWLen, MaxPWLen);
		QOut(Term, B1);
		QOut(Term, "0and capital letters are different from small ones.");
	}
}

static void SetStats(ChType Cl, int Vit, int Fat, int Mag, int Mony)
{
	User->Class = Cl;
	User->Hits = User->MaxHits = Vit;
	User->Fatigue = User->MaxFatigue = Fat;
	User->Magic = User->MaxMagic = Mag;
	User->Money = Mony;
}

void ChooseClass(void)
{
	int		Num;
	Alfa	Word;

	User->Entry = XStats;
	GetWord(Word, &Num);
	CapAlfa(Word);
	if (!strcmp(Word, "STOP"))
	{
		Logoff(User);
		QOut(Term, "2");
		QDea(Term);
		User = NULL;
		return;
	}

	switch (*Word)
	{
		case 'F': SetStats(Fighter,	  8, 14, 2, 200); break;
		case 'T': SetStats(Thief,	  7, 10, 3, 130); break;
		case 'M': SetStats(MagicUser, 6,  9, 6, 150); break;
		case 'C': SetStats(Cleric,	  7, 11, 4,  80); break;
		case 'R': SetStats(Ranger,	  7, 11, 3, 150); break;
		case 'P': SetStats(Paladin,	 11,  8, 3, 140); break;
		default:
			User->Entry = XNewClass;
			Loc = LenBuf + 1;
			QOut(Term, "0No such class.  Try again.");
			return;
	}

	if (Loc >= LenBuf)
	{
		QOut(Term,
			"0Choose your character's attributes for strength, intelligence,");
		QOut(Term, "0  dexterity, piety and constitution.");
		QOut(Term,
			"0(Example:  15, 7, 9, 10, 9)  Each must be between 5 and 18, and");
		QOut(Term, "0the total average must be 11 or less.");
	}
}

void MatchPW(void)
{
	const char	*Host = QHost(Term);
	char		PassWord[MaxPWLen + 1];
	int			Wait, OK;

	if ((Wait = LoginWait(Host, User->Name)))
	{
		Logoff(User);
		TooManyPW(Wait);
		QOut(Term, "2");
		QDea(Term);
		User = NULL;
		return;
	}

	GetPW(PassWord);
	OK = CheckPW(User->PWHash, PassWord);

	if (!OK)
	{
		LoginFailed(Host, User->Name);
		Logoff(User);
		QOut(Term, "0Wrong password, sorry.");
		QOut(Term, "2");
		QDea(Term);
		User = NULL;
	}
	else
	{
		LoginOK(Host, User->Name);
		PlacePlayer(User, User->RmCode);
		AnnounceLogin();
		User->Entry = XCmd;
		Loc = LenBuf + 1;
	}
}

void GetName(void)
{
	UserPoint	TempNext, Other;
	UserType	DumUser;
	int			Dummy;
	Alfa		Nam, CapName;

	GetWord(Nam, &Dummy);
	strcpy(CapName, Nam);
	CapAlfa(CapName);

	if (!strcmp(CapName, "STOP"))
	{
		Logoff(User);
		QOut(Term, "2");
		QDea(Term);
		User = NULL;
		return;
	}

	if (Dummy || !*Nam)
	{
		QOut(Term, "0Bad characters in name.");
		Loc = LenBuf + 1;
		return;
	}

	for (Other = UserTail; Other; Other = Other->NextUser)
	{
		Alfa	Temp;

		strcpy(Temp, Other->Name);
		CapAlfa(Temp);
		if (!strcmp(Temp, CapName))
		{
			Logoff(User);
			QOut(Term, "0Name already in use!");
			QOut(Term, "2");
			QDea(Term);
			User = NULL;
			return;
		}
	}

	TempNext = User->NextUser;
	ReadPlayer(&DumUser, Nam);
	*User =	DumUser;

	User->Status	= SLogin;
	User->Trm		= Term;
	User->XPid		= Pid;
	User->NextUser	= TempNext;

	if (!User->SSJ
		&& (TaskClosed == 'C' || TaskClosed == 'T' && !User->PlayTester))
	{
		QOut(Term,
			"0As you approach the gates, a little gnome jumps out from behind");
		QOut(Term,
			"0a rock.  He whines, 'Go back!  The masters are not ready for");
		QOut(Term, "0you yet!'  The gnome then disappears into the brush.");
		Logoff(User);
		QOut(Term, "2");
		QDea(Term);
		User = NULL;
	}
	else if (!*User->Name)
	{
		strcpy(User->Name, Nam);
		User->Entry = XNewClass;
		if (Loc >= LenBuf)
		{
			QOut(Term, "0Please select your character's class:");
			QOut(Term,
				"0Fighter, Thief, Cleric, Paladin, Ranger, or Magic-User.");
		}
	}
	else if (IsGuest(User->Trm))
	{
		QOut(Term, "0Guests can't play saved characters, sorry.");
		Logoff(User);
		QOut(Term, "2");
		QDea(Term);
		User = NULL;
	}
	else
		User->Entry = XPassword;
}
