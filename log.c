#include "scepter.h"

static void	AnnounceLogin(void);
static void	SetStats(ChType Cl, int Vit, int Fat, int Mag, int Mony);
static int	FindHost(const char *Host);
static int	FindName(char *Name);

/*
 * Limits on guessing passwords (#82), kept in memory only.  An address with
 * HostFails wrong passwords within HostWindow seconds is refused for
 * HostBlock seconds, doubling each time up to HostBlockMax; the doubling is
 * forgotten after a day without a refusal.  A character with NameFails wrong
 * passwords from anywhere, the last within NameWindow seconds, may be tried
 * only once every NameDelay seconds: slow enough to stop guessing, without
 * letting anyone lock the owner out.
 */
#define	HostFails		5
#define	HostWindow		600
#define	HostBlock		600
#define	HostBlockMax	7200
#define	NameFails		10
#define	NameWindow		3600
#define	NameDelay		30
#define	FailSlots		64

static struct
{
	char	Host[64];
	int		Fails;			/* since Start */
	int		Blocks;			/* refusals, for the doubling */
	long	Start, Until;
} FailHost[FailSlots];

static struct
{
	Alfa	Name;
	int		Fails;
	long	Last;
} FailName[FailSlots];

/* The slot for Host: its own, else the one least recently used */
static int FindHost(const char *Host)
{
	int		ILoop, Old = 0;

	for (ILoop = 0; ILoop < FailSlots; ILoop++)
	{
		if (!strcmp(FailHost[ILoop].Host, Host))
			return ILoop;
		if (MaxL(FailHost[ILoop].Start, FailHost[ILoop].Until)
			< MaxL(FailHost[Old].Start, FailHost[Old].Until))
			Old = ILoop;
	}
	memset(&FailHost[Old], 0, sizeof(FailHost[Old]));
	snprintf(FailHost[Old].Host, sizeof(FailHost[Old].Host), "%s", Host);
	return Old;
}

/* The slot for Name: its own, else the one least recently used */
static int FindName(char *Name)
{
	int		ILoop, Old = 0;

	for (ILoop = 0; ILoop < FailSlots; ILoop++)
	{
		if (!strcmp(FailName[ILoop].Name, Name))
			return ILoop;
		if (FailName[ILoop].Last < FailName[Old].Last)
			Old = ILoop;
	}
	memset(&FailName[Old], 0, sizeof(FailName[Old]));
	strcpy(FailName[Old].Name, Name);
	return Old;
}

/* Seconds before Name may try a password from Host; 0 if it may now */
int LoginWait(const char *Host, char *Name)
{
	int		H = FindHost(Host), N = FindName(Name);

	if (FailHost[H].Until > RealTime)
		return FailHost[H].Until - RealTime;
	if (FailName[N].Fails >= NameFails && RealTime - FailName[N].Last < NameDelay)
		return FailName[N].Last + NameDelay - RealTime;
	return 0;
}

void LoginFailed(const char *Host, char *Name)
{
	int		H = FindHost(Host), N = FindName(Name);

	if (RealTime - FailHost[H].Start >= HostWindow)
	{
		FailHost[H].Fails = 0;
		FailHost[H].Start = RealTime;
	}
	if (RealTime - FailHost[H].Until >= 86400)
		FailHost[H].Blocks = 0;
	if (RealTime - FailName[N].Last >= NameWindow)
		FailName[N].Fails = 0;
	FailName[N].Fails++;
	FailName[N].Last = RealTime;

	snprintf(B1, sizeof(B1), " Wrong password for %s from %s.", Name, Host);
	if (++FailHost[H].Fails >= HostFails)
	{
		long	Block = MinL(HostBlockMax, (long)HostBlock << Min(FailHost[H].Blocks, 4));

		FailHost[H].Until = RealTime + Block;
		FailHost[H].Blocks++;
		FailHost[H].Fails = 0;
		snprintf(B1, sizeof(B1), " Wrong password for %s from %s;"
				 " address refused for %ld minutes.", Name, Host, Block / 60);
	}
	ErrLog(B1);
}

void LoginOK(const char *Host, char *Name)
{
	FailHost[FindHost(Host)].Fails = 0;
	FailName[FindName(Name)].Fails = 0;
}

/* Tell the player how long LoginWait says to wait */
void TooManyPW(int Wait)
{
	if (Wait < 120)
		sprintf(B1, "0Too many wrong passwords.  Try again in %d seconds.", Wait);
	else
		sprintf(B1, "0Too many wrong passwords.  Try again in %d minutes.",
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
				sprintf(B1, "0Daily %d guild fee paid in full.  Thank you!",
						User->Lvl * 20);
				QOut(Term, B1);
			}
		}
	}

	CmdCode = 0;
	sprintf(B1, "0### Please welcome %s, the %s.",
			User->Name, CName[(int) User->Class]);
	AllMsg(B1);

	if (!User->SSJ && (User->Invisible || User->NonExistant))
		User->NonExistant = User->Invisible = FALSE;
	if (User->Invisible)
	{
		sprintf(B1, "0You are currently %s.",
				User->NonExistant ? "nonexistant" : "invisible");
		QOut(Term, B1);
	}

	RoomDisplay(User->RmCode, User->Brief);
}

UserPoint Login(void)
{
	UserPoint	NewUser;

	NUsers++;
	NewUser = malloc(sizeof(UserType));
	*NewUser = ProtoUser;
	sprintf(B1, "0Scepter Fantasy Role-Playing Game  Ver 2.4.3  (Run %d)",
			NumRun);
	QOut(Term, B1);

	if (*Notice)
	{
		sprintf(B1, "0Notice:  %s", Notice);
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
		sprintf(B1, "0Passwords must be %d to %d characters long.", MinPWLen,
				MaxPWLen);
		QOut(Term, B1);
	}
	else if (SetPW(User->PWHash, PassWord))
	{
		User->PW = 0;
		User->Entry = XSex;
	}
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
		sprintf(B1, "0Choose a password of %d to %d characters.  Spaces are allowed,",
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
	int			Dummy, Len, Start = Loc, Wait, OK;
	Alfa		Word;

	if ((Wait = LoginWait(Host, User->Name)))
	{
		Logoff(User);
		TooManyPW(Wait);
		QOut(Term, "2");
		QDea(Term);
		User = NULL;
		return;
	}

	/* a character not on since version 2 has only the old hash of one word */
	GetWord(Word, &Dummy);
	CapAlfa(Word);
	Loc = Start;
	Len = GetPW(PassWord);
	if (*User->PWHash)
		OK = CheckPW(User->PWHash, PassWord);
	else
		OK = Len > 0 && *Word && User->PW && Hash(Word) == User->PW;

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
		if (!*User->PWHash && SetPW(User->PWHash, PassWord))
		{
			User->PW = 0;
			QOut(Term, "0Passwords now tell capital letters from small ones.  Yours is");
			QOut(Term, "0now exactly what you typed just now.");
			if (Len < MinPWLen)
			{
				sprintf(B1, "0Please use CHANGEPW to choose one of at least %d characters.",
						MinPWLen);
				QOut(Term, B1);
			}
		}
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
