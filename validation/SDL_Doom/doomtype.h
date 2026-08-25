//
// Copyright(C) 1993-1996 Id Software, Inc.
// Copyright(C) 2005-2014 Simon Howard
// Copyright(C) 2024 Dreamcast Port
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//

#ifndef __DOOMTYPE__
#define __DOOMTYPE__

#include <inttypes.h>
#include <strings.h>
#include <limits.h>

// Pack attribute for Dreamcast/GCC
#define PACKEDATTR __attribute__((packed))

// Simple boolean type
typedef enum {
    BOOLEAN_FALSE = 0,
    BOOLEAN_TRUE = 1,
    BOOLEAN_UNDEF = 0xFFFFFFFF
} boolean;

// Define for compatibility
#define false BOOLEAN_FALSE
#define true BOOLEAN_TRUE
#define undef BOOLEAN_UNDEF

// Basic types
typedef uint8_t byte;

// Path settings for Dreamcast
#define DIR_SEPARATOR '/'
#define DIR_SEPARATOR_S "/"
#define PATH_SEPARATOR ':'

// Helper macro
#define arrlen(array) (sizeof(array) / sizeof(*array))

#endif