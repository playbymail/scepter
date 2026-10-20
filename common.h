/* Paths may be overridden at build time, e.g. -DWORKDIR='"/var/scepter"'. */
#ifdef ERUG
#define	BINDIR	"/home/wm/erug"
#define	TMPDIR	"/home/wm/erug"
#define	WORKDIR	"/home/wm/erug"
#define	SERVER	"erugd"
#else
#ifndef	BINDIR
#define	BINDIR	"."
#endif
#ifndef	TMPDIR
#define	TMPDIR	"/tmp"
#endif
#ifndef	WORKDIR
#define	WORKDIR	"."
#endif
#ifndef	SERVER
#define	SERVER	"scepterd"
#endif
#endif
