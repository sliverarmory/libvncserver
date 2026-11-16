#ifndef D3DES_H
#define D3DES_H

/*
 * This is D3DES (V5.09) by Richard Outerbridge with the double and
 * triple-length support removed for use in VNC.
 *
 * These changes are:
 *  Copyright (C) 1999 AT&T Laboratories Cambridge.  All Rights Reserved.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 */

/* d3des.h -
 *
 *	Headers and defines for d3des.c
 *	Graven Imagery, 1992.
 *
 * Copyright (c) 1988,1989,1990,1991,1992 by Richard Outerbridge
 *	(GEnie : OUTER; CIS : [71755,204])
 */

#define EN0	0	/* MODE == encrypt */
#define DE1	1	/* MODE == decrypt */

extern void rfbDesKey(unsigned char *, int, unsigned long [32]);
/*		      hexkey[8]     MODE     schedule[32]
 * Writes the DES key schedule for encryption or decryption according to MODE.
 */


extern void rfbDes(unsigned char *, unsigned char *, unsigned long [32]);
/*		    from[8]	      to[8]      schedule[32]
 * Encrypts/Decrypts one block with the supplied key schedule. 'from' and
 * 'to' can be the same address.
 */

/* d3des.h V5.09 rwo 9208.04 15:06 Graven Imagery
 ********************************************************************/

#endif
