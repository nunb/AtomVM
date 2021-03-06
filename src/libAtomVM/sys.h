/***************************************************************************
 *   Copyright 2017 by Davide Bettio <davide@uninstall.it>                 *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU Lesser General Public License as        *
 *   published by the Free Software Foundation; either version 2 of the    *
 *   License, or (at your option) any later version.                       *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA .        *
 ***************************************************************************/

/**
 * @file sys.h
 * @brief Platform specific functions.
 *
 * @details This header defines platform dependent functions, that mostly deals with events.
 */

#ifndef _SYS_H_
#define _SYS_H_

#include "globalcontext.h"
#include "linkedlist.h"
#include "module.h"

#include <stdint.h>
#include <time.h>

typedef void (*event_handler_t)(void *data);
typedef void (*waiting_hook_t)(void *data);

typedef struct EventListener {
    struct ListHead listeners_list_head;

    int expires;
    struct timespec expiral_timestamp;

    waiting_hook_t waiting_hook;

    event_handler_t handler;
    void *data;
    int fd;
} EventListener;

/**
 * @brief waits platform events
 *
 * @details wait any of the specified events using a platform specific implementation, that might be poll on unix-like systems.
 * @param listeners_list a list of listeners that are waiting for some events, each listener waits an event and has a callback.
 */
void sys_waitevents(struct ListHead *listeners_list);

/**
 * @brief sets the timestamp for a future event
 *
 * @details sets the timestamp to a timestamp n milliseconds in the future using platform monotonic timer source.
 * @param t the timespec that will be updated.
 * @param millis ammount of milliseconds relative to current timestamp.
 */
void sys_set_timestamp_from_relative_to_abs(struct timespec *t, int32_t millis);

/**
 * @brief Loads a BEAM module using platform dependent methods.
 *
 * @details Loads a BEAM module into memory using platform dependent methods and returns a pointer to a Module struct.
 * @param global the global context.
 * @param module_name the name of the BEAM file (e.g. "mymodule.beam").
 */
Module *sys_load_module(GlobalContext *global, const char *module_name);

#endif
