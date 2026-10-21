/*
 * upgradegame -- bring a game's database up to this build's schema.
 *
 *	upgradegame [-n] GAMEDIR
 *
 *	-n	only report what would change
 *
 * scepterd opens only a database at its own schema version, PRAGMA
 * user_version (gamedb.c, docs/data-files.md).  Stop the server first.
 * upgradegame backs GAMEDIR/game.db up to GAMEDIR/game.db.backup-YYYYMMDD-
 * HHMMSS, applies every migration it is missing in one transaction, and
 * checks the result.  If anything fails, the database is left as it was,
 * beside its backup.  It never creates a directory.
 */
#include <stdio.h>
#include <unistd.h>

#include "gamedb.h"

static void Error(const char *Msg)
{
	fprintf(stderr, "upgradegame: %s\n", Msg);
}

int main(int argc, char **argv)
{
	int		Opt, DryRun = 0;

	while ((Opt = getopt(argc, argv, "n")) != -1)
		switch (Opt)
		{
			case 'n':	DryRun = 1;		break;
			default:
				fprintf(stderr, "usage: upgradegame [-n] GAMEDIR\n");
				return 2;
		}
	if (argc - optind != 1)
	{
		fprintf(stderr, "usage: upgradegame [-n] GAMEDIR\n");
		return 2;
	}
	DBReport = Error;
	if (!DBMigrate(argv[optind], DryRun))
	{
		fprintf(stderr, "upgradegame: %s was not upgraded\n", argv[optind]);
		return 1;
	}
	return 0;
}
