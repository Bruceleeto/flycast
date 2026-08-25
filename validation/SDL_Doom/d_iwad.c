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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kos.h>

#include "config.h"
#include "deh_str.h"
#include "doomkeys.h"
#include "d_iwad.h"
#include "i_system.h"
#include "m_argv.h"
#include "m_config.h"
#include "m_misc.h"
#include "w_wad.h"
#include "z_zone.h"

// Simplified IWAD list for Dreamcast
static const iwad_t iwads[] = {
    { "doom.wad",     doom,      retail,     "Doom" },
    { "doom1.wad",    doom,      shareware,  "Doom Shareware" }
};

// Check if a file exists in the romdisk
static boolean RD_FileExists(char *filename) {
    file_t f;
    char fullpath[32];
    
    snprintf(fullpath, sizeof(fullpath), "/rd/%s", filename);
    f = fs_open(fullpath, O_RDONLY);
    
    if(f) {
        fs_close(f);
        return true;
    }
    return false;
}

// Find WAD in romdisk directory
char *D_FindWADByName(char *name) {
    char *path;
    char fullpath[32];
    
    // Check if file exists in romdisk
    snprintf(fullpath, sizeof(fullpath), "/rd/%s", name);
    if (RD_FileExists(name)) {
        path = strdup(fullpath);
        return path;
    }
    
    return NULL;
}

// Main IWAD finding function
char *D_FindIWAD(int mask, GameMission_t *mission) {
    char *result;
    int i;

    // First check for doom.wad in romdisk
    result = D_FindWADByName("doom.wad");
    if (result != NULL) {
        *mission = doom;
        return result;
    }

    // If not found, try doom1.wad
    result = D_FindWADByName("doom1.wad");
    if (result != NULL) {
        *mission = doom;
        return result;
    }

    // No IWAD found
    I_Error("Game WAD not found in romdisk (/rd)!");
    return NULL;
}

// For savegame compatibility
char *D_SaveGameIWADName(GameMission_t gamemission) {
    return "doom.wad";
}

// Suggest IWAD name based on mission/mode
char *D_SuggestIWADName(GameMission_t mission, GameMode_t mode) {
    return "doom.wad";
}

// Get game description
char *D_SuggestGameName(GameMission_t mission, GameMode_t mode) {
    return "Doom";
}

char *D_TryFindWADByName(char *filename)
{
    char *result;

    result = D_FindWADByName(filename);

    if (result != NULL)
    {
        return result;
    }
    else
    {
        // If not found, just return the original filename
        return strdup(filename);
    }
}


// Find all IWADs (simplified for DC)
const iwad_t **D_FindAllIWADs(int mask) {
    const iwad_t **result;
    result = malloc(sizeof(iwad_t *) * 2); // Just doom.wad and NULL terminator
    
    if (RD_FileExists("doom.wad")) {
        result[0] = &iwads[0];
        result[1] = NULL;
    } else {
        result[0] = NULL;
    }
    
    return result;
}