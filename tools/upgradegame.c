/*
 * upgradegame -- bring a game directory's data files to this build's format.
 *
 *	upgradegame [-n] GAMEDIR
 *
 *	-n	only report what would change
 *
 * scepterd refuses any data file whose format version is not its own
 * (datafile.c, docs/data-files.md).  This converts an older game, one
 * version at a time, to the version this build reads:
 *
 *	0 -> 1	files from before headers existed get a header; the records
 *			are unchanged
 *
 * Stop the server first.  Nothing is changed unless every file can be
 * upgraded.  The whole game directory is first copied to
 * GAMEDIR.backup-YYYYMMDD-HHMMSS.  Each file is then rewritten to a
 * temporary file, flushed to disk and renamed over the original, keeping
 * its owner and mode, so a file is always either old or new.  If the
 * upgrade stops part way, run it again: files already upgraded are left
 * alone.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "common.h"
#include "scepter.h"

static const char	*Game;
static int			DryRun;

static void Fail(const char *Fmt, const char *Arg)
{
	fprintf(stderr, "upgradegame: ");
	fprintf(stderr, Fmt, Arg);
	fprintf(stderr, "\n");
	exit(1);
}

/* Copy From to To (a new file), keeping the mode and, if allowed, the owner. */
static void CopyFile(const char *From, const char *To, const struct stat *st)
{
	char	Buf[65536];
	ssize_t	n;
	int		in, out;

	if ((in = open(From, O_RDONLY)) < 0)
		Fail("%s: cannot read", From);
	if ((out = open(To, O_WRONLY | O_CREAT | O_EXCL, st->st_mode & 07777)) < 0)
		Fail("%s: cannot create", To);
	while ((n = read(in, Buf, sizeof Buf)) > 0)
		if (write(out, Buf, n) != n)
			Fail("%s: write failed", To);
	if (n < 0 || fsync(out) || close(out))
		Fail("%s: copy failed", To);
	close(in);
	if (chown(To, st->st_uid, st->st_gid))
		;	/* not root: the copy is ours, which is fine */
}

/* Copy GAMEDIR to GAMEDIR.backup-TIME, one directory level deep (dat/, ram/). */
static void Backup(char *Dest, size_t Len)
{
	time_t		t = time(NULL);
	char		Stamp[32];
	const char	*Sub[] = { "", "/dat", "/ram" };
	int			i;

	strftime(Stamp, sizeof Stamp, "%Y%m%d-%H%M%S", localtime(&t));
	snprintf(Dest, Len, "%s.backup-%s", Game, Stamp);

	for (i = 0; i < 3; i++)
	{
		char			From[1100], To[1100];
		struct stat		st;
		DIR				*d;
		struct dirent	*e;

		snprintf(From, sizeof From, "%s%s", Game, Sub[i]);
		snprintf(To, sizeof To, "%s%s", Dest, Sub[i]);
		if (stat(From, &st))
			continue;
		if (mkdir(To, st.st_mode & 07777))
			Fail("%s: cannot create the backup directory", To);
		if (chown(To, st.st_uid, st.st_gid))
			;
		if (!(d = opendir(From)))
			Fail("%s: cannot list", From);
		while ((e = readdir(d)))
		{
			char	F[1400], T[1400];

			snprintf(F, sizeof F, "%s/%s", From, e->d_name);
			snprintf(T, sizeof T, "%s/%s", To, e->d_name);
			if (!stat(F, &st) && S_ISREG(st.st_mode))
				CopyFile(F, T, &st);
		}
		closedir(d);
	}
}

/* Version 0 -> 1: put a header in front of the unchanged records. */
static void Upgrade0(const char *Path, int Kind)
{
	char		Tmp[1200], Buf[65536];
	struct stat	st;
	FILE		*in, *out;
	size_t		n;
	int			fd;

	snprintf(Tmp, sizeof Tmp, "%s.upgrade", Path);
	if (stat(Path, &st))
		Fail("%s: cannot read", Path);
	if (!(in = fopen(Path, "rb")))
		Fail("%s: cannot read", Path);
	unlink(Tmp);
	if ((fd = open(Tmp, O_WRONLY | O_CREAT | O_EXCL, st.st_mode & 07777)) < 0)
		Fail("%s: cannot create", Tmp);
	if (!(out = fdopen(fd, "wb")))
		Fail("%s: cannot create", Tmp);
	if (!WriteDataHead(out, Kind))
		Fail("%s: write failed", Tmp);
	while ((n = fread(Buf, 1, sizeof Buf, in)) > 0)
		if (fwrite(Buf, 1, n, out) != n)
			Fail("%s: write failed", Tmp);
	if (ferror(in) || fflush(out) || fsync(fd) || fclose(out))
		Fail("%s: write failed", Tmp);
	fclose(in);
	if (chown(Tmp, st.st_uid, st.st_gid))
		;
	if (rename(Tmp, Path))
		Fail("%s: cannot replace", Path);
}

int main(int argc, char **argv)
{
	int				Result[NDataFiles];
	unsigned long	Version[NDataFiles], RecSize;
	int				i, Opt, Bad = 0, Todo = 0;
	char			Path[1100], Msg[256], Dest[1100];

	while ((Opt = getopt(argc, argv, "n")) != -1)
		switch (Opt)
		{
			case 'n':	DryRun = TRUE;	break;
			default:	Fail("usage: upgradegame [-n] GAMEDIR%s", "");
		}
	if (argc - optind != 1)
		Fail("usage: upgradegame [-n] GAMEDIR%s", "");
	Game = argv[optind];

	/* look at every file before changing any */
	for (i = 0; i < NDataFiles; i++)
	{
		FILE	*fp;
		int		Kind = DataFile[i].Kind;

		snprintf(Path, sizeof Path, "%s/%s", Game, DataFile[i].Name);
		if (!(fp = fopen(Path, "rb")))
		{
			fprintf(stderr, "%s: %s\n", Path, strerror(errno));
			Bad++;
			continue;
		}
		Result[i] = ReadDataHead(fp, Kind, &Version[i], &RecSize);
		fclose(fp);

		if (Result[i] == DataOK)
			printf("%s: version %d, up to date\n", DataFile[i].Name, DataVersion);
		else if (Result[i] == DataNoHead)
		{
			printf("%s: version 0, %s to version %d\n", DataFile[i].Name,
				   DryRun ? "would upgrade" : "upgrading", DataVersion);
			Todo++;
		}
		else
		{
			/* a newer version, a foreign file, or a different ABI */
			DataProblem(Result[i], Kind, Version[i], RecSize, Msg, sizeof Msg);
			if (Result[i] == DataWrongVersion && Version[i] < DataVersion)
				snprintf(Msg, sizeof Msg, "version %lu, which this upgradegame"
						 " cannot convert", Version[i]);
			fprintf(stderr, "%s: %s\n", Path, Msg);
			Bad++;
		}
	}

	if (Bad)
		Fail("nothing changed: %s cannot be upgraded", Game);
	if (!Todo)
	{
		printf("%s is already at format version %d\n", Game, DataVersion);
		return 0;
	}
	if (DryRun)
		return 0;

	Backup(Dest, sizeof Dest);
	printf("backed up %s to %s\n", Game, Dest);
	for (i = 0; i < NDataFiles; i++)
		if (Result[i] == DataNoHead)
		{
			snprintf(Path, sizeof Path, "%s/%s", Game, DataFile[i].Name);
			Upgrade0(Path, DataFile[i].Kind);
		}
	if (CheckGame(Game))
		Fail("%s: upgraded files fail the check; restore the backup", Game);
	printf("%s is now at format version %d\n", Game, DataVersion);
	return 0;
}
