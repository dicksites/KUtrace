// from_base40.h 
//
// This decodes base40 
//

#ifndef __FROM_BASE40_H__
#define __FROM_BASE40_H__

// Unpack six characters from 32 bits.
// str must be 8 bytes. We somewhat-arbitrarily capitalize the first letter
char* Base40ToChar(uint64 base40, char* str);

#endif	// __FROM_BASE40_H__

