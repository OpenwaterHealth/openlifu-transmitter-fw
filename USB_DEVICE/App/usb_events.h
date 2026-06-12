#ifndef USB_EVENTS_H
#define USB_EVENTS_H

#include <stdint.h>

/**
 * @brief USB event identifiers.
 *
 * To add a new event:
 *   1. Insert a new entry before USB_EVENT_COUNT.
 *   2. Call usb_notify(USB_EVENT_YOUR_NEW_EVENT) at the appropriate site.
 *   No other changes to this module are required.
 */
typedef enum
{
    USB_EVENT_CONNECT,      /**< USB cable attached / device enumerated       */
    USB_EVENT_DISCONNECT,   /**< USB cable detached / device suspended        */
    USB_EVENT_PORT_OPEN,    /**< Host opened the VCP (DTR asserted)           */
    USB_EVENT_PORT_CLOSE,   /**< Host closed the VCP (DTR de-asserted)        */
    /* ----- add new events above this line --------------------------------- */
    USB_EVENT_COUNT         /**< Sentinel — do not use as an event            */
} usb_event_t;

/** Callback signature for all USB events. */
typedef void (*usb_event_cb_t)(usb_event_t event);

/**
 * @brief  Register a callback for a specific USB event.
 * @param  event  The event to listen for.
 * @param  cb     Callback function (pass NULL to unregister).
 */
void usb_register_callback(usb_event_t event, usb_event_cb_t cb);

/**
 * @brief  Fire a USB event, invoking its registered callback (if any).
 * @param  event  The event that occurred.
 */
void usb_notify(usb_event_t event);

/**
 * @brief  Query whether the USB cable is connected (device enumerated).
 * @retval 1 if connected, 0 otherwise.
 */
uint8_t usb_is_connected(void);

/**
 * @brief  Query whether the host has opened the VCP (DTR asserted).
 * @retval 1 if port is open, 0 otherwise.
 */
uint8_t usb_is_port_open(void);

#endif /* USB_EVENTS_H */
