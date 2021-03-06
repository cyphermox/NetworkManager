// SPDX-License-Identifier: GPL-2.0+
/* NetworkManager -- Network link manager
 *
 * Copyright 2012 Red Hat, Inc.
 */

#ifndef __NETWORKMANAGER_DEVICE_BRIDGE_H__
#define __NETWORKMANAGER_DEVICE_BRIDGE_H__

#include "nm-device.h"

#define NM_TYPE_DEVICE_BRIDGE            (nm_device_bridge_get_type ())
#define NM_DEVICE_BRIDGE(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), NM_TYPE_DEVICE_BRIDGE, NMDeviceBridge))
#define NM_DEVICE_BRIDGE_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass),  NM_TYPE_DEVICE_BRIDGE, NMDeviceBridgeClass))
#define NM_IS_DEVICE_BRIDGE(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), NM_TYPE_DEVICE_BRIDGE))
#define NM_IS_DEVICE_BRIDGE_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass),  NM_TYPE_DEVICE_BRIDGE))
#define NM_DEVICE_BRIDGE_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj),  NM_TYPE_DEVICE_BRIDGE, NMDeviceBridgeClass))

typedef struct _NMDeviceBridge NMDeviceBridge;
typedef struct _NMDeviceBridgeClass NMDeviceBridgeClass;

GType nm_device_bridge_get_type (void);

extern const NMBtVTableNetworkServer *nm_bt_vtable_network_server;

#endif /* __NETWORKMANAGER_DEVICE_BRIDGE_H__ */
