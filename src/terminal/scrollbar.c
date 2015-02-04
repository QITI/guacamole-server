/*
 * Copyright (C) 2015 Glyptodon LLC
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "config.h"
#include "scrollbar.h"

#include <guacamole/client.h>
#include <guacamole/layer.h>
#include <guacamole/socket.h>
#include <guacamole/protocol.h>

#include <stdlib.h>

guac_terminal_scrollbar* guac_terminal_scrollbar_alloc(guac_client* client,
        const guac_layer* parent, int parent_width, int parent_height, int visible_area) {

    /* Allocate scrollbar */
    guac_terminal_scrollbar* scrollbar =
        malloc(sizeof(guac_terminal_scrollbar));

    /* Associate client */
    scrollbar->client = client;

    /* Init default min/max and value */
    scrollbar->min   = 0;
    scrollbar->max   = 0;
    scrollbar->value = 0;

    /* Init parent data */
    scrollbar->parent        = parent;
    scrollbar->parent_width  = 0;
    scrollbar->parent_height = 0;
    scrollbar->visible_area  = 0;

    /* Init handle render state */
    scrollbar->render_state.handle_x      = 0;
    scrollbar->render_state.handle_y      = 0;
    scrollbar->render_state.handle_width  = 0;
    scrollbar->render_state.handle_height = 0;

    /* Init container render state */
    scrollbar->render_state.container_x      = 0;
    scrollbar->render_state.container_y      = 0;
    scrollbar->render_state.container_width  = 0;
    scrollbar->render_state.container_height = 0;

    /* Allocate and init layers */
    scrollbar->container = guac_client_alloc_layer(client);
    scrollbar->handle    = guac_client_alloc_layer(client);

    /* Init mouse event state tracking */
    scrollbar->dragging_handle = 0;

    /* Reposition and resize to fit parent */
    guac_terminal_scrollbar_parent_resized(scrollbar,
            parent_width, parent_height, visible_area);

    return scrollbar;

}

void guac_terminal_scrollbar_free(guac_terminal_scrollbar* scrollbar) {

    /* Free layers */
    guac_client_free_layer(scrollbar->client, scrollbar->handle);
    guac_client_free_layer(scrollbar->client, scrollbar->container);

    /* Free scrollbar */
    free(scrollbar);

}

/**
 * Calculates the state of the scroll bar, given its minimum, maximum, current
 * values, and the state of any dragging operation. The resulting render state
 * will not be reflected graphically unless the scrollbar is flushed, and any
 * resulting value will not be assigned to the scrollbar unless explicitly set
 * with guac_terminal_scrollbar_set_value().
 *
 * @param scrollbar
 *     The scrollbar whose state should be calculated.
 *
 * @param render_state
 *     A pointer to an existing guac_terminal_scrollbar_render_state that will
 *     be populated with the calculated result.
 *
 * @param value
 *     A pointer to an existing int that will be populated with the updated
 *     scrollbar value.
 */
static void calculate_state(guac_terminal_scrollbar* scrollbar,
        guac_terminal_scrollbar_render_state* render_state,
        int* value) {

    /* Use unchanged current value by default */
    *value = scrollbar->value;

    /* Calculate container dimensions */
    render_state->container_width  = GUAC_TERMINAL_SCROLLBAR_WIDTH;
    render_state->container_height = scrollbar->parent_height;

    /* Calculate container position */
    render_state->container_x = scrollbar->parent_width
                              - render_state->container_width;

    render_state->container_y = 0;

    /* Calculate handle dimensions */
    render_state->handle_width  = render_state->container_width
                                - GUAC_TERMINAL_SCROLLBAR_PADDING*2;

    /* Handle can be no bigger than the scrollbar itself */
    int max_handle_height = render_state->container_height
                          - GUAC_TERMINAL_SCROLLBAR_PADDING*2;

    /* Calculate legal delta between scroll values */
    int scroll_delta;
    if (scrollbar->max > scrollbar->min)
        scroll_delta = scrollbar->max - scrollbar->min;
    else
        scroll_delta = 0;

    /* Scale handle relative to visible area vs. scrolling region size */
    int proportional_height = max_handle_height
                            * scrollbar->visible_area
                            / (scroll_delta + scrollbar->visible_area);

    /* Ensure handle is no smaller than minimum height */
    if (proportional_height > GUAC_TERMINAL_SCROLLBAR_MIN_HEIGHT)
        render_state->handle_height = proportional_height;
    else
        render_state->handle_height = GUAC_TERMINAL_SCROLLBAR_MIN_HEIGHT;

    /* Ensure handle is no larger than maximum height */
    if (render_state->handle_height > max_handle_height)
        render_state->handle_height = max_handle_height;

    /* Calculate handle X position */
    render_state->handle_x = GUAC_TERMINAL_SCROLLBAR_PADDING;

    /* Calculate handle Y range */
    int min_handle_y = GUAC_TERMINAL_SCROLLBAR_PADDING;
    int max_handle_y = min_handle_y + max_handle_height
                     - render_state->handle_height;

    /* Position handle relative to mouse if being dragged */
    if (scrollbar->dragging_handle) {

        int dragged_handle_y = scrollbar->drag_current_y
                             - scrollbar->drag_offset_y;

        /* Keep handle within bounds */
        if (dragged_handle_y < min_handle_y)
            dragged_handle_y = min_handle_y;
        else if (dragged_handle_y > max_handle_y)
            dragged_handle_y = max_handle_y;

        render_state->handle_y = dragged_handle_y;

        /* Calculate scrollbar value */
        if (max_handle_y > min_handle_y) {
            *value = scrollbar->min
                   + (dragged_handle_y - min_handle_y)
                      * scroll_delta
                      / (max_handle_y - min_handle_y);
        }

    }

    /* Handle Y position is relative to current scroll value */
    else if (scroll_delta > 0)
        render_state->handle_y = min_handle_y
                               + (max_handle_y - min_handle_y)
                                  * (scrollbar->value - scrollbar->min)
                                  / scroll_delta;

    /* ... unless there is only one possible scroll value */
    else
        render_state->handle_y = GUAC_TERMINAL_SCROLLBAR_PADDING;

}

void guac_terminal_scrollbar_flush(guac_terminal_scrollbar* scrollbar) {

    guac_socket* socket = scrollbar->client->socket;

    /* Get old state */
    int old_value = scrollbar->value;
    guac_terminal_scrollbar_render_state* old_state = &scrollbar->render_state;

    /* Calculate new state */
    int new_value;
    guac_terminal_scrollbar_render_state new_state;
    calculate_state(scrollbar, &new_state, &new_value);

    /* Notify of scroll if value is changing */
    if (new_value != old_value) {

        /* TODO: Call scroll value handler */
        guac_client_log(scrollbar->client, GUAC_LOG_DEBUG,
                "SCROLL: min=%i max=%i value=%i",
                scrollbar->min, scrollbar->max, new_value);

    }

    /* Reposition container if moved */
    if (old_state->container_x != new_state.container_x
     || old_state->container_y != new_state.container_y) {

        guac_protocol_send_move(socket,
                scrollbar->container, scrollbar->parent,
                new_state.container_x,
                new_state.container_y,
                0);

    }

    /* Resize and redraw container if size changed */
    if (old_state->container_width  != new_state.container_width
     || old_state->container_height != new_state.container_height) {

        /* Set new size */
        guac_protocol_send_size(socket, scrollbar->container,
                new_state.container_width,
                new_state.container_height);

        /* Fill container with solid color */
        guac_protocol_send_rect(socket, scrollbar->container, 0, 0,
                new_state.container_width,
                new_state.container_height);

        guac_protocol_send_cfill(socket, GUAC_COMP_SRC, scrollbar->container,
                0x40, 0x40, 0x40, 0xFF);

    }

    /* Reposition handle if moved */
    if (old_state->handle_x != new_state.handle_x
     || old_state->handle_y != new_state.handle_y) {

        guac_protocol_send_move(socket,
                scrollbar->handle, scrollbar->container,
                new_state.handle_x,
                new_state.handle_y,
                0);

    }

    /* Resize and redraw handle if size changed */
    if (old_state->handle_width  != new_state.handle_width
     || old_state->handle_height != new_state.handle_height) {

        /* Send new size */
        guac_protocol_send_size(socket, scrollbar->handle,
                new_state.handle_width,
                new_state.handle_height);

        /* Fill and stroke handle with solid color */
        guac_protocol_send_rect(socket, scrollbar->handle, 0, 0,
                new_state.handle_width,
                new_state.handle_height);

        guac_protocol_send_cfill(socket, GUAC_COMP_SRC, scrollbar->handle,
                0x80, 0x80, 0x80, 0xFF);

        guac_protocol_send_cstroke(socket, GUAC_COMP_OVER, scrollbar->handle,
                GUAC_LINE_CAP_SQUARE, GUAC_LINE_JOIN_MITER, 2,
                0xA0, 0xA0, 0xA0, 0xFF);

    }

    /* Store current render state */
    scrollbar->render_state = new_state;

}

void guac_terminal_scrollbar_set_bounds(guac_terminal_scrollbar* scrollbar,
        int min, int max) {

    /* Fit value within bounds */
    if (scrollbar->value > max)
        scrollbar->value = max;
    else if (scrollbar->value < min)
        scrollbar->value = min;

    /* Update bounds */
    scrollbar->min = min;
    scrollbar->max = max;

}

void guac_terminal_scrollbar_set_value(guac_terminal_scrollbar* scrollbar,
        int value) {

    /* Fit value within bounds */
    if (value > scrollbar->max)
        value = scrollbar->max;
    else if (value < scrollbar->min)
        value = scrollbar->min;

    /* Update value */
    scrollbar->value = value;

}

void guac_terminal_scrollbar_parent_resized(guac_terminal_scrollbar* scrollbar,
        int parent_width, int parent_height, int visible_area) {

    /* Assign new dimensions */
    scrollbar->parent_width  = parent_width;
    scrollbar->parent_height = parent_height;
    scrollbar->visible_area  = visible_area;

}

int guac_terminal_scrollbar_handle_mouse(guac_terminal_scrollbar* scrollbar,
        int x, int y, int mask) {

    /* Get container rectangle bounds */
    int parent_left   = scrollbar->render_state.container_x;
    int parent_top    = scrollbar->render_state.container_y;
    int parent_right  = parent_left + scrollbar->render_state.container_width;
    int parent_bottom = parent_top  + scrollbar->render_state.container_height;

    /* Calculate handle rectangle bounds */
    int handle_left   = parent_left + scrollbar->render_state.handle_x;
    int handle_top    = parent_top  + scrollbar->render_state.handle_y;
    int handle_right  = handle_left + scrollbar->render_state.handle_width;
    int handle_bottom = handle_top  + scrollbar->render_state.handle_height;

    /* Handle click on handle */
    if (scrollbar->dragging_handle) {

        /* Update drag while mouse button is held */
        if (mask & GUAC_CLIENT_MOUSE_LEFT)
            scrollbar->drag_current_y = y;

        /* Stop drag if mouse button is released */
        else
            scrollbar->dragging_handle = 0;

        /* Mouse event was handled by scrollbar */
        return 1;

    }
    else if (mask == GUAC_CLIENT_MOUSE_LEFT
            && x >= handle_left && x < handle_right
            && y >= handle_top  && y < handle_bottom) {

        /* Start drag */
        scrollbar->dragging_handle = 1;
        scrollbar->drag_offset_y = y - handle_top;
        scrollbar->drag_current_y = y;

        /* Mouse event was handled by scrollbar */
        return 1;

    }

    /* Eat any events that occur within the scrollbar */
    return x >= parent_left && x < parent_right
        && y >= parent_top  && y < parent_bottom;

}

