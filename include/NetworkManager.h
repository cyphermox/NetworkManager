/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/* NetworkManager -- Network link manager
 *
 * Dan Williams <dcbw@redhat.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * (C) Copyright 2004 - 2011 Red Hat, Inc.
 */

#ifndef NETWORK_MANAGER_H
#define NETWORK_MANAGER_H

/*
 * dbus services details
 */
#define	NM_DBUS_SERVICE                     "org.freedesktop.NetworkManager"

#define	NM_DBUS_PATH                        "/org/freedesktop/NetworkManager"
#define	NM_DBUS_INTERFACE                   "org.freedesktop.NetworkManager"
#define	NM_DBUS_INTERFACE_DEVICE            NM_DBUS_INTERFACE ".Device"
#define NM_DBUS_INTERFACE_DEVICE_WIRED      NM_DBUS_INTERFACE_DEVICE ".Wired"
#define NM_DBUS_INTERFACE_DEVICE_WIRELESS   NM_DBUS_INTERFACE_DEVICE ".Wireless"
#define NM_DBUS_INTERFACE_DEVICE_BLUETOOTH  NM_DBUS_INTERFACE_DEVICE ".Bluetooth"
#define NM_DBUS_PATH_ACCESS_POINT           NM_DBUS_PATH "/AccessPoint"
#define NM_DBUS_INTERFACE_ACCESS_POINT      NM_DBUS_INTERFACE ".AccessPoint"
#define NM_DBUS_INTERFACE_SERIAL_DEVICE     NM_DBUS_INTERFACE_DEVICE ".Serial"
#define NM_DBUS_INTERFACE_GSM_DEVICE        NM_DBUS_INTERFACE_DEVICE ".Gsm"
#define NM_DBUS_INTERFACE_CDMA_DEVICE       NM_DBUS_INTERFACE_DEVICE ".Cdma"
#define NM_DBUS_INTERFACE_DEVICE_WIMAX      NM_DBUS_INTERFACE_DEVICE ".WiMax"
#define NM_DBUS_INTERFACE_WIMAX_NSP         NM_DBUS_INTERFACE ".WiMax.Nsp"
#define NM_DBUS_PATH_WIMAX_NSP              NM_DBUS_PATH "/Nsp"
#define NM_DBUS_INTERFACE_ACTIVE_CONNECTION NM_DBUS_INTERFACE ".Connection.Active"
#define NM_DBUS_INTERFACE_IP4_CONFIG        NM_DBUS_INTERFACE ".IP4Config"
#define NM_DBUS_INTERFACE_DHCP4_CONFIG      NM_DBUS_INTERFACE ".DHCP4Config"
#define NM_DBUS_INTERFACE_IP6_CONFIG        NM_DBUS_INTERFACE ".IP6Config"
#define NM_DBUS_INTERFACE_DHCP6_CONFIG      NM_DBUS_INTERFACE ".DHCP6Config"


#define NM_DBUS_IFACE_SETTINGS            "org.freedesktop.NetworkManager.Settings"
#define NM_DBUS_PATH_SETTINGS             "/org/freedesktop/NetworkManager/Settings"

#define NM_DBUS_IFACE_SETTINGS_CONNECTION "org.freedesktop.NetworkManager.Settings.Connection"
#define NM_DBUS_PATH_SETTINGS_CONNECTION  "/org/freedesktop/NetworkManager/Settings/Connection"
#define NM_DBUS_IFACE_SETTINGS_CONNECTION_SECRETS "org.freedesktop.NetworkManager.Settings.Connection.Secrets"

#define NM_DBUS_INTERFACE_AGENT_MANAGER   NM_DBUS_INTERFACE ".AgentManager"
#define NM_DBUS_PATH_AGENT_MANAGER        "/org/freedesktop/NetworkManager/AgentManager"

#define NM_DBUS_INTERFACE_SECRET_AGENT    NM_DBUS_INTERFACE ".SecretAgent"
#define NM_DBUS_PATH_SECRET_AGENT         "/org/freedesktop/NetworkManager/SecretAgent"

/* General NetworkManager state */
typedef enum {
	NM_STATE_UNKNOWN = 0,
	NM_STATE_ASLEEP,
	NM_STATE_CONNECTING,
	NM_STATE_CONNECTED,
	NM_STATE_DISCONNECTED
} NMState;

/* Types of NetworkManager devices */
typedef enum {
	NM_DEVICE_TYPE_UNKNOWN   = 0,
	NM_DEVICE_TYPE_ETHERNET  = 1,
	NM_DEVICE_TYPE_WIFI      = 2,
	NM_DEVICE_TYPE_GSM       = 3,
	NM_DEVICE_TYPE_CDMA      = 4,
	NM_DEVICE_TYPE_BT        = 5,  /* Bluetooth */
	NM_DEVICE_TYPE_OLPC_MESH = 6,
	NM_DEVICE_TYPE_WIMAX     = 7
} NMDeviceType;

/* General device capability flags */
typedef enum {
	NM_DEVICE_CAP_NONE           = 0x00000000,
	NM_DEVICE_CAP_NM_SUPPORTED   = 0x00000001,
	NM_DEVICE_CAP_CARRIER_DETECT = 0x00000002
} NMDeviceCapabilities;


/* 802.11 Wifi device capabilities */
typedef enum {
	NM_WIFI_DEVICE_CAP_NONE          = 0x00000000,
	NM_WIFI_DEVICE_CAP_CIPHER_WEP40  = 0x00000001,
	NM_WIFI_DEVICE_CAP_CIPHER_WEP104 = 0x00000002,
	NM_WIFI_DEVICE_CAP_CIPHER_TKIP   = 0x00000004,
	NM_WIFI_DEVICE_CAP_CIPHER_CCMP   = 0x00000008,
	NM_WIFI_DEVICE_CAP_WPA           = 0x00000010,
	NM_WIFI_DEVICE_CAP_RSN           = 0x00000020
} NMDeviceWifiCapabilities;


/* 802.11 Access Point flags */
typedef enum {
	NM_802_11_AP_FLAGS_NONE    = 0x00000000,
	NM_802_11_AP_FLAGS_PRIVACY = 0x00000001
} NM80211ApFlags;

/*
 * 802.11 Access Point security flags
 *
 * These describe the current security requirements of the BSSID as extracted
 * from various pieces of beacon information, like beacon flags and various
 * information elements.
 */
typedef enum {
	NM_802_11_AP_SEC_NONE            = 0x00000000,
	NM_802_11_AP_SEC_PAIR_WEP40      = 0x00000001,
	NM_802_11_AP_SEC_PAIR_WEP104     = 0x00000002,
	NM_802_11_AP_SEC_PAIR_TKIP       = 0x00000004,
	NM_802_11_AP_SEC_PAIR_CCMP       = 0x00000008,
	NM_802_11_AP_SEC_GROUP_WEP40     = 0x00000010,
	NM_802_11_AP_SEC_GROUP_WEP104    = 0x00000020,
	NM_802_11_AP_SEC_GROUP_TKIP      = 0x00000040,
	NM_802_11_AP_SEC_GROUP_CCMP      = 0x00000080,
	NM_802_11_AP_SEC_KEY_MGMT_PSK    = 0x00000100,
	NM_802_11_AP_SEC_KEY_MGMT_802_1X = 0x00000200
} NM80211ApSecurityFlags;

/*
 * 802.11 AP and Station modes
 *
 */
typedef enum {
	NM_802_11_MODE_UNKNOWN = 0,
	NM_802_11_MODE_ADHOC,
	NM_802_11_MODE_INFRA
} NM80211Mode;

/**
 * NMBluetoothCapabilities:
 * @NM_BT_CAPABILITY_NONE: device has no usable capabilities
 * @NM_BT_CAPABILITY_DUN: device provides Dial-Up Networking capability
 * @NM_BT_CAPABILITY_NAP: device provides Network Access Point capability
 *
 * #NMBluetoothCapabilities values indicate the usable capabilities of a
 * Bluetooth device.
 */
typedef enum {
	NM_BT_CAPABILITY_NONE = 0x00000000,
	NM_BT_CAPABILITY_DUN  = 0x00000001,
	NM_BT_CAPABILITY_NAP  = 0x00000002,
} NMBluetoothCapabilities;


/*
 * Device states
 */
typedef enum {
	NM_DEVICE_STATE_UNKNOWN = 0,

	/* Initial state of all devices and the only state for devices not
	 * managed by NetworkManager.
	 *
	 * Allowed next states:
	 *   UNAVAILABLE:  the device is now managed by NetworkManager
	 */
	NM_DEVICE_STATE_UNMANAGED = 1,

	/* Indicates the device is not yet ready for use, but is managed by
	 * NetworkManager.  For Ethernet devices, the device may not have an
	 * active carrier.  For WiFi devices, the device may not have it's radio
	 * enabled.
	 *
	 * Allowed next states:
	 *   UNMANAGED:  the device is no longer managed by NetworkManager
	 *   DISCONNECTED:  the device is now ready for use
	 */
	NM_DEVICE_STATE_UNAVAILABLE = 2,

	/* Indicates the device does not have an activate connection to anything.
	 *
	 * Allowed next states:
	 *   UNMANAGED:  the device is no longer managed by NetworkManager
	 *   UNAVAILABLE:  the device is no longer ready for use (rfkill, no carrier, etc)
	 *   PREPARE:  the device has started activation
	 */
	NM_DEVICE_STATE_DISCONNECTED = 3,

	/* Indicate states in device activation.
	 *
	 * Allowed next states:
	 *   UNMANAGED:  the device is no longer managed by NetworkManager
	 *   UNAVAILABLE:  the device is no longer ready for use (rfkill, no carrier, etc)
	 *   FAILED:  an error ocurred during activation
	 *   NEED_AUTH:  authentication/secrets are needed
	 *   ACTIVATED:  (IP_CONFIG only) activation was successful
	 *   DISCONNECTED:  the device's connection is no longer valid, or NetworkManager went to sleep
	 */
	NM_DEVICE_STATE_PREPARE = 4,
	NM_DEVICE_STATE_CONFIG = 5,
	NM_DEVICE_STATE_NEED_AUTH = 6,
	NM_DEVICE_STATE_IP_CONFIG = 7,

	/* Indicates the device is part of an active network connection.
	 *
	 * Allowed next states:
	 *   UNMANAGED:  the device is no longer managed by NetworkManager
	 *   UNAVAILABLE:  the device is no longer ready for use (rfkill, no carrier, etc)
	 *   FAILED:  a DHCP lease was not renewed, or another error
	 *   DISCONNECTED:  the device's connection is no longer valid, or NetworkManager went to sleep
	 */
	NM_DEVICE_STATE_ACTIVATED = 8,

	/* Indicates the device's activation failed.
	 *
	 * Allowed next states:
	 *   UNMANAGED:  the device is no longer managed by NetworkManager
	 *   UNAVAILABLE:  the device is no longer ready for use (rfkill, no carrier, etc)
	 *   DISCONNECTED:  the device's connection is ready for activation, or NetworkManager went to sleep
	 */
	NM_DEVICE_STATE_FAILED = 9
} NMDeviceState;


/*
 * Device state change reason codes
 */
typedef enum {
	/* No reason given */
	NM_DEVICE_STATE_REASON_NONE = 0,

	/* Unknown error */
	NM_DEVICE_STATE_REASON_UNKNOWN = 1,

	/* Device is now managed */
	NM_DEVICE_STATE_REASON_NOW_MANAGED = 2,

	/* Device is now unmanaged */
	NM_DEVICE_STATE_REASON_NOW_UNMANAGED = 3,

	/* The device could not be readied for configuration */
	NM_DEVICE_STATE_REASON_CONFIG_FAILED = 4,

	/* IP configuration could not be reserved (no available address, timeout, etc) */
	NM_DEVICE_STATE_REASON_IP_CONFIG_UNAVAILABLE = 5,

	/* The IP config is no longer valid */
	NM_DEVICE_STATE_REASON_IP_CONFIG_EXPIRED = 6,

	/* Secrets were required, but not provided */
	NM_DEVICE_STATE_REASON_NO_SECRETS = 7,

	/* 802.1x supplicant disconnected */
	NM_DEVICE_STATE_REASON_SUPPLICANT_DISCONNECT = 8,

	/* 802.1x supplicant configuration failed */
	NM_DEVICE_STATE_REASON_SUPPLICANT_CONFIG_FAILED = 9,

	/* 802.1x supplicant failed */
	NM_DEVICE_STATE_REASON_SUPPLICANT_FAILED = 10,

	/* 802.1x supplicant took too long to authenticate */
	NM_DEVICE_STATE_REASON_SUPPLICANT_TIMEOUT = 11,

	/* PPP service failed to start */
	NM_DEVICE_STATE_REASON_PPP_START_FAILED = 12,

	/* PPP service disconnected */
	NM_DEVICE_STATE_REASON_PPP_DISCONNECT = 13,

	/* PPP failed */
	NM_DEVICE_STATE_REASON_PPP_FAILED = 14,

	/* DHCP client failed to start */
	NM_DEVICE_STATE_REASON_DHCP_START_FAILED = 15,

	/* DHCP client error */
	NM_DEVICE_STATE_REASON_DHCP_ERROR = 16,

	/* DHCP client failed */
	NM_DEVICE_STATE_REASON_DHCP_FAILED = 17,

	/* Shared connection service failed to start */
	NM_DEVICE_STATE_REASON_SHARED_START_FAILED = 18,

	/* Shared connection service failed */
	NM_DEVICE_STATE_REASON_SHARED_FAILED = 19,

	/* AutoIP service failed to start */
	NM_DEVICE_STATE_REASON_AUTOIP_START_FAILED = 20,

	/* AutoIP service error */
	NM_DEVICE_STATE_REASON_AUTOIP_ERROR = 21,

	/* AutoIP service failed */
	NM_DEVICE_STATE_REASON_AUTOIP_FAILED = 22,

	/* The line is busy */
	NM_DEVICE_STATE_REASON_MODEM_BUSY = 23,

	/* No dial tone */
	NM_DEVICE_STATE_REASON_MODEM_NO_DIAL_TONE = 24,

	/* No carrier could be established */
	NM_DEVICE_STATE_REASON_MODEM_NO_CARRIER = 25,

	/* The dialing request timed out */
	NM_DEVICE_STATE_REASON_MODEM_DIAL_TIMEOUT = 26,

	/* The dialing attempt failed */
	NM_DEVICE_STATE_REASON_MODEM_DIAL_FAILED = 27,

	/* Modem initialization failed */
	NM_DEVICE_STATE_REASON_MODEM_INIT_FAILED = 28,

	/* Failed to select the specified APN */
	NM_DEVICE_STATE_REASON_GSM_APN_FAILED = 29,

	/* Not searching for networks */
	NM_DEVICE_STATE_REASON_GSM_REGISTRATION_NOT_SEARCHING = 30,

	/* Network registration denied */
	NM_DEVICE_STATE_REASON_GSM_REGISTRATION_DENIED = 31,

	/* Network registration timed out */
	NM_DEVICE_STATE_REASON_GSM_REGISTRATION_TIMEOUT = 32,

	/* Failed to register with the requested network */
	NM_DEVICE_STATE_REASON_GSM_REGISTRATION_FAILED = 33,

	/* PIN check failed */
	NM_DEVICE_STATE_REASON_GSM_PIN_CHECK_FAILED = 34,

	/* Necessary firmware for the device may be missing */
	NM_DEVICE_STATE_REASON_FIRMWARE_MISSING = 35,

	/* The device was removed */
	NM_DEVICE_STATE_REASON_REMOVED = 36,

	/* NetworkManager went to sleep */
	NM_DEVICE_STATE_REASON_SLEEPING = 37,

	/* The device's active connection disappeared */
	NM_DEVICE_STATE_REASON_CONNECTION_REMOVED = 38,

	/* Device disconnected by user or client */
	NM_DEVICE_STATE_REASON_USER_REQUESTED = 39,

	/* Carrier/link changed */
	NM_DEVICE_STATE_REASON_CARRIER = 40,

	/* The device's existing connection was assumed */
	NM_DEVICE_STATE_REASON_CONNECTION_ASSUMED = 41,

	/* The supplicant is now available */
	NM_DEVICE_STATE_REASON_SUPPLICANT_AVAILABLE = 42,

	/* The modem could not be found */
	NM_DEVICE_STATE_REASON_MODEM_NOT_FOUND = 43,

	/* The Bluetooth connection failed or timed out */
	NM_DEVICE_STATE_REASON_BT_FAILED = 44,

	/* Unused */
	NM_DEVICE_STATE_REASON_LAST = 0xFFFF
} NMDeviceStateReason;


typedef enum {
	NM_ACTIVE_CONNECTION_STATE_UNKNOWN = 0,

	/* Indicates the connection is activating */
	NM_ACTIVE_CONNECTION_STATE_ACTIVATING,

	/* Indicates the connection is currently active */
	NM_ACTIVE_CONNECTION_STATE_ACTIVATED
} NMActiveConnectionState;

#endif /* NETWORK_MANAGER_H */

