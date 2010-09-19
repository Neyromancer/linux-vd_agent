/*  vdagent-x11.h vdagent x11 code header file

    Copyright 2010 Red Hat, Inc.

    Red Hat Authors:
    Hans de Goede <hdegoede@redhat.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or   
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of 
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the  
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef __VDAGENT_H
#define __VDAGENT_H

#include "udscs.h"

struct vdagent_x11;

struct vdagent_x11 *vdagent_x11_create(struct udscs_connection *vdagentd,
    int verbose);
void vdagent_x11_destroy(struct vdagent_x11 *x11);

int  vdagent_x11_get_fd(struct vdagent_x11 *x11);
void vdagent_x11_do_read(struct vdagent_x11 *x11);

#endif