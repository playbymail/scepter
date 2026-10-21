/*
 * dbtest -- run one function of the game database module (gamedb.c), for
 * tools/datatest.py.
 *
 *	dbtest create DIR
 *	dbtest open DIR
 *	dbtest version DIR
 *	dbtest backup DIR TO
 *	dbtest restore FROM DIR
 *	dbtest migrate [-n] DIR
 *
 * Exits 0 if the function succeeds and 1 if it fails; the module reports
 * why on stderr.  "version" prints the schema version.
 */
#include <stdio.h>
#include <string.h>

#include "gamedb.h"

int main(int argc, char **argv)
{
	int		Ok = 0, Version;

	if (argc == 3 && !strcmp(argv[1], "create"))
		Ok = DBCreate(argv[2]);
	else
	if (argc == 3 && !strcmp(argv[1], "open"))
	{
		Ok = DBOpen(argv[2]);
		DBClose();
	}
	else
	if (argc == 3 && !strcmp(argv[1], "version"))
	{
		if ((Ok = DBVersionOf(argv[2], &Version)))
			printf("%d\n", Version);
	}
	else
	if (argc == 4 && !strcmp(argv[1], "backup"))
		Ok = DBBackup(argv[2], argv[3]);
	else
	if (argc == 4 && !strcmp(argv[1], "restore"))
		Ok = DBRestore(argv[2], argv[3]);
	else
	if (argc == 3 && !strcmp(argv[1], "migrate"))
		Ok = DBMigrate(argv[2], 0);
	else
	if (argc == 4 && !strcmp(argv[1], "migrate") && !strcmp(argv[2], "-n"))
		Ok = DBMigrate(argv[3], 1);
	else
	{
		fprintf(stderr, "usage: dbtest create|open|version DIR | backup DIR TO"
				" | restore FROM DIR | migrate [-n] DIR\n");
		return 2;
	}
	return Ok ? 0 : 1;
}
