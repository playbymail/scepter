/*
 * SHA-256 (FIPS 180-4), for the revision of a world file (#138).
 */
#ifndef	SHA256_H
#define	SHA256_H

#include <stddef.h>
#include <stdint.h>

#define	SHA256Hex	65			/* 64 hex digits and a NUL */

typedef struct
{
	uint32_t	H[8];
	uint64_t	Bytes;			/* hashed so far */
	uint8_t		Block[64];
	size_t		Used;			/* bytes waiting in Block */
} SHA256Type;

void	SHA256Init(SHA256Type *S);
void	SHA256Add(SHA256Type *S, const void *Data, size_t Len);
void	SHA256Hex64(SHA256Type *S, char Hex[SHA256Hex]);

#endif
