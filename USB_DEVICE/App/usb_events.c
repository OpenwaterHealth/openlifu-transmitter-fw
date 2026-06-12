#include "usb_events.h"

/* One slot per event — indexed by usb_event_t. */
static usb_event_cb_t cb_table[USB_EVENT_COUNT] = { 0 };

/* Tracked connection state — updated by usb_notify(). */
static volatile uint8_t _usb_connected = 0;
static volatile uint8_t _usb_port_open = 0;

void usb_register_callback(usb_event_t event, usb_event_cb_t cb)
{
    if (event < USB_EVENT_COUNT)
    {
        cb_table[event] = cb;
    }
}

void usb_notify(usb_event_t event)
{
    /* Update internal state flags. */
    switch (event)
    {
        case USB_EVENT_CONNECT:     _usb_connected = 1; break;
        case USB_EVENT_DISCONNECT:  _usb_connected = 0; _usb_port_open = 0; break;
        case USB_EVENT_PORT_OPEN:   _usb_port_open = 1; break;
        case USB_EVENT_PORT_CLOSE:  _usb_port_open = 0; break;
        default: break;
    }

    if (event < USB_EVENT_COUNT && cb_table[event])
    {
        cb_table[event](event);
    }
}

uint8_t usb_is_connected(void)
{
    return _usb_connected;
}

uint8_t usb_is_port_open(void)
{
    return _usb_port_open;
}
