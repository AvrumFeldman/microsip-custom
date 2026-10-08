// Modified 2026-10-08 for MicroSIP Custom: custom build version and voice-only feature selection.
/*
/*
 * Copyright (C) 2011-2026 MicroSIP (http://www.microsip.org)
 *
 * This program is free software; you can redistribute it and/or modify
 * it unbodr the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA 
 */

#pragma once
#ifdef MICROSIP_AUDIO_ONLY
#define _GLOBAL_VERSION "3.22.16.3"
#define _GLOBAL_VERSION_COMMA 3,22,16,3
#else
#define _GLOBAL_VERSION "3.22.16"
#define _GLOBAL_VERSION_COMMA 3,22,16,0
#endif
#define _GLOBAL_TIMESTAMP 1789417397
#define _GLOBAL_KEY "*********"
#ifndef MICROSIP_AUDIO_ONLY
#define _GLOBAL_VIDEO
#endif
#define _GLOBAL_NAME "MicroSIP"
#define _GLOBAL_COMPANY "www.microsip.org"
