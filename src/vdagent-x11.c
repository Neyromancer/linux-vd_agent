//#include "/var/tmp/sensor.h"
/*  vdagent-x11.c vdagent x11 code

    Copyright 2010-2011 Red Hat, Inc.

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

/* Note: Our event loop is only called when there is data to be read from the
   X11 socket. If events have arrived and have already been read by libX11 from
   the socket triggered by other libX11 calls from this file, the select for
   read in the main loop, won't see these and our event loop won't get called!
   
   Thus we must make sure that all queued events have been consumed, whenever
   we return to the main loop. IOW all (externally callable) functions in this
   file must end with calling XPending and consuming all queued events.
   
   Calling XPending when-ever we return to the mainloop also ensures any
   pending writes are flushed. */

#include <glib.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <syslog.h>
#include <assert.h>
#include <unistd.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xfixes.h>
#include "vdagentd-proto.h"
#include "vdagent-x11.h"
#include "vdagent-x11-priv.h"

/* Stupid X11 API, there goes our encapsulate all data in a struct design */
int (*vdagent_x11_prev_error_handler)(Display *, XErrorEvent *);
int vdagent_x11_caught_error;

static void vdagent_x11_handle_selection_notify(struct vdagent_x11 *x11,
                                                XEvent *event, int incr);
static void vdagent_x11_handle_selection_request(struct vdagent_x11 *x11);
static void vdagent_x11_handle_targets_notify(struct vdagent_x11 *x11,
                                              XEvent *event);
static void vdagent_x11_handle_property_delete_notify(struct vdagent_x11 *x11,
                                                      XEvent *del_event);
static void vdagent_x11_send_selection_notify(struct vdagent_x11 *x11,
                Atom prop, struct vdagent_x11_selection_request *request);
static void vdagent_x11_set_clipboard_owner(struct vdagent_x11 *x11,
                                            uint8_t selection, int new_owner);

static const char *vdagent_x11_sel_to_str(uint8_t selection) {
    switch (selection) {
    case VD_AGENT_CLIPBOARD_SELECTION_CLIPBOARD:
        return "clipboard";
    case VD_AGENT_CLIPBOARD_SELECTION_PRIMARY:
        return "primary";
    case VD_AGENT_CLIPBOARD_SELECTION_SECONDARY:
        return "secondary";
    default:
        return "unknown";
    }
}

static int vdagent_x11_debug_error_handler(
    Display *display, XErrorEvent *error)
{
    abort();
}

/* With the clipboard we're sometimes dealing with Properties on another apps
   Window. which can go away at any time. */
static int vdagent_x11_ignore_bad_window_handler(
    Display *display, XErrorEvent *error)
{

// ============= added for debugging | KORMULEV =================
	syslog( LOG_INFO, "vdagent_x11_ignore_bad_window_handler | vdagent-x11.c | line 91" );
// ==============================================================
    if (error->error_code == BadWindow)
        return 0;

    return vdagent_x11_prev_error_handler(display, error);
}

void vdagent_x11_set_error_handler(struct vdagent_x11 *x11,
    int (*handler)(Display *, XErrorEvent *))
{
    XSync(x11->display, False);
    vdagent_x11_caught_error = 0;
    vdagent_x11_prev_error_handler = XSetErrorHandler(handler);
}

int vdagent_x11_restore_error_handler(struct vdagent_x11 *x11)
{
    int error;

    XSync(x11->display, False);
    XSetErrorHandler(vdagent_x11_prev_error_handler);
    error = vdagent_x11_caught_error;
    vdagent_x11_caught_error = 0;

    return error;
}

static void vdagent_x11_get_wm_name(struct vdagent_x11 *x11)
{
    Atom type_ret;
    int format_ret;
    unsigned long len, remain;
    unsigned char *data = NULL;
    Window sup_window = None;

    /* XGetWindowProperty can throw a BadWindow error. One way we can trigger
       this is when the display-manager (ie gdm) has set, and not cleared the
       _NET_SUPPORTING_WM_CHECK property, and the window manager running in
       the user session has not yet updated it to point to its window, so its
       pointing to a non existing window. */
// ============= added for debugging | KORMULEV =================
	syslog( LOG_INFO, "vdagent_x11_get_wm_name | vdagent-x11.c | line 134" );
	syslog( LOG_INFO, "x11->xfixes_event_base %d\n", x11->xfixes_event_base );
	syslog( LOG_INFO, "Before vdagent_x11_set_error_handler" );
// ==============================================================
    vdagent_x11_set_error_handler(x11, vdagent_x11_ignore_bad_window_handler);

    /* Get the window manager SUPPORTING_WM_CHECK window */
    if (XGetWindowProperty(x11->display, x11->root_window[0],
            XInternAtom(x11->display, "_NET_SUPPORTING_WM_CHECK", False), 0,
            LONG_MAX, False, XA_WINDOW, &type_ret, &format_ret, &len,
            &remain, &data) == Success) {
        if (type_ret == XA_WINDOW)
            sup_window = *((Window *)data);
        XFree(data);
    }
    if (sup_window == None &&
        XGetWindowProperty(x11->display, x11->root_window[0],
            XInternAtom(x11->display, "_WIN_SUPPORTING_WM_CHECK", False), 0,
            LONG_MAX, False, XA_CARDINAL, &type_ret, &format_ret, &len,
            &remain, &data) == Success) {
        if (type_ret == XA_CARDINAL)
            sup_window = *((Window *)data);
        XFree(data);
    }
    /* So that we can get the net_wm_name */
    if (sup_window != None) {
        Atom utf8 = XInternAtom(x11->display, "UTF8_STRING", False);
        if (XGetWindowProperty(x11->display, sup_window,
                XInternAtom(x11->display, "_NET_WM_NAME", False), 0,
                LONG_MAX, False, utf8, &type_ret, &format_ret, &len,
                &remain, &data) == Success) {
            if (type_ret == utf8) {
                x11->net_wm_name =
                    g_strndup((char *)data, (format_ret / 8) * len);
            }
            XFree(data);
        }
        if (x11->net_wm_name == NULL &&
            XGetWindowProperty(x11->display, sup_window,
                XInternAtom(x11->display, "_NET_WM_NAME", False), 0,
                LONG_MAX, False, XA_STRING, &type_ret, &format_ret, &len,
                &remain, &data) == Success) {
            if (type_ret == XA_STRING) {
                x11->net_wm_name =
                    g_strndup((char *)data, (format_ret / 8) * len);
            }
            XFree(data);
        }
    }

    vdagent_x11_restore_error_handler(x11);
}

struct vdagent_x11 *vdagent_x11_create(struct udscs_connection *vdagentd,
    int debug, int sync)
{
    struct vdagent_x11 *x11;
    XWindowAttributes attrib;
    int i, j, major, minor;

    x11 = calloc(1, sizeof(*x11));
    if (!x11) {
        syslog(LOG_ERR, "out of memory allocating vdagent_x11 struct");
        return NULL;
    }

    x11->vdagentd = vdagentd;
    x11->debug = debug;

    x11->display = XOpenDisplay(NULL);
    if (!x11->display) {
        syslog(LOG_ERR, "could not connect to X-server");
        free(x11);
        return NULL;
    }

    x11->screen_count = ScreenCount(x11->display);
    if (x11->screen_count > MAX_SCREENS) {
        syslog(LOG_ERR, "Error too much screens: %d > %d",
               x11->screen_count, MAX_SCREENS);
        XCloseDisplay(x11->display);
        free(x11);
        return NULL;
    }

    if (sync) {
        XSetErrorHandler(vdagent_x11_debug_error_handler);
        XSynchronize(x11->display, True);
    }

    for (i = 0; i < x11->screen_count; i++)
        x11->root_window[i] = RootWindow(x11->display, i);

    x11->fd = ConnectionNumber(x11->display);
    x11->clipboard_atom = XInternAtom(x11->display, "CLIPBOARD", False);
    x11->clipboard_primary_atom = XInternAtom(x11->display, "PRIMARY", False);
    x11->targets_atom = XInternAtom(x11->display, "TARGETS", False);
    x11->incr_atom = XInternAtom(x11->display, "INCR", False);
    x11->multiple_atom = XInternAtom(x11->display, "MULTIPLE", False);
    x11->timestamp_atom = XInternAtom(x11->display, "TIMESTAMP", False);
    for(i = 0; i < clipboard_format_count; i++) {
        x11->clipboard_formats[i].type = clipboard_format_templates[i].type;
        for(j = 0; clipboard_format_templates[i].atom_names[j]; j++) {
            x11->clipboard_formats[i].atoms[j] =
                XInternAtom(x11->display,
                            clipboard_format_templates[i].atom_names[j],
                            False);
        }
        x11->clipboard_formats[i].atom_count = j;
    }

    /* We should not store properties (for selections) on the root window */
    x11->selection_window = XCreateSimpleWindow(x11->display, x11->root_window[0],
                                                0, 0, 1, 1, 0, 0, 0);
    if (x11->debug)
        syslog(LOG_DEBUG, "Selection window: %u", (int)x11->selection_window);

    vdagent_x11_randr_init(x11);
// =============================== added for debugging | KORMULEV ==================
	syslog( LOG_INFO, "vdagent_x11_create | vdagent-x11.c | line 252" );
	syslog( LOG_INFO, "x11->xfixes_event_base is %d\n", x11->xfixes_event_base );
// =================================================================================
    if (XFixesQueryExtension(x11->display, &x11->xfixes_event_base, &i) &&
        XFixesQueryVersion(x11->display, &major, &minor) && major >= 1) {
        x11->has_xfixes = 1;
        XFixesSelectSelectionInput(x11->display, x11->root_window[0],
                                   x11->clipboard_atom,
                                   XFixesSetSelectionOwnerNotifyMask|
                                   XFixesSelectionWindowDestroyNotifyMask|
                                   XFixesSelectionClientCloseNotifyMask);
        XFixesSelectSelectionInput(x11->display, x11->root_window[0],
                                   x11->clipboard_primary_atom,
                                   XFixesSetSelectionOwnerNotifyMask|
                                   XFixesSelectionWindowDestroyNotifyMask|
                                   XFixesSelectionClientCloseNotifyMask);
    } else
        syslog(LOG_ERR, "no xfixes, no guest -> client copy paste support");
// =============================== added for debugging | KORMULEV ==================
	syslog( LOG_INFO, "vdagent_x11_create | vdagent-x11.c | line 324" );
	syslog( LOG_INFO, "Before XExtendedMaxRequestSize" );
// =================================================================================
    x11->max_prop_size = XExtendedMaxRequestSize(x11->display);
// =============================== added for debugging | KORMULEV ==================
	syslog( LOG_INFO, "vdagent_x11_create | vdagent-x11.c | line 329" );
	syslog( LOG_INFO, "After XExtendedMaxRequestSize" );
	syslog( LOG_INFO, "x11->max_prop_size is %d\n", x11->max_prop_size );
// =================================================================================
    if (x11->max_prop_size) {
        x11->max_prop_size -= 100;
    } else {
        x11->max_prop_size = XMaxRequestSize(x11->display) - 100;
    }
    /* Be a good X11 citizen and maximize the amount of data we send at once */
    if (x11->max_prop_size > 262144)
        x11->max_prop_size = 262144;
// =============================== added for debugging | KORMULEV ==================
	syslog( LOG_INFO, "vdagent_x11_create | vdagent-x11.c | line 342" );
	syslog( LOG_INFO, "<<<<< ================ Before for-loop where resolution changes cant be caught ======================== >>>>" );
// =================================================================================
    for (i = 0; i < x11->screen_count; i++) {
        /* Catch resolution changes */
        XSelectInput(x11->display, x11->root_window[i], StructureNotifyMask);
// ============= added for debugging | KORMULEV ==================
	syslog( LOG_INFO, "Catch resolution changes | vdagent-x11.c | line 336 | vdagent_x11_create" );
// ===============================================================
        /* Get the current resolution */
        XGetWindowAttributes(x11->display, x11->root_window[i], &attrib);
// =================== added for debugging | KORMULEV =====================
	syslog( LOG_INFO, "vdagent_x11_create | vdagent-x11.c | line 354" );
	syslog( LOG_INFO, "attrib.width is %d\n", attrib.width );
        syslog( LOG_INFO, "attrib.height is %d\n", attrib.height );
	syslog( LOG_INFO, "x11->width[ %d ] is %d\n", i, x11->width[ i ] );
        syslog( LOG_INFO, "x11->height[ %d ] is %d\n", i, x11->height[ i ] );

// XXX might need to change to a call to function set_scren_to_best_size
	int num_size = 0;
		XRRScreenSize *sizes = XRRSizes( x11->display, i, &num_size );
	int j = 0;
	int max_screen_width = 0;
	int max_screen_height = 0;
	for ( j = 0; j < num_size; ++j ) {
		int tmp_width = sizes[ j ].width;
		if ( tmp_width > max_screen_width )
			max_screen_width = tmp_width;
		int tmp_height = sizes[ j ].height;
		if ( tmp_height > max_screen_height )
			max_screen_height = tmp_height;
	}
	x11->width[ i ] = max_screen_width;
	x11->height[ i ] = max_screen_height;
// ========================================================================
// ================= commented for debugging ==============================
//        x11->width[i]  = attrib.width;
//        x11->height[i] = attrib.height;
// ========================================================================
    }
    vdagent_x11_send_daemon_guest_xorg_res(x11, 1);

    /* Get net_wm_name, since we are started at the same time as the wm,
       sometimes we need to wait a bit for it to show up. */
    i = 10;
    vdagent_x11_get_wm_name(x11);
    while (x11->net_wm_name == NULL && --i > 0) {
        usleep(100000);
        vdagent_x11_get_wm_name(x11);
    }
    if (x11->debug && x11->net_wm_name)
        syslog(LOG_DEBUG, "net_wm_name: \"%s\", has icons: %d",
               x11->net_wm_name, vdagent_x11_has_icons_on_desktop(x11));

    /* Flush output buffers and consume any pending events */
    vdagent_x11_do_read(x11);
// ====================== added for debugging | KORMULEV =================
	syslog( LOG_INFO, "vdagent_x11_create | vdagent-x11.c | line 438" );
	syslog( LOG_INFO, "x11->xfixes_event_base %d\n", x11->xfixes_event_base );
// =======================================================================

    return x11;
}

void vdagent_x11_destroy(struct vdagent_x11 *x11, int vdagentd_disconnected)
{
    uint8_t sel;

    if (!x11)
        return;
// ====================== added for debugging | KORMULEV =================
	syslog( LOG_INFO, "vdagent_x11_destroy | vdagent-x11.c | line 452" );
	syslog( LOG_INFO, "x11->xfixes_event_base %d\n", x11->xfixes_event_base );
// =======================================================================
    if (vdagentd_disconnected)
        x11->vdagentd = NULL;

    for (sel = 0; sel < VD_AGENT_CLIPBOARD_SELECTION_SECONDARY; ++sel) {
        vdagent_x11_set_clipboard_owner(x11, sel, owner_none);
    }

    XCloseDisplay(x11->display);
    g_free(x11->net_wm_name);
    free(x11->randr.failed_conf);
    free(x11);
}

int vdagent_x11_get_fd(struct vdagent_x11 *x11)
{
    return x11->fd;
}

static void vdagent_x11_next_selection_request(struct vdagent_x11 *x11)
{
    struct vdagent_x11_selection_request *selection_request;
    selection_request = x11->selection_req;
    x11->selection_req = selection_request->next;
// ====================== added for debugging | KORMULEV =================
	syslog( LOG_INFO, "vdagent_x11_next_selection_request | vdagent-x11.c | line 454" );
	syslog( LOG_INFO, "x11->xfixes_event_base %d\n", x11->xfixes_event_base );
// =======================================================================

    free(selection_request);
}

static void vdagent_x11_next_conversion_request(struct vdagent_x11 *x11)
{
    struct vdagent_x11_conversion_request *conversion_req;
    conversion_req = x11->conversion_req;
    x11->conversion_req = conversion_req->next;
    free(conversion_req);
}

static void vdagent_x11_set_clipboard_owner(struct vdagent_x11 *x11,
    uint8_t selection, int new_owner)
{
    struct vdagent_x11_selection_request *prev_sel, *curr_sel, *next_sel;
    struct vdagent_x11_conversion_request *prev_conv, *curr_conv, *next_conv;
    int once;

    /* Clear pending requests and clipboard data */
    once = 1;
    prev_sel = NULL;
    next_sel = x11->selection_req;
    while (next_sel) {
        curr_sel = next_sel;
        next_sel = curr_sel->next;
        if (curr_sel->selection == selection) {
            if (once) {
                SELPRINTF("selection requests pending on clipboard ownership "
                          "change, clearing");
                once = 0;
            }
            vdagent_x11_send_selection_notify(x11, None, curr_sel);
            if (curr_sel == x11->selection_req) {
                x11->selection_req = next_sel;
                free(x11->selection_req_data);
                x11->selection_req_data = NULL;
                x11->selection_req_data_pos = 0;
                x11->selection_req_data_size = 0;
                x11->selection_req_atom = None;
            } else {
                prev_sel->next = next_sel;
            }
            free(curr_sel);
        } else {
            prev_sel = curr_sel;
        }
    }

    once = 1;
    prev_conv = NULL;
    next_conv = x11->conversion_req;
    while (next_conv) {
        curr_conv = next_conv;
        next_conv = curr_conv->next;
        if (curr_conv->selection == selection) {
            if (once) {
                SELPRINTF("client clipboard request pending on clipboard "
                          "ownership change, clearing");
                once = 0;
            }
            if (x11->vdagentd)
                udscs_write(x11->vdagentd, VDAGENTD_CLIPBOARD_DATA, selection,
                            VD_AGENT_CLIPBOARD_NONE, NULL, 0);
            if (curr_conv == x11->conversion_req) {
                x11->conversion_req = next_conv;
                x11->clipboard_data_size = 0;
                x11->expect_property_notify = 0;
            } else {
                prev_conv->next = next_conv;
            }
            free(curr_conv);
        } else {
            prev_conv = curr_conv;
        }
    }

    if (new_owner == owner_none) {
        /* When going from owner_guest to owner_none we need to send a
           clipboard release message to the client */
        if (x11->clipboard_owner[selection] == owner_guest && x11->vdagentd) {
            udscs_write(x11->vdagentd, VDAGENTD_CLIPBOARD_RELEASE, selection,
                        0, NULL, 0);
        }
        x11->clipboard_type_count[selection] = 0;
    }
    x11->clipboard_owner[selection] = new_owner;
}

static int vdagent_x11_get_clipboard_atom(struct vdagent_x11 *x11, uint8_t selection, Atom* clipboard)
{
    if (selection == VD_AGENT_CLIPBOARD_SELECTION_CLIPBOARD) {
        *clipboard = x11->clipboard_atom;
    } else if (selection == VD_AGENT_CLIPBOARD_SELECTION_PRIMARY) {
        *clipboard = x11->clipboard_primary_atom;
    } else {
        syslog(LOG_ERR, "get_clipboard_atom: unknown selection");
        return -1;
    }

    return 0;
}

static int vdagent_x11_get_clipboard_selection(struct vdagent_x11 *x11,
    XEvent *event, uint8_t *selection)
{
    Atom atom;
// ================= added for debugging | KORMULEV ================
	syslog( LOG_INFO, "vdagent_x11_get_clipboard_selection | vdagent-x11.c | line 554" );
	syslog( LOG_INFO, "event->type is %d\n", event->type );
	syslog( LOG_INFO, "x11->xfixes_event_base is %d\n", x11->xfixes_event_base );
// =================================================================
    if (event->type == x11->xfixes_event_base) {
        XFixesSelectionNotifyEvent *xfev = (XFixesSelectionNotifyEvent *)event;
        atom = xfev->selection;
    } else if (event->type == SelectionNotify) {
        atom = event->xselection.selection;
    } else if (event->type == SelectionRequest) {
        atom = event->xselectionrequest.selection;
    } else {
        syslog(LOG_ERR, "get_clipboard_selection: unknown event type");
        return -1;
    }

    if (atom == x11->clipboard_atom) {
        *selection = VD_AGENT_CLIPBOARD_SELECTION_CLIPBOARD;
    } else if (atom == x11->clipboard_primary_atom) {
        *selection = VD_AGENT_CLIPBOARD_SELECTION_PRIMARY;
    } else {
        syslog(LOG_ERR, "get_clipboard_selection: unknown selection");
        return -1;
    }

    return 0;
}

// ==================== added for debugging | KORMULEV =======================================================
static void free_randr_resources( struct vdagent_x11 *x11 ) {
	int i;
	if ( !x11->randr.res )
		return;

	if ( NULL != x11->randr.outputs ) {
		for ( i = 0; i < x11->randr.res->noutput; ++i ) {
			XRRFreeOutputInfo( x11->randr.outputs[ i ] );
		}
		free( x11->randr.outputs );
	}
	if ( NULL != x11->randr.crtcs ) {
		for ( i = 0; i < x11->randr.res->ncrtc; ++i ) {
			XRRFreeCrtcInfo( x11->randr.crtcs[ i ] );
		}
		free( x11->randr.crtcs );
	}
	XRRFreeScreenResources( x11->randr.res );
	x11->randr.res = NULL;
	x11->randr.outputs = NULL;
	x11->randr.crtcs = NULL;
	x11->randr.num_monitors = 0;
}

static void update_randr_res( struct vdagent_x11 *x11, int poll ) {
	int i;
	free_randr_resources( x11 );
	if ( poll )
		x11->randr.res = XRRGetScreenResources( x11->display, x11->root_window[ 0 ] );
	else
		x11->randr.res = XRRGetScreenResourcesCurrent( x11->display, x11->root_window[ 0 ] );
	x11->randr.outputs = malloc( x11->randr.res->noutput * sizeof( *x11->randr.outputs ) );
	if ( NULL == x11->randr.outputs ) {
		syslog( LOG_ERR, "Couldn't allocate enough memory for randr.outputs" );
	}
	x11->randr.crtcs = malloc( x11->randr.res->ncrtc * sizeof( *x11->randr.crtcs ) );
	if ( NULL == x11->randr.outputs ) {
		syslog( LOG_ERR, "Couldn't allocate enough memory for randr.crtcs" );
	}

	for ( i = 0; i < x11->randr.res->noutput; ++i ) {
		x11->randr.outputs[ i ] = XRRGetOutputInfo( x11->display, x11->randr.res,
							    x11->randr.res->outputs[ i ] );

		if ( RR_Connected == x11->randr.outputs[ i ]->connection )
			x11->randr.num_monitors++;
	}
	for ( i = 0; i < x11->randr.res->ncrtc; ++i ) {
		x11->randr.crtcs[ i ] = XRRGetCrtcInfo( x11->display, x11->randr.res,
							x11->randr.res->crtcs[ i ] );
// ======================= added for debugging ===================================
		x11->randr.crtcs[ i ]->width = x11->width[ 0 ];
		x11->randr.crtcs[ i ]->height = x11->height[ 0 ];
// ===============================================================================
	}
	if ( 1 != XRRGetScreenSizeRange( x11->display, x11->root_window[ 0 ],
				    &x11->randr.min_width,
				    &x11->randr.min_height,
				    &x11->randr.max_width,
				    &x11->randr.max_height ) ) {
		syslog( LOG_ERR, "update_randr_res" );
	}

}

static int set_screen_to_defined_size( struct vdagent_x11 *x11, int width, int height, 
				   int *out_width, int *out_height ) {

	int i, num_sizes = 0;
	int best = -1;
	unsigned int closest_diff = -1;
	XRRScreenSize *sizes;
	XRRScreenConfiguration *config;
	Rotation rotation;

	sizes = XRRSizes( x11->display, 0, &num_sizes );
	if ( NULL == sizes || !num_sizes ) {
		syslog( LOG_ERR, "XRRSizes failed" );

		return 0;
	}
	if ( x11->debug )
		syslog( LOG_DEBUG, "set_screen_to_defined_size found %d modes\n", num_sizes );

	for ( i = 0; i < num_sizes; ++i ) {
		if ( sizes[ i ].width > width ||
		     sizes[ i ].height > height )
			continue;

		unsigned int wdiff = width - sizes[ i ].width;
		unsigned int hdiff = height - sizes[ i ].height;
		unsigned int diff = wdiff * wdiff + hdiff * hdiff;
		if ( diff < closest_diff ) {
			closest_diff = diff;
			best = i;
		}
	}

	if ( -1 == best ) {
		syslog( LOG_ERR, "no suitable resolution found for monitor" );

		return 0;
	}

	config = XRRGetScreenInfo( x11->display, x11->root_window[ 0 ] );
	if ( NULL == config ) {
		syslog( LOG_ERR, "get screen info failed" );

		return 0;
	}
	XRRConfigCurrentConfiguration( config, &rotation );
	XRRSetScreenConfig( x11->display, config, x11->root_window[ 0 ], best,
			    rotation, CurrentTime );
	XRRFreeScreenConfigInfo( config );
	if ( x11->debug )
		syslog( LOG_DEBUG, "set_screen_to_defined_size set size to: %dx%d\n",
			sizes[ best ].width, sizes[ best ].height );

	*out_width = sizes[ best ].width;
	*out_height = sizes[ best ].height;

	return 1;
}

void vdagent_process_screen_size_change( struct vdagent_x11 *x11 )
{

// ============= added for debugging | KORMULEV ==================
     syslog( LOG_INFO, "<<<<<<<<<<<<<<< !!!!!!!!!!!!!!!!! vdagent_process_screen_size_change !!!!!!!!!!!!!!!!!!!!! >>>>>>>>>>>>>>>>>" );
     syslog( LOG_INFO, "Catch resolution changes | vdagent_process_screen_size_change | vdagent-x11.c | line 567 " );
    // ===============================================================
	system( "xrandr --output Virtual-0 --auto" );
/* 
   XWindowAttributes attrib;
   XRRScreenSize *sizes = NULL;
   XRRScreenConfiguration **sc;
   int i = 0;
   int out_width = 0;
   int out_height = 0;
   for ( i = 0; i < x11->screen_count; ++i ) {
      // Catch resolution changes
      XSelectInput( x11->display, x11->root_window[ i ], StructureNotifyMask );
      XRRScreenConfiguration *tmp = XRRGetScreenInfo( x11->display, x11->root_window[ i ] );
      if ( NULL == tmp ) {
	       syslog( LOG_ERR, "Could not get screen info" );
      }
      sc = ( XRRScreenConfiguration ** )malloc( x11->screen_count * sizeof( tmp ) ); 
      if ( NULL == sc ) {
		syslog( LOG_ERR, "Could not allocate memory. Not enough available" );
      }
      sc[ i ] = tmp;
      if ( NULL == sc ) {
		syslog( LOG_ERR, "Something went wrong. Could not assign screen info correctly" );
      }
      
      // Get the current resolution
      XGetWindowAttributes( x11->display, x11->root_window[ i ], &attrib );
      int num_size = 0;
      sizes = XRRSizes( x11->display, i, &num_size );
      x11->width[ i ] = sizes[ 0 ].width;
      x11->height[ i ] = sizes[ 0 ].height;
   }

      // =================== added for debugging | KORMULEV =====================
      syslog( LOG_INFO, "vdagent_process_screen_size_change | vdagent-x11.c | line 573" );
      syslog( LOG_INFO, "attrib.width is %d\n", attrib.width );
      syslog( LOG_INFO, "attrib.height is %d\n", attrib.height );
      syslog( LOG_INFO, "sizes[ i ].width is %d\n", sizes[ i ].width );
      syslog( LOG_INFO, "sizes[ i ].height is %d\n", sizes[ i ].height );
      syslog( LOG_INFO, "x11->width[ %d ] is %d\n", i, x11->width[ i ] );
      syslog( LOG_INFO, "x11->height[ %d ] is %d\n", i, x11->height[ i ] );
      // ========================================================================

        set_screen_to_defined_size( x11, x11->width[ i ], x11->height[ i ], &out_width, &out_height );
// =================== added for debugging | KORMULEV =====================
      syslog( LOG_INFO, "vdagent_process_screen_size_change | vdagent-x11.c | line 737" );
      syslog( LOG_INFO, "attrib.width is %d\n", attrib.width );
      syslog( LOG_INFO, "attrib.height is %d\n", attrib.height );
      syslog( LOG_INFO, "sizes[ i ].width is %d\n", sizes[ i ].width );
      syslog( LOG_INFO, "sizes[ i ].height is %d\n", sizes[ i ].height );
      syslog( LOG_INFO, "x11->width[ %d ] is %d\n", i, x11->width[ i ] );
      syslog( LOG_INFO, "x11->height[ %d ] is %d\n", i, x11->height[ i ] );
// ========================================================================
    update_randr_res( x11, 0 );
    vdagent_x11_send_daemon_guest_xorg_res(x11, 1);
    x11->dont_send_guest_xorg_res = 1;
    vdagent_x11_do_read( x11 );
    x11->dont_send_guest_xorg_res = 0;
    vdagent_x11_send_daemon_guest_xorg_res(x11, 0 );
    vdagent_x11_do_read( x11 );
*/
}

// ============================================================================================================


static void vdagent_x11_handle_event(struct vdagent_x11 *x11, XEvent event)
{
    int i, handled = 0;
    uint8_t selection;
// ================= added for debugging | KORMULEV ================
	syslog( LOG_INFO, "vdagent_x11_handle_event | vdagent-x11.c | line 586" );
	syslog( LOG_INFO, "event.type is %d", event.type );
	syslog( LOG_INFO, "x11->xfixes_event_base is %d\n", x11->xfixes_event_base );
// =================================================================
    if (event.type == x11->xfixes_event_base) {
        union {
            XEvent ev;
            XFixesSelectionNotifyEvent xfev;
        } ev;

        if (vdagent_x11_get_clipboard_selection(x11, &event, &selection)) {
            return;
        }

        ev.ev = event;
        switch (ev.xfev.subtype) {
        case XFixesSetSelectionOwnerNotify:
            break;
        /* Treat ... as a SelectionOwnerNotify None */
        case XFixesSelectionWindowDestroyNotify:
        case XFixesSelectionClientCloseNotify:
            ev.xfev.owner = None;
            break;
        default:
            VSELPRINTF("unexpected xfix event subtype %d window %d",
                       (int)ev.xfev.subtype, (int)event.xany.window);
            return;
        }
        VSELPRINTF("New selection owner: %u", (unsigned int)ev.xfev.owner);

        /* Ignore becoming the owner ourselves */
        if (ev.xfev.owner == x11->selection_window)
            return;

        /* If the clipboard owner is changed we no longer own it */
        vdagent_x11_set_clipboard_owner(x11, selection, owner_none);
// =================== added for debugging | kormulev =====================
	syslog( LOG_INFO, "vdagent_x11_handle_event | vdagent-x11.c | line 659" );
	syslog( LOG_INFO, "x11->xfixes_event_base is %d\n", x11->xfixes_event_base );
// ========================================================================
        if (ev.xfev.owner == None)
            return;

        /* Request the supported targets from the new owner */
        XConvertSelection(x11->display, ev.xfev.selection, x11->targets_atom,
                          x11->targets_atom, x11->selection_window,
                          CurrentTime);

        x11->expected_targets_notifies[selection]++;
        return;
    }
// =================== added for debugging | kormulev =====================
	syslog( LOG_INFO, "<<<<<< ========================================================== >>>>>" );
	syslog( LOG_INFO, "vdagent_x11_handle_event | vdagent-x11.c | line 627" );
	syslog( LOG_INFO, "event \"configurenotify\" is %d\n", ConfigureNotify );
	syslog( LOG_INFO, "event event.type is %d\n", event.type );
	syslog( LOG_INFO, "event.xconfigure.width is %d\n", event.xconfigure.width );
	syslog( LOG_INFO, "event.xconfigure.height is %d\n", event.xconfigure.height );
	syslog( LOG_INFO, "x11->width[ 0 ] is %d\n", x11->width[ 0 ] );
	syslog( LOG_INFO, "x11->height[ 0 ] is %d\n", x11->height[ 0 ] );
//	syslog( LOG_INFO, "x11->width[ 1 ] is %d\n", x11->width[ 1 ] );
//	syslog( LOG_INFO, "x11->height[ 1 ] is %d\n", x11->height[ 1 ] );
	syslog( LOG_INFO, "<<<<<< ========================================================== >>>>>" );
// ========================================================================
    switch (event.type) {
    case ConfigureNotify:
        // TODO: handle CrtcConfigureNotify, OutputConfigureNotify can be ignored.
        for (i = 0; i < x11->screen_count; i++)
            if (event.xconfigure.window == x11->root_window[i])
                break;
        if (i == x11->screen_count)
            break;

        handled = 1;
        vdagent_x11_randr_handle_root_size_change(x11, i,
                event.xconfigure.width, event.xconfigure.height);
// =================== added for debugging | KORMULEV =====================
	syslog( LOG_INFO, "vdagent_x11_handle_event | vdagent-x11.c | line 644" );
	syslog( LOG_INFO, "event width is %d\n", event.xconfigure.width );
        syslog( LOG_INFO, "event height is %d\n", event.xconfigure.height);
// ========================================================================
        break;
    case MappingNotify:
        /* These are uninteresting */
        handled = 1;
        break;
    case SelectionNotify:
        if (event.xselection.target == x11->targets_atom)
            vdagent_x11_handle_targets_notify(x11, &event);
        else
            vdagent_x11_handle_selection_notify(x11, &event, 0);

        handled = 1;
        break;
    case PropertyNotify:
        if (x11->expect_property_notify &&
                                event.xproperty.state == PropertyNewValue) {
            vdagent_x11_handle_selection_notify(x11, &event, 1);
        }
        if (x11->selection_req_data && 
                                 event.xproperty.state == PropertyDelete) {
            vdagent_x11_handle_property_delete_notify(x11, &event);
        }
        /* Always mark as handled, since we cannot unselect input for property
           notifications once we are done with handling the incr transfer. */
        handled = 1;
        break;
    case SelectionClear:
        /* Do nothing the clipboard ownership will get updated through
           the XFixesSetSelectionOwnerNotify event */
        handled = 1;
        break;
    case SelectionRequest: {
        struct vdagent_x11_selection_request *req, *new_req;

        if (vdagent_x11_get_clipboard_selection(x11, &event, &selection)) {
            return;
        }

        new_req = malloc(sizeof(*new_req));
        if (!new_req) {
            SELPRINTF("out of memory on SelectionRequest, ignoring.");
            break;
        }

        handled = 1;

        new_req->event = event;
        new_req->selection = selection;
        new_req->next = NULL;

        if (!x11->selection_req) {
            x11->selection_req = new_req;
            vdagent_x11_handle_selection_request(x11);
            break;
        }

        /* maybe we should limit the selection_request stack depth ? */
        req = x11->selection_req;
        while (req->next)
            req = req->next;

        req->next = new_req;
        break;
    }
    }
    if (!handled && x11->debug)
        syslog(LOG_DEBUG, "unhandled x11 event, type %d, window %d",
               (int)event.type, (int)event.xany.window);
}

void vdagent_x11_do_read(struct vdagent_x11 *x11)
{
    XEvent event;

    while (XPending(x11->display)) {
        XNextEvent(x11->display, &event);
// ============= added for debugging | KORMULEV =================
	//XRRUpdateConfiguration( &event );
	syslog( LOG_INFO, "vdagent_x11_do_read | vdagent-x11.c | event->type %d\n", event.type );
// ==============================================================
        vdagent_x11_handle_event(x11, event);
    }
}

static const char *vdagent_x11_get_atom_name(struct vdagent_x11 *x11, Atom a)
{
    if (a == None)
        return "None";

    return XGetAtomName(x11->display, a);
}

static int vdagent_x11_get_selection(struct vdagent_x11 *x11, XEvent *event,
    uint8_t selection, Atom type, Atom prop, int format,
    unsigned char **data_ret, int incr)
{
    Bool del = incr ? True: False;
    Atom type_ret;
    int format_ret, ret_val = -1;
    unsigned long len, remain;
    unsigned char *data = NULL;
// =================== added for debugging | KORMULEV =====================
	syslog( LOG_INFO, "vdagent_x11_get_selection | vdagent-x11.c | line 776" );
	syslog( LOG_INFO, "event->type is %d\n", event->type );
// ========================================================================

    *data_ret = NULL;

    if (!incr) {
        if (event->xselection.property == None) {
            VSELPRINTF("XConvertSelection refused by clipboard owner");
            goto exit;
        }

        if (event->xselection.requestor != x11->selection_window ||
            event->xselection.property != prop) {
            SELPRINTF("SelectionNotify parameters mismatch");
            goto exit;
        }
    }

    if (XGetWindowProperty(x11->display, x11->selection_window, prop, 0,
                           LONG_MAX, del, type, &type_ret, &format_ret, &len,
                           &remain, &data) != Success) {
        SELPRINTF("XGetWindowProperty failed");
        goto exit;
    }

    if (!incr && prop != x11->targets_atom) {
        if (type_ret == x11->incr_atom) {
            int prop_min_size = *(uint32_t*)data;

            if (x11->expect_property_notify) {
                SELPRINTF("received an incr SelectionNotify while "
                          "still reading another incr property");
                goto exit;
            }

            if (x11->clipboard_data_space < prop_min_size) {
                free(x11->clipboard_data);
                x11->clipboard_data = malloc(prop_min_size);
                if (!x11->clipboard_data) {
                    SELPRINTF("out of memory allocating clipboard buffer");
                    x11->clipboard_data_space = 0;
                    goto exit;
                }
                x11->clipboard_data_space = prop_min_size;
            }
            x11->expect_property_notify = 1;
// =================== added for debugging | KORMULEV =====================
	syslog( LOG_INFO, "vdagent_x11_get_selection | vdagent-x11.c | line 823" );
	syslog( LOG_INFO, "event->type is %d\n", event->type );
	syslog( LOG_INFO, "Before XSelectInput" );
// ========================================================================
            XSelectInput(x11->display, x11->selection_window,
                         PropertyChangeMask);
// =================== added for debugging | KORMULEV =====================
	syslog( LOG_INFO, "vdagent_x11_get_selection | vdagent-x11.c | line 831" );
	syslog( LOG_INFO, "event->type is %d\n", event->type );
	syslog( LOG_INFO, "After XSelectInput" );
// ========================================================================

            XDeleteProperty(x11->display, x11->selection_window, prop);
            XFree(data);
            return 0; /* Wait for more data */
        }
        XDeleteProperty(x11->display, x11->selection_window, prop);
    }

    if (type_ret != type) {
        SELPRINTF("expected property type: %s, got: %s",
                  vdagent_x11_get_atom_name(x11, type),
                  vdagent_x11_get_atom_name(x11, type_ret));
        goto exit;
    }

    if (format_ret != format) {
        SELPRINTF("expected %d bit format, got %d bits", format, format_ret);
        goto exit;
    }

    /* Convert len to bytes */
    switch(format) {
    case 8:
        break;
    case 16:
        len *= sizeof(short);
        break;
    case 32:
        len *= sizeof(long);
        break;
    }

    if (incr) {
        if (len) {
            if (x11->clipboard_data_size + len > x11->clipboard_data_space) {
                void *old_clipboard_data = x11->clipboard_data;

                x11->clipboard_data_space = x11->clipboard_data_size + len;
                x11->clipboard_data = realloc(x11->clipboard_data,
                                              x11->clipboard_data_space);
                if (!x11->clipboard_data) {
                    SELPRINTF("out of memory allocating clipboard buffer");
                    x11->clipboard_data_space = 0;
                    free(old_clipboard_data);
                    goto exit;
                }
            }
            memcpy(x11->clipboard_data + x11->clipboard_data_size, data, len);
            x11->clipboard_data_size += len;
            VSELPRINTF("Appended %ld bytes to buffer", len);
            XFree(data);
            return 0; /* Wait for more data */
        }
        len = x11->clipboard_data_size;
        *data_ret = x11->clipboard_data;
    } else
        *data_ret = data;

    if (len > 0) {
        ret_val = len;
    } else {
        SELPRINTF("property contains no data (zero length)");
        *data_ret = NULL;
    }

exit:
    if ((incr || ret_val == -1) && data)
        XFree(data);

    if (incr) {
        x11->clipboard_data_size = 0;
        x11->expect_property_notify = 0;
    }

    return ret_val;
}

static void vdagent_x11_get_selection_free(struct vdagent_x11 *x11,
    unsigned char *data, int incr)
{
    if (incr) {
        /* If the clipboard has grown large return the memory to the system */
        if (x11->clipboard_data_space > 512 * 1024) {
            free(x11->clipboard_data);
            x11->clipboard_data = NULL;
            x11->clipboard_data_space = 0;
        }
    } else if (data)
        XFree(data);
}

static uint32_t vdagent_x11_target_to_type(struct vdagent_x11 *x11,
    uint8_t selection, Atom target)
{
    int i, j;
// =================== added for debugging | KORMULEV =====================
	syslog( LOG_INFO, "vdagent_x11_target_to_type | vdagent-x11.c | line 954" );
	syslog( LOG_INFO, "x11->xfixes_event_base is %d\n", x11->xfixes_event_base );
// ========================================================================

    for (i = 0; i < clipboard_format_count; i++) {
        for (j = 0; j < x11->clipboard_formats[i].atom_count; j++) {
            if (x11->clipboard_formats[i].atoms[j] == target) {
                return x11->clipboard_formats[i].type;
            }
        }
    }

    SELPRINTF("unexpected selection type %s",
              vdagent_x11_get_atom_name(x11, target));
    return VD_AGENT_CLIPBOARD_NONE;
}

static Atom vdagent_x11_type_to_target(struct vdagent_x11 *x11,
                                       uint8_t selection, uint32_t type)
{
    int i;

    for (i = 0; i < x11->clipboard_type_count[selection]; i++) {
        if (x11->clipboard_agent_types[selection][i] == type) {
            return x11->clipboard_x11_targets[selection][i];
        }
    }
    SELPRINTF("client requested unavailable type %u", type);
    return None;
}

static void vdagent_x11_handle_conversion_request(struct vdagent_x11 *x11)
{
    Atom clip = None;

    if (!x11->conversion_req) {
        return;
    }

    vdagent_x11_get_clipboard_atom(x11, x11->conversion_req->selection, &clip);
    XConvertSelection(x11->display, clip, x11->conversion_req->target,
                      clip, x11->selection_window, CurrentTime);
}

static void vdagent_x11_handle_selection_notify(struct vdagent_x11 *x11,
                                                XEvent *event, int incr)
{
    int len = 0;
    unsigned char *data = NULL;
    uint32_t type;
    uint8_t selection = -1;
    Atom clip = None;

    if (!x11->conversion_req) {
        syslog(LOG_ERR, "SelectionNotify received without a target");
        return;
    }
    vdagent_x11_get_clipboard_atom(x11, x11->conversion_req->selection, &clip);

    if (incr) {
        if (event->xproperty.atom != clip ||
                event->xproperty.window != x11->selection_window) {
            return;
        }
    } else {
        if (vdagent_x11_get_clipboard_selection(x11, event, &selection)) {
            len = -1;
        } else if (selection != x11->conversion_req->selection) {
            SELPRINTF("Requested data for selection %d got %d",
                      (int)x11->conversion_req->selection, (int)selection);
            len = -1;
        }
        if (event->xselection.target != x11->conversion_req->target &&
                event->xselection.target != x11->incr_atom) {
            SELPRINTF("Requested %s target got %s",
                vdagent_x11_get_atom_name(x11, x11->conversion_req->target),
                vdagent_x11_get_atom_name(x11, event->xselection.target));
            len = -1;
        }
    }

    selection = x11->conversion_req->selection;
    type = vdagent_x11_target_to_type(x11, selection,
                                      x11->conversion_req->target);
    if (len == 0) { /* No errors so far */
        len = vdagent_x11_get_selection(x11, event, selection,
                                        x11->conversion_req->target,
                                        clip, 8, &data, incr);
        if (len == 0) { /* waiting for more data? */
            return;
        }
    }
    if (len == -1) {
        type = VD_AGENT_CLIPBOARD_NONE;
        len = 0;
    }

    udscs_write(x11->vdagentd, VDAGENTD_CLIPBOARD_DATA, selection, type,
                data, len);
    vdagent_x11_get_selection_free(x11, data, incr);

    vdagent_x11_next_conversion_request(x11);
    vdagent_x11_handle_conversion_request(x11);
}

static Atom atom_lists_overlap(Atom *atoms1, Atom *atoms2, int l1, int l2)
{
    int i, j;

    for (i = 0; i < l1; i++)
        for (j = 0; j < l2; j++)
            if (atoms1[i] == atoms2[j])
                return atoms1[i];

    return 0;
}

static void vdagent_x11_print_targets(struct vdagent_x11 *x11,
    uint8_t selection, const char *action, Atom *atoms, int c)
{
    int i;
// =================== added for debugging | KORMULEV =====================
	syslog( LOG_INFO, "vdagent_x11_print_targets | vdagent-x11.c | line 1076" );
	syslog( LOG_INFO, "x11->xfixes_event_base is %d\n", x11->xfixes_event_base );
// ========================================================================
    VSELPRINTF("%s %d targets:", action, c);
    for (i = 0; i < c; i++)
        VSELPRINTF("%s", vdagent_x11_get_atom_name(x11, atoms[i]));
}

static void vdagent_x11_handle_targets_notify(struct vdagent_x11 *x11,
                                              XEvent *event)
{
    int i, len;
    Atom atom, *atoms = NULL;
    uint8_t selection;
    int *type_count;
// =================== added for debugging | KORMULEV =====================
	syslog( LOG_INFO, "vdagent_x11_handle_targets_notify | vdagent-x11.c | line 1092" );
	syslog( LOG_INFO, "x11->xfixes_event_base is %d\n", x11->xfixes_event_base );
	syslog( LOG_INFO, "event->type is %d\n", event->type );
// ========================================================================   
    if (vdagent_x11_get_clipboard_selection(x11, event, &selection)) {
        return;
    }

    if (!x11->expected_targets_notifies[selection]) {
        SELPRINTF("unexpected selection notify TARGETS");
        return;
    }

    x11->expected_targets_notifies[selection]--;

    /* If we have more targets_notifies pending, ignore this one, we
       are only interested in the targets list of the current owner
       (which is the last one we've requested a targets list from) */
    if (x11->expected_targets_notifies[selection]) {
        return;
    }

    len = vdagent_x11_get_selection(x11, event, selection,
                                    XA_ATOM, x11->targets_atom, 32,
                                    (unsigned char **)&atoms, 0);
    if (len == 0 || len == -1) /* waiting for more data or error? */
        return;

    /* bytes -> atoms */
    len /= sizeof(Atom);
    vdagent_x11_print_targets(x11, selection, "received", atoms, len);

    type_count = &x11->clipboard_type_count[selection];
    *type_count = 0;
    for (i = 0; i < clipboard_format_count; i++) {
        atom = atom_lists_overlap(x11->clipboard_formats[i].atoms, atoms,
                                  x11->clipboard_formats[i].atom_count, len);
        if (atom) {
            x11->clipboard_agent_types[selection][*type_count] =
                x11->clipboard_formats[i].type;
            x11->clipboard_x11_targets[selection][*type_count] = atom;
            (*type_count)++;
            if (*type_count ==
                    sizeof(x11->clipboard_agent_types[0])/sizeof(uint32_t)) {
                SELPRINTF("handle_targets_notify: too many types");
                break;
            }
        }
    }

    if (*type_count) {
        udscs_write(x11->vdagentd, VDAGENTD_CLIPBOARD_GRAB, selection, 0,
                    (uint8_t *)x11->clipboard_agent_types[selection],
                    *type_count * sizeof(uint32_t));
        vdagent_x11_set_clipboard_owner(x11, selection, owner_guest);
    }

    vdagent_x11_get_selection_free(x11, (unsigned char *)atoms, 0);
}

static void vdagent_x11_send_selection_notify(struct vdagent_x11 *x11,
    Atom prop, struct vdagent_x11_selection_request *request)
{
    XEvent res, *event;

    if (request) {
        event = &request->event;
    } else {
        event = &x11->selection_req->event;
    }
// =================== added for debugging | KORMULEV =====================
	syslog( LOG_INFO, "vdagent_x11_send_selection_notify | vdagent-x11.c | line 1163" );
	syslog( LOG_INFO, "x11->xfixes_event_base is %d\n", x11->xfixes_event_base );
	syslog( LOG_INFO, "event->type is %d\n", event->type );
// ======================================================================== 
    res.xselection.property = prop;
    res.xselection.type = SelectionNotify;
    res.xselection.display = event->xselectionrequest.display;
    res.xselection.requestor = event->xselectionrequest.requestor;
    res.xselection.selection = event->xselectionrequest.selection;
    res.xselection.target = event->xselectionrequest.target;
    res.xselection.time = event->xselectionrequest.time;

    vdagent_x11_set_error_handler(x11, vdagent_x11_ignore_bad_window_handler);
    XSendEvent(x11->display, event->xselectionrequest.requestor, 0, 0, &res);
    vdagent_x11_restore_error_handler(x11);

    if (!request) {
        vdagent_x11_next_selection_request(x11);
        vdagent_x11_handle_selection_request(x11);
    }
}

static void vdagent_x11_send_targets(struct vdagent_x11 *x11,
    uint8_t selection, XEvent *event)
{
    Atom prop, targets[256] = { x11->targets_atom, };
    int i, j, k, target_count = 1;
// =================== added for debugging | KORMULEV =====================
	syslog( LOG_INFO, "vdagent_x11_send_targets | vdagent-x11.c | line 1191" );
	syslog( LOG_INFO, "x11->xfixes_event_base is %d\n", x11->xfixes_event_base );
	syslog( LOG_INFO, "event->type is %d\n", event->type );
// ========================================================================
    for (i = 0; i < x11->clipboard_type_count[selection]; i++) {
        for (j = 0; j < clipboard_format_count; j++) {
            if (x11->clipboard_formats[j].type !=
                    x11->clipboard_agent_types[selection][i])
                continue;

            for (k = 0; k < x11->clipboard_formats[j].atom_count; k++) {
                targets[target_count] = x11->clipboard_formats[j].atoms[k];
                target_count++;
                if (target_count == sizeof(targets)/sizeof(Atom)) {
                    SELPRINTF("send_targets: too many targets");
                    goto exit_loop;
                }
            }
        }
    }
exit_loop:

    prop = event->xselectionrequest.property;
    if (prop == None)
        prop = event->xselectionrequest.target;

    vdagent_x11_set_error_handler(x11, vdagent_x11_ignore_bad_window_handler);
    XChangeProperty(x11->display, event->xselectionrequest.requestor, prop,
                    XA_ATOM, 32, PropModeReplace, (unsigned char *)&targets,
                    target_count);
    if (vdagent_x11_restore_error_handler(x11) == 0) {
        vdagent_x11_print_targets(x11, selection, "sent",
                                  targets, target_count);
        vdagent_x11_send_selection_notify(x11, prop, NULL);
    } else
       SELPRINTF("send_targets: Failed to sent, requestor window gone");
}

static void vdagent_x11_handle_selection_request(struct vdagent_x11 *x11)
{
    XEvent *event;
    uint32_t type = VD_AGENT_CLIPBOARD_NONE;
    uint8_t selection;
    if (!x11->selection_req)
        return;

    event = &x11->selection_req->event;
    selection = x11->selection_req->selection;
// =================== added for debugging | KORMULEV =====================
	syslog( LOG_INFO, "vdagent_x11_send_targets | vdagent-x11.c | line 1240" );
	syslog( LOG_INFO, "x11->xfixes_event_base is %d\n", x11->xfixes_event_base );
	syslog( LOG_INFO, "event->type is %d\n", event->type );
// ========================================================================

    if (x11->clipboard_owner[selection] != owner_client) {
        SELPRINTF("received selection request event for target %s, "
                  "while not owning client clipboard",
            vdagent_x11_get_atom_name(x11, event->xselectionrequest.target));
        vdagent_x11_send_selection_notify(x11, None, NULL);
        return;
    }

    if (event->xselectionrequest.target == x11->multiple_atom) {
        SELPRINTF("multiple target not supported");
        vdagent_x11_send_selection_notify(x11, None, NULL);
        return;
    }

    if (event->xselectionrequest.target == x11->timestamp_atom) {
        /* TODO: use more accurate selection time */
        guint32 timestamp = event->xselectionrequest.time;

        XChangeProperty(x11->display, event->xselectionrequest.requestor,
                       event->xselectionrequest.property,
                        event->xselectionrequest.target, 32, PropModeReplace,
                        (guint8*)&timestamp, 1);
        vdagent_x11_send_selection_notify(x11,
                       event->xselectionrequest.property, NULL);
       return;
    }


    if (event->xselectionrequest.target == x11->targets_atom) {
        vdagent_x11_send_targets(x11, selection, event);
        return;
    }

    type = vdagent_x11_target_to_type(x11, selection,
                                      event->xselectionrequest.target);
    if (type == VD_AGENT_CLIPBOARD_NONE) {
        vdagent_x11_send_selection_notify(x11, None, NULL);
        return;
    }

    udscs_write(x11->vdagentd, VDAGENTD_CLIPBOARD_REQUEST, selection, type,
                NULL, 0);
}

static void vdagent_x11_handle_property_delete_notify(struct vdagent_x11 *x11,
                                                      XEvent *del_event)
{
    XEvent *sel_event;
    int len;
    uint8_t selection;

    assert(x11->selection_req);
    sel_event = &x11->selection_req->event;
    selection = x11->selection_req->selection;
    if (del_event->xproperty.window != sel_event->xselectionrequest.requestor
            || del_event->xproperty.atom != x11->selection_req_atom) {
        return;
    }

    len = x11->selection_req_data_size - x11->selection_req_data_pos;
    if (len > x11->max_prop_size) {
        len = x11->max_prop_size;
    }

    if (len) {
        VSELPRINTF("Sending %d-%d/%d bytes of clipboard data",
                x11->selection_req_data_pos,
                x11->selection_req_data_pos + len - 1,
                x11->selection_req_data_size);
    } else {
        VSELPRINTF("Ending incr send of clipboard data");
    }
    vdagent_x11_set_error_handler(x11, vdagent_x11_ignore_bad_window_handler);
    XChangeProperty(x11->display, sel_event->xselectionrequest.requestor,
                    x11->selection_req_atom,
                    sel_event->xselectionrequest.target, 8, PropModeReplace,
                    x11->selection_req_data + x11->selection_req_data_pos,
                    len);
    if (vdagent_x11_restore_error_handler(x11)) {
        SELPRINTF("incr sent failed, requestor window gone");
        len = 0;
    }

    x11->selection_req_data_pos += len;

    /* Note we must explictly send a 0 sized XChangeProperty to signal the
       incr transfer is done. Hence we do not check if we've send all data
       but instead check we've send the final 0 sized XChangeProperty. */
    if (len == 0) {
        free(x11->selection_req_data);
        x11->selection_req_data = NULL;
        x11->selection_req_data_pos = 0;
        x11->selection_req_data_size = 0;
        x11->selection_req_atom = None;
        vdagent_x11_next_selection_request(x11);
        vdagent_x11_handle_selection_request(x11);
    }
}

void vdagent_x11_clipboard_request(struct vdagent_x11 *x11,
        uint8_t selection, uint32_t type)
{
    Atom target, clip;
    struct vdagent_x11_conversion_request *req, *new_req;

    /* We don't use clip here, but we call get_clipboard_atom to verify
       selection is valid */
    if (vdagent_x11_get_clipboard_atom(x11, selection, &clip)) {
        goto none;
    }

    if (x11->clipboard_owner[selection] != owner_guest) {
        SELPRINTF("received clipboard req while not owning guest clipboard");
        goto none;
    }

    target = vdagent_x11_type_to_target(x11, selection, type);
    if (target == None) {
        goto none;
    }

    new_req = malloc(sizeof(*new_req));
    if (!new_req) {
        SELPRINTF("out of memory on client clipboard request, ignoring.");
        return;
    }

    new_req->target = target;
    new_req->selection = selection;
    new_req->next = NULL;
// =================== added for debugging | KORMULEV =====================
	syslog( LOG_INFO, "vdagent_x11_clipboard_request | vdagent-x11.c | line 1376" );
	syslog( LOG_INFO, "x11->xfixes_event_base is %d\n", x11->xfixes_event_base );
// ========================================================================

    if (!x11->conversion_req) {
        x11->conversion_req = new_req;
        vdagent_x11_handle_conversion_request(x11);
        /* Flush output buffers and consume any pending events */
        vdagent_x11_do_read(x11);
        return;
    }
// =================== added for debugging | KORMULEV =====================
	syslog( LOG_INFO, "vdagent_x11_clipboard_request | vdagent-x11.c | line 1388" );
	syslog( LOG_INFO, "x11->xfixes_event_base is %d\n", x11->xfixes_event_base );
// ========================================================================

    /* maybe we should limit the conversion_request stack depth ? */
    req = x11->conversion_req;
    while (req->next)
        req = req->next;

    req->next = new_req;
    return;

none:
    udscs_write(x11->vdagentd, VDAGENTD_CLIPBOARD_DATA,
                selection, VD_AGENT_CLIPBOARD_NONE, NULL, 0);
}

void vdagent_x11_clipboard_grab(struct vdagent_x11 *x11, uint8_t selection,
    uint32_t *types, uint32_t type_count)
{
    Atom clip = None;

    if (vdagent_x11_get_clipboard_atom(x11, selection, &clip)) {
        return;
    }

    if (type_count > sizeof(x11->clipboard_agent_types[0])/sizeof(uint32_t)) {
        SELPRINTF("x11_clipboard_grab: too many types");
        type_count = sizeof(x11->clipboard_agent_types[0])/sizeof(uint32_t);
    }

    memcpy(x11->clipboard_agent_types[selection], types,
           type_count * sizeof(uint32_t));
    x11->clipboard_type_count[selection] = type_count;

    XSetSelectionOwner(x11->display, clip,
                       x11->selection_window, CurrentTime);
    vdagent_x11_set_clipboard_owner(x11, selection, owner_client);

    /* Flush output buffers and consume any pending events */
    vdagent_x11_do_read(x11);
}

void vdagent_x11_clipboard_data(struct vdagent_x11 *x11, uint8_t selection,
    uint32_t type, uint8_t *data, uint32_t size)
{
    Atom prop;
    XEvent *event;
    uint32_t type_from_event;
// =================== added for debugging | KORMULEV =====================
	syslog( LOG_INFO, "vdagent_x11_clipboard_data | vdagent-x11.c | line 1438" );
	syslog( LOG_INFO, "x11->xfixes_event_base is %d\n", x11->xfixes_event_base );
// ========================================================================
    if (x11->selection_req_data) {
        if (type || size) {
            SELPRINTF("received clipboard data while still sending"
                      " data from previous request, ignoring");
        }
        free(data);
        return;
    }

    if (!x11->selection_req) {
        if (type || size) {
            SELPRINTF("received clipboard data without an "
                      "outstanding selection request, ignoring");
        }
        free(data);
        return;
    }

    event = &x11->selection_req->event;
    type_from_event = vdagent_x11_target_to_type(x11, 
                                             x11->selection_req->selection,
                                             event->xselectionrequest.target);
    if (type_from_event != type ||
            selection != x11->selection_req->selection) {
        if (selection != x11->selection_req->selection) {
            SELPRINTF("expecting data for selection %d got %d",
                      (int)x11->selection_req->selection, (int)selection);
        }
        if (type_from_event != type) {
            SELPRINTF("expecting type %u clipboard data got %u",
                      type_from_event, type);
        }
        vdagent_x11_send_selection_notify(x11, None, NULL);
        free(data);

        /* Flush output buffers and consume any pending events */
        vdagent_x11_do_read(x11);
        return;
    }

    prop = event->xselectionrequest.property;
    if (prop == None)
        prop = event->xselectionrequest.target;

    if (size > x11->max_prop_size) {
        unsigned long len = size;
        VSELPRINTF("Starting incr send of clipboard data");

        vdagent_x11_set_error_handler(x11, vdagent_x11_ignore_bad_window_handler);
        XSelectInput(x11->display, event->xselectionrequest.requestor,
                     PropertyChangeMask);
        XChangeProperty(x11->display, event->xselectionrequest.requestor, prop,
                        x11->incr_atom, 32, PropModeReplace,
                        (unsigned char*)&len, 1);
        if (vdagent_x11_restore_error_handler(x11) == 0) {
            x11->selection_req_data = data;
            x11->selection_req_data_pos = 0;
            x11->selection_req_data_size = size;
            x11->selection_req_atom = prop;
            vdagent_x11_send_selection_notify(x11, prop, x11->selection_req);
        } else {
            SELPRINTF("clipboard data sent failed, requestor window gone");
            free(data);
        }
    } else {
        vdagent_x11_set_error_handler(x11, vdagent_x11_ignore_bad_window_handler);
        XChangeProperty(x11->display, event->xselectionrequest.requestor, prop,
                        event->xselectionrequest.target, 8, PropModeReplace,
                        data, size);
        if (vdagent_x11_restore_error_handler(x11) == 0)
            vdagent_x11_send_selection_notify(x11, prop, NULL);
        else
            SELPRINTF("clipboard data sent failed, requestor window gone");

        free(data);
    }

    /* Flush output buffers and consume any pending events */
    vdagent_x11_do_read(x11);
}

void vdagent_x11_clipboard_release(struct vdagent_x11 *x11, uint8_t selection)
{
    XEvent event;
    Atom clip = None;

    if (vdagent_x11_get_clipboard_atom(x11, selection, &clip)) {
        return;
    }

    if (x11->clipboard_owner[selection] != owner_client) {
        SELPRINTF("received release while not owning client clipboard");
        return;
    }

    XSetSelectionOwner(x11->display, clip, None, CurrentTime);
    /* Make sure we process the XFixesSetSelectionOwnerNotify event caused
       by this, so we don't end up changing the clipboard owner to none, after
       it has already been re-owned because this event is still pending. */
    XSync(x11->display, False);
// =================== added for debugging | KORMULEV =====================
	syslog( LOG_INFO, "vdagent_x11_clipboard_release | vdagent-x11.c | line 1519" );
	syslog( LOG_INFO, "x11->xfixes_event_base is %d\n", x11->xfixes_event_base );
	syslog( LOG_INFO, "event.type is %d\n", event.type );
// ========================================================================
    while (XCheckTypedEvent(x11->display, x11->xfixes_event_base,
                            &event))
        vdagent_x11_handle_event(x11, event);

    /* Note no need to do a set_clipboard_owner(owner_none) here, as that is
       already done by processing the XFixesSetSelectionOwnerNotify event. */

    /* Flush output buffers and consume any pending events */
    vdagent_x11_do_read(x11);
}

void vdagent_x11_client_disconnected(struct vdagent_x11 *x11)
{
    int sel;

    for (sel = 0; sel < VD_AGENT_CLIPBOARD_SELECTION_SECONDARY; sel++) {
        if (x11->clipboard_owner[sel] == owner_client)
            vdagent_x11_clipboard_release(x11, sel);
    }
}

/* Function used to determine the default location to save file-xfers,
   xdg desktop dir or xdg download dir. We error on the save side and use a
   whitelist approach, so any unknown desktops will end up with saving
   file-xfers to the xdg download dir, and opening the xdg download dir with
   xdg-open when the file-xfer completes. */
int vdagent_x11_has_icons_on_desktop(struct vdagent_x11 *x11)
{
    const char * const wms_with_icons_on_desktop[] = {
        "Metacity", /* GNOME-2 or GNOME-3 fallback */
        "Xfwm4",    /* XFCE */
        "Marco",    /* Mate */
        NULL
    };
    int i;

    if (x11->net_wm_name)
        for (i = 0; wms_with_icons_on_desktop[i]; i++)
            if (!strcmp(x11->net_wm_name, wms_with_icons_on_desktop[i]))
                return 1;

    return 0;
}
