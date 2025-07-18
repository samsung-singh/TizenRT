/****************************************************************************
 *
 * Copyright 2017 Samsung Electronics All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
 * either express or implied. See the License for the specific
 * language governing permissions and limitations under the License.
 *
 ****************************************************************************/
#include <tinyara/config.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <wifi_manager/wifi_manager.h>
#include <tinyara/net/netlog.h>
#include <tinyara/net/if/wifi.h>
#include "wifi_manager_utils.h"
#include "wifi_manager_profile.h"
#include "wifi_manager_dhcp.h"
#include "wifi_manager_stats.h"
#include "wifi_manager_event.h"
#include "wifi_manager_msghandler.h"
#include "wifi_manager_error.h"
#include "wifi_manager_cb.h"
#include "wifi_manager_state.h"
#include "wifi_manager_info.h"
#include "wifi_manager_lwnl.h"

/*  Setting MACRO */
static inline void WIFIMGR_SET_SSID(char *s)
{
	wifimgr_info_msg_s winfo;
	winfo.ssid = s;
	wifimgr_set_info(WIFIMGR_SSID, &winfo);
}

static inline void WIFIMGR_SET_SOFTAP_SSID(char *s)
{
	wifimgr_info_msg_s winfo;
	winfo.softap_ssid = s;
	wifimgr_set_info(WIFIMGR_SOFTAP_SSID, &winfo);
}

/*  Copy MACRO */
#define WIFIMGR_COPY_SOFTAP_CONFIG(dest, src)                                  \
	do {                                                                       \
		(dest).channel = (src)->channel;                                       \
		strncpy((dest).ssid, (src)->ssid, WIFIMGR_SSID_LEN);                   \
		(dest).ssid[WIFIMGR_SSID_LEN] = '\0';                                  \
		strncpy((dest).passphrase, (src)->passphrase, WIFIMGR_PASSPHRASE_LEN); \
		(dest).passphrase[WIFIMGR_PASSPHRASE_LEN] = '\0';                      \
	} while (0)

#define WIFIMGR_COPY_AP_INFO(dest, src)                                       \
	do {                                                                      \
		(dest).ssid_length = (src).ssid_length;                               \
		(dest).passphrase_length = (src).passphrase_length;                   \
		strncpy((dest).ssid, (src).ssid, WIFIMGR_SSID_LEN);                   \
		(dest).ssid[WIFIMGR_SSID_LEN] = '\0';                                 \
		strncpy((dest).passphrase, (src).passphrase, WIFIMGR_PASSPHRASE_LEN); \
		(dest).passphrase[WIFIMGR_PASSPHRASE_LEN] = '\0';                     \
		(dest).ap_auth_type = (src).ap_auth_type;                             \
		(dest).ap_crypto_type = (src).ap_crypto_type;                         \
	} while (0)

/*  Initialize MACRO */
#define WM_APINFO_INITIALIZER                                         \
	{                                                                 \
		{                                                             \
			0,                                                        \
		},                                                            \
			0, {                                                      \
				   0,                                                 \
			   },                                                     \
			0, WIFI_MANAGER_AUTH_UNKNOWN, WIFI_MANAGER_CRYPTO_UNKNOWN \
	}
#define WIFIMGR_SOTFAP_CONFIG \
	{                         \
		{                     \
			0,                \
		},                    \
			{                 \
				0,            \
			},                \
			1                 \
	}

struct _wifimgr_state_handle {
	wifimgr_state_e state;
	wifimgr_state_e prev_state; // it is for returning to previous sta state after scanning is done
	// substate
	_wifimgr_disconn_substate_e disconn_substate;
	sem_t *api_sig;
	wifi_manager_softap_config_s softap_config;
};
typedef struct _wifimgr_state_handle _wifimgr_state_handle_s;

/* global variables*/
static _wifimgr_state_handle_s g_manager_info = {
	WIFIMGR_UNINITIALIZED, WIFIMGR_UNINITIALIZED, // state, prev_state
	// substate
	WIFIMGR_DISCONN_NONE, // _wifimgr_disconn_substate_e
	NULL,
	WIFIMGR_SOTFAP_CONFIG};

#if defined(CONFIG_ENABLE_HOMELYNK) && (CONFIG_ENABLE_HOMELYNK == 1)
struct _bridge_state {
	bool is_on;
	bool is_sta_connected;
	bool is_softap_after_bridge;
};
typedef struct _bridge_state _bridge_state_s;

static _bridge_state_s g_bridge_state = { false, false, false };

static inline void WIFIMGR_SET_BRIDGE_STATE_ON(void)
{
	g_bridge_state.is_on = true;
}

static inline void WIFIMGR_SET_BRIDGE_STA_CONNECTED(bool is_sta_connected)
{
	g_bridge_state.is_sta_connected = is_sta_connected;
}

static inline void WIFIMGR_SET_IS_SOFTAP_AFTER_BRIDGE(bool is_softap_after_bridge)
{
	g_bridge_state.is_softap_after_bridge = is_softap_after_bridge;
}
#endif

/* Internal functions*/
static wifi_manager_result_e _wifimgr_deinit(void);
static wifi_manager_result_e _wifimgr_run_sta(void);
static wifi_manager_result_e _wifimgr_connect_ap(wifi_manager_ap_config_s *config);
static wifi_manager_result_e _wifimgr_save_connected_config(wifi_manager_ap_config_s *config);
static wifi_manager_result_e _wifimgr_disconnect_ap(void);
static wifi_manager_result_e _wifimgr_run_softap(wifi_manager_softap_config_s *config);
static wifi_manager_result_e _wifimgr_stop_softap(void);
static wifi_manager_result_e _wifimgr_scan(wifi_manager_scan_config_s *config);
static wifi_manager_result_e _wifimgr_scan_multi_aps(wifi_manager_scan_multi_configs_s *configs);
#if defined(CONFIG_ENABLE_HOMELYNK) && (CONFIG_ENABLE_HOMELYNK == 1)
static wifi_manager_result_e _wifimgr_control_bridge(uint8_t isenable);
#endif

/* functions managing a state machine*/
#undef WIFIMGR_STATE_TABLE
#define WIFIMGR_STATE_TABLE(state, handler, str) \
	static wifi_manager_result_e handler(wifimgr_msg_s *msg);
#include "wifi_manager_state_table.h"
typedef wifi_manager_result_e (*wifimgr_handler)(wifimgr_msg_s *msg);

/* g_handler should be matched to _wifimgr_state*/
static const wifimgr_handler g_handler[] = {
#undef WIFIMGR_STATE_TABLE
#define WIFIMGR_STATE_TABLE(state, handler, str) handler,
#include "wifi_manager_state_table.h"
};

static char *wifimgr_state_str[] = {
#undef WIFIMGR_STATE_TABLE
#define WIFIMGR_STATE_TABLE(state, handler, str) str,
#include "wifi_manager_state_table.h"
	"WIFIMGR_NONE",
	"WIFIMGR_STATE_MAX",
};

/*  State MACRO */
#define WIFIMGR_CHECK_STATE(s) ((s) != g_manager_info.state)
#define WIFIMGR_IS_STATE(s) ((s) == g_manager_info.state)
#define WIFIMGR_GET_STATE g_manager_info.state
#define WIFIMGR_GET_PREVSTATE g_manager_info.prev_state
#define WIFIMGR_STORE_PREV_STATE (g_manager_info.prev_state = g_manager_info.state)
#define WIFIMGR_RESTORE_STATE                             \
	do {                                                  \
		g_manager_info.state = g_manager_info.prev_state; \
		g_manager_info.prev_state = WIFIMGR_NONE;         \
		wifimgr_info_msg_s twmsg;                         \
		twmsg.state = g_manager_info.state;               \
		wifimgr_set_info(WIFIMGR_STATE, &twmsg);          \
	} while (0)

#define TAG "[WM]"

static inline void WIFIMGR_SET_STATE(wifimgr_state_e s)
{
	g_manager_info.state = s;
	wifimgr_info_msg_s wmsg;
	wmsg.state = s;
	wifimgr_set_info(WIFIMGR_STATE, &wmsg);
}

static inline char *wifimgr_get_state_str(int state)
{
	return wifimgr_state_str[state];
}

/*  Substate MACRO */
static inline void WIFIMGR_SET_SUBSTATE(_wifimgr_disconn_substate_e state, sem_t *signal)
{
	g_manager_info.disconn_substate = state;
	g_manager_info.api_sig = signal;
}

static inline void WIFIMGR_RESET_SUBSTATE(void)
{
	g_manager_info.disconn_substate = WIFIMGR_DISCONN_NONE;
	g_manager_info.api_sig = NULL;
}

#if defined(CONFIG_ENABLE_HOMELYNK) && (CONFIG_ENABLE_HOMELYNK == 1)
static inline void WIFIMGR_RESET_BRIDGE_STATE(void)
{
	g_bridge_state.is_on = false;
	g_bridge_state.is_sta_connected = false;
	g_bridge_state.is_softap_after_bridge = false;
}
#endif

static inline void WIFIMGR_SEND_API_SIGNAL(sem_t *api_sig)
{
	/*	send signal to wifi_manager_api to notify request is done*/
	if (api_sig) {
		sem_post(api_sig);
	}
}

static void _free_scan_list(trwifi_scan_list_s *scan_list)
{
	trwifi_scan_list_s *iter = scan_list, *prev = NULL;
	while (iter) {
		prev = iter;
		iter = iter->next;
		free(prev);
	}
}

wifi_manager_result_e _wifimgr_deinit(void)
{
	WIFIMGR_CHECK_UTILRESULT(wifi_utils_deinit(), TAG, "wifi_utils_deinit fail");
	wifimgr_unregister_all();

	return WIFI_MANAGER_SUCCESS;
}

wifi_manager_result_e _wifimgr_run_sta(void)
{
	WIFIMGR_CHECK_UTILRESULT(wifi_utils_start_sta(), TAG, "Starting STA failed.");
#ifdef CONFIG_DISABLE_EXTERNAL_AUTOCONNECT
	WIFIMGR_CHECK_UTILRESULT(wifi_utils_set_autoconnect(0), TAG, "Set Autoconnect failed");
#else
	WIFIMGR_CHECK_UTILRESULT(wifi_utils_set_autoconnect(1), TAG, "Set Autoconnect failed");
#endif
	return WIFI_MANAGER_SUCCESS;
}

wifi_manager_result_e _wifimgr_save_connected_config(wifi_manager_ap_config_s *config)
{
#ifdef CONFIG_WIFI_MANAGER_SAVE_CONFIG
	trwifi_result_e ret = wifi_profile_write(config, 1);
	if (ret != TRWIFI_SUCCESS) {
		NET_LOGE(TAG, "Failed to save the connected AP configuration in file system\n");
		return WIFI_MANAGER_FAIL;
	}
#endif
	return WIFI_MANAGER_SUCCESS;
}

wifi_manager_result_e _wifimgr_connect_ap(wifi_manager_ap_config_s *config)
{
	trwifi_ap_config_s util_config;
	strncpy(util_config.ssid, config->ssid, WIFIMGR_SSID_LEN);
	util_config.ssid[WIFIMGR_SSID_LEN] = '\0';
	util_config.ssid_length = config->ssid_length;
	strncpy(util_config.passphrase, config->passphrase, WIFIMGR_PASSPHRASE_LEN);
	util_config.passphrase[WIFIMGR_PASSPHRASE_LEN] = '\0';
	util_config.passphrase_length = config->passphrase_length;
	util_config.ap_auth_type = wifimgr_convert2trwifi_auth(config->ap_auth_type);
	util_config.ap_crypto_type = wifimgr_convert2trwifi_crypto(config->ap_crypto_type);

	trwifi_result_e wres = wifi_utils_connect_ap(&util_config, NULL);
	if (wres == TRWIFI_ALREADY_CONNECTED) {
		return WIFI_MANAGER_ALREADY_CONNECTED;
	} else if (wres != TRWIFI_SUCCESS) {
		WIFIADD_ERR_RECORD(ERR_WIFIMGR_CONNECT_FAIL);
		return WIFI_MANAGER_FAIL;
	}
	WIFIMGR_SET_SSID(config->ssid);
	wifi_manager_result_e wret = _wifimgr_save_connected_config(config);
	if (wret != WIFI_MANAGER_SUCCESS) {
		WIFIADD_ERR_RECORD(ERR_WIFIMGR_INTERNAL_FAIL);
		return wret;
	}

	return WIFI_MANAGER_SUCCESS;
}

wifi_manager_result_e _wifimgr_disconnect_ap(void)
{
	WIFIMGR_CHECK_UTILRESULT(wifi_utils_disconnect_ap(NULL), TAG, "disconnect to ap fail");
	return WIFI_MANAGER_SUCCESS;
}

wifi_manager_result_e _wifimgr_run_softap(wifi_manager_softap_config_s *config)
{
	if (strlen(config->ssid) > WIFIMGR_SSID_LEN || strlen(config->passphrase) > WIFIMGR_PASSPHRASE_LEN) {
		NET_LOGE(TAG, "SSID or PASSPHRASE length invalid\n");
		return WIFI_MANAGER_FAIL;
	}
	trwifi_softap_config_s softap_config;

	softap_config.channel = config->channel;
	softap_config.ap_crypto_type = TRWIFI_CRYPTO_AES;
	softap_config.ap_auth_type = TRWIFI_AUTH_WPA2_PSK;
	softap_config.ssid_length = strlen(config->ssid);
	softap_config.passphrase_length = strlen(config->passphrase);
	strncpy(softap_config.ssid, config->ssid, WIFIMGR_SSID_LEN);
	softap_config.ssid[WIFIMGR_SSID_LEN] = '\0';
	strncpy(softap_config.passphrase, config->passphrase, WIFIMGR_PASSPHRASE_LEN);
	softap_config.passphrase[WIFIMGR_PASSPHRASE_LEN] = '\0';

	WIFIMGR_CHECK_UTILRESULT(wifi_utils_start_softap(&softap_config),
							 TAG, "Starting softap mode failed");
#ifndef CONFIG_WIFIMGR_DISABLE_DHCPS
	WIFIMGR_CHECK_RESULT(wm_dhcps_start(), (TAG, "Starting DHCP server failed\n"), WIFI_MANAGER_FAIL);
	dhcps_reset_num();
#endif
	/* update wifi_manager_info */
	WIFIMGR_SET_SOFTAP_SSID(config->ssid);

	/* For tracking softap stats, the LAST value is used */
	WIFIMGR_STATS_INC(CB_SOFTAP_DONE);
	return WIFI_MANAGER_SUCCESS;
}

wifi_manager_result_e _wifimgr_stop_softap(void)
{
	WIFIMGR_CHECK_UTILRESULT(wifi_utils_stop_softap(), TAG, "Stoping softap failed");
#ifndef CONFIG_WIFIMGR_DISABLE_DHCPS
	WIFIMGR_CHECK_RESULT(wm_dhcps_stop(), (TAG, "Stoping softap DHCP server failed.\n"), WIFI_MANAGER_FAIL);
#endif
	return WIFI_MANAGER_SUCCESS;
}

wifi_manager_result_e _wifimgr_scan(wifi_manager_scan_config_s *config)
{
	if (!config) {
		WIFIMGR_CHECK_UTILRESULT(wifi_utils_scan_ap(NULL), TAG,
								 "request scan to wifi utils is fail");
		return WIFI_MANAGER_SUCCESS;
	}

	trwifi_scan_config_s uconf = {0, {
										 0,
									 },
								  0};
	memset(&uconf, 0, sizeof(trwifi_scan_config_s));
	if (config->ssid_length > 0) {
		strncpy(uconf.ssid, config->ssid, config->ssid_length + 1);
		uconf.ssid_length = config->ssid_length;
	}
	if (config->channel != 0) {
		uconf.channel = config->channel;
	}

	WIFIMGR_CHECK_UTILRESULT(wifi_utils_scan_ap((void *)&uconf), TAG,
							 "request scan is fail");
	return WIFI_MANAGER_SUCCESS;
}

static wifi_manager_result_e _wifimgr_scan_multi_aps(wifi_manager_scan_multi_configs_s *configs)
{
	if (!configs) {
		WIFIMGR_CHECK_UTILRESULT(wifi_utils_scan_multi_aps(NULL), TAG,
								 "request scan multi aps to wifi utils is fail");
		return WIFI_MANAGER_SUCCESS;
	}

	trwifi_scan_multi_configs_s uconf = {0};
	memset(&uconf, 0, sizeof(trwifi_scan_multi_configs_s));
	uconf.scan_ap_config_count = configs->scan_ap_config_count;
	uconf.scan_all = configs->scan_all;
	for (int i = 0; i < configs->scan_ap_config_count; i++) {
		wifi_manager_scan_config_s *config = &configs->ap_configs[i];
		if (config->ssid_length > 0) {
			strncpy(uconf.scan_ap_config[i].ssid, config->ssid, config->ssid_length + 1);
			uconf.scan_ap_config[i].ssid_length = config->ssid_length;
		}
		if (config->channel != 0) {
			uconf.scan_ap_config[i].channel = config->channel;
		}
	}

	WIFIMGR_CHECK_UTILRESULT(wifi_utils_scan_multi_aps((void *)&uconf), TAG,
							 "request scan multi aps is fail");
	return WIFI_MANAGER_SUCCESS;
}

#if defined(CONFIG_ENABLE_HOMELYNK) && (CONFIG_ENABLE_HOMELYNK == 1)
wifi_manager_result_e _wifimgr_control_bridge(uint8_t isenable)
{
	WIFIMGR_CHECK_UTILRESULT(wifi_utils_control_bridge(isenable), TAG, "Starting STA failed.");
	return WIFI_MANAGER_SUCCESS;
}
#endif

wifi_manager_result_e _handler_on_uninitialized_state(wifimgr_msg_s *msg)
{
	wifimgr_evt_e evt = msg->event;
	if (evt != WIFIMGR_CMD_INIT) {
		WIFIADD_ERR_RECORD(ERR_WIFIMGR_INVALID_EVENT);
		return WIFI_MANAGER_FAIL;
	}

	WIFIMGR_CHECK_UTILRESULT(wifi_utils_init(), TAG, "wifi_utils_init fail");

#ifdef CONFIG_WIFI_MANAGER_SAVE_CONFIG
	WIFIMGR_CHECK_UTILRESULT(wifi_profile_init(), TAG, "wifi_profile init fail");
#endif
	/*  register default callback to callback handler */
	wifi_manager_cb_s *cb = (wifi_manager_cb_s *)msg->param;
	int res = wifimgr_register_cb(cb);
	if (res < 0) {
		NET_LOGE(TAG, "WIFIMGR REGISTER CB\n");
	}
#ifdef CONFIG_DISABLE_EXTERNAL_AUTOCONNECT
	WIFIMGR_CHECK_UTILRESULT(wifi_utils_set_autoconnect(0), TAG, "Set Autoconnect failed");
#else
	WIFIMGR_CHECK_UTILRESULT(wifi_utils_set_autoconnect(1), TAG, "Set Autoconnect failed");
#endif

	WIFIMGR_SET_STATE(WIFIMGR_STA_DISCONNECTED);

	return WIFI_MANAGER_SUCCESS;
}

wifi_manager_result_e _handler_on_disconnected_state(wifimgr_msg_s *msg)
{
	if (msg->event == WIFIMGR_CMD_CONNECT) {
		wifi_manager_ap_config_s *apinfo = (wifi_manager_ap_config_s *)msg->param;
		WIFIMGR_CHECK_RESULT(_wifimgr_connect_ap(apinfo), (TAG, "connect ap fail\n"), WIFI_MANAGER_FAIL);
		WIFIMGR_SET_STATE(WIFIMGR_STA_CONNECTING);
	} else if (msg->event == WIFIMGR_CMD_DEINIT) {
		WIFIMGR_CHECK_RESULT(_wifimgr_deinit(), (TAG, "critical error\n"), WIFI_MANAGER_FAIL);
		WIFIMGR_SEND_API_SIGNAL(msg->signal);
		WIFIMGR_SET_STATE(WIFIMGR_UNINITIALIZED);
	} else if (msg->event == WIFIMGR_CMD_SET_SOFTAP) {
		WIFIMGR_CHECK_RESULT(_wifimgr_run_softap((wifi_manager_softap_config_s *)msg->param),
							 (TAG, "run_softap fail\n"), WIFI_MANAGER_FAIL);
		WIFIMGR_SEND_API_SIGNAL(msg->signal);
		WIFIMGR_SET_STATE(WIFIMGR_SOFTAP);
	} else if (msg->event == WIFIMGR_CMD_SCAN) {
		WIFIMGR_CHECK_RESULT(_wifimgr_scan((wifi_manager_scan_config_s *)msg->param), (TAG, "fail scan\n"), WIFI_MANAGER_FAIL);
		WIFIMGR_STORE_PREV_STATE;
		WIFIMGR_SET_STATE(WIFIMGR_SCANNING);
	} else if (msg->event == WIFIMGR_CMD_SCAN_MULTI_APS) {
		WIFIMGR_CHECK_RESULT(_wifimgr_scan_multi_aps((wifi_manager_scan_multi_configs_s *)msg->param), (TAG, "fail scan\n"), WIFI_MANAGER_FAIL);
		WIFIMGR_STORE_PREV_STATE;
		WIFIMGR_SET_STATE(WIFIMGR_SCANNING);
	} else if (msg->event == WIFIMGR_EVT_SCAN_DONE) {
		wifimgr_call_cb(CB_SCAN_DONE, msg->param);
		_free_scan_list((trwifi_scan_list_s *)msg->param);
#if defined(CONFIG_ENABLE_HOMELYNK) && (CONFIG_ENABLE_HOMELYNK == 1)
	} else if (msg->event == WIFIMGR_CMD_SET_BRIDGE) {
		wifi_manager_bridge_config_s *config = (wifi_manager_bridge_config_s *)msg->param;
		uint8_t enable = config->enable;
		if (!enable) {
			WIFIADD_ERR_RECORD(ERR_WIFIMGR_INVALID_EVENT);
			return WIFI_MANAGER_FAIL;
		}
		
		WIFIMGR_CHECK_RESULT(_wifimgr_control_bridge(enable), (TAG, "critical error\n"), WIFI_MANAGER_FAIL);
		WIFIMGR_CHECK_RESULT(_wifimgr_run_softap((wifi_manager_softap_config_s *)&config->softap_config),
							(TAG, "run_softap fail\n"), WIFI_MANAGER_FAIL);
		WIFIMGR_SET_BRIDGE_STATE_ON();
		WIFIMGR_SET_BRIDGE_STA_CONNECTED(false);
		WIFIMGR_SET_IS_SOFTAP_AFTER_BRIDGE(false);
		WIFIMGR_SEND_API_SIGNAL(msg->signal);
		WIFIMGR_SET_STATE(WIFIMGR_BRIDGE);
#endif
	} else {
		WIFIADD_ERR_RECORD(ERR_WIFIMGR_INVALID_EVENT);
		return WIFI_MANAGER_FAIL;
	}
	return WIFI_MANAGER_SUCCESS;
}

wifi_manager_result_e _handler_on_disconnecting_state(wifimgr_msg_s *msg)
{
	if (msg->event == WIFIMGR_CMD_DEINIT) {
		WIFIMGR_SET_SUBSTATE(WIFIMGR_DISCONN_DEINIT, msg->signal);
		return WIFI_MANAGER_SUCCESS;
	}

	if (msg->event != WIFIMGR_EVT_STA_DISCONNECTED && msg->event != WIFIMGR_EVT_STA_CONNECTED && msg->event != WIFIMGR_EVT_STA_CONNECT_FAILED && msg->event != WIFIMGR_EVT_SCAN_DONE) {
		WIFIADD_ERR_RECORD(ERR_WIFIMGR_INVALID_EVENT);
		NET_LOGE(TAG, "invalid param\n");
		return WIFI_MANAGER_BUSY;
	}

	if (msg->event == WIFIMGR_EVT_SCAN_DONE) {
		_free_scan_list((trwifi_scan_list_s *)msg->param);
	}

	/* it handles disconnecting state differently by substate
	 * for example, if it enters disconnecting state because dhcpc fails
	 * then it should not call disconnect callback to applications */
	switch (g_manager_info.disconn_substate) {
	case WIFIMGR_DISCONN_DEINIT:
		WIFIMGR_CHECK_RESULT(_wifimgr_deinit(), (TAG, "critical error\n"), WIFI_MANAGER_FAIL);
		WIFIMGR_SEND_API_SIGNAL(g_manager_info.api_sig);
		WIFIMGR_SET_STATE(WIFIMGR_UNINITIALIZED);
		break;
	case WIFIMGR_DISCONN_SOFTAP:
#if defined(CONFIG_ENABLE_HOMELYNK) && (CONFIG_ENABLE_HOMELYNK == 1)
		if (g_bridge_state.is_on == true) {
			WIFIMGR_RESET_BRIDGE_STATE();
			WIFIMGR_CHECK_RESULT(_wifimgr_control_bridge(0), (TAG, "critical error\n"), WIFI_MANAGER_FAIL);
		}
#else
		WIFIMGR_CHECK_RESULT(_wifimgr_run_softap(
								 (wifi_manager_softap_config_s *)&g_manager_info.softap_config),
							 (TAG, "run_softap fail\n"), WIFI_MANAGER_FAIL);
#endif
		WIFIMGR_SEND_API_SIGNAL(g_manager_info.api_sig);
		WIFIMGR_SET_STATE(WIFIMGR_SOFTAP);
		break;
	case WIFIMGR_DISCONN_INTERNAL_ERROR:
		wifimgr_call_cb(CB_STA_CONNECT_FAILED, msg->param);
		WIFIMGR_SET_STATE(WIFIMGR_STA_DISCONNECTED);
		break;
	case WIFIMGR_DISCONN_NONE:
		wifimgr_call_cb(CB_STA_DISCONNECTED, msg->param);
#if defined(CONFIG_ENABLE_HOMELYNK) && (CONFIG_ENABLE_HOMELYNK == 1)
		if (g_bridge_state.is_on == true) {
			WIFIMGR_SET_BRIDGE_STA_CONNECTED(false);
			WIFIMGR_SET_STATE(WIFIMGR_BRIDGE);
		}
		else {
			WIFIMGR_SET_STATE(WIFIMGR_STA_DISCONNECTED);
		}
#else 
		WIFIMGR_SET_STATE(WIFIMGR_STA_DISCONNECTED);
#endif
		break;
	default:
		NET_LOGE(TAG, "invalid argument\n");
		break;
	}

	WIFIMGR_RESET_SUBSTATE();

	return WIFI_MANAGER_SUCCESS;
}

wifi_manager_result_e _handler_on_connecting_state(wifimgr_msg_s *msg)
{
	if (msg->event == WIFIMGR_EVT_STA_CONNECTED) {
#ifndef CONFIG_WIFIMGR_DISABLE_DHCPC
		wifi_manager_result_e wret;
		wret = dhcpc_get_ipaddr();
		if (wret != WIFI_MANAGER_SUCCESS) {
			WIFIMGR_CHECK_RESULT(_wifimgr_disconnect_ap(), (TAG, "critical error: DHCP failure\n"), WIFI_MANAGER_FAIL);
			WIFIMGR_SET_SUBSTATE(WIFIMGR_DISCONN_INTERNAL_ERROR, NULL);
			WIFIMGR_SET_STATE(WIFIMGR_STA_DISCONNECTING);
			return wret;
		}
#endif
		wifimgr_call_cb(CB_STA_CONNECTED, msg->param);
#if defined(CONFIG_ENABLE_HOMELYNK) && (CONFIG_ENABLE_HOMELYNK == 1)
		if (g_bridge_state.is_on == true) {
			WIFIMGR_SET_BRIDGE_STA_CONNECTED(true);
			WIFIMGR_SET_STATE(WIFIMGR_BRIDGE);
		}
		else {
			WIFIMGR_SET_STATE(WIFIMGR_STA_CONNECTED);
		}
#else
		WIFIMGR_SET_STATE(WIFIMGR_STA_CONNECTED);
#endif
		trwifi_info info_utils;
		wifi_utils_get_info(&info_utils);
	} else if (msg->event == WIFIMGR_EVT_STA_CONNECT_FAILED) {
		wifimgr_call_cb(CB_STA_CONNECT_FAILED, msg->param);
#if defined(CONFIG_ENABLE_HOMELYNK) && (CONFIG_ENABLE_HOMELYNK == 1)
		if (g_bridge_state.is_on == true) {
			WIFIMGR_SET_BRIDGE_STA_CONNECTED(false);
			WIFIMGR_SET_STATE(WIFIMGR_BRIDGE);
		}
		else {
			WIFIMGR_SET_STATE(WIFIMGR_STA_DISCONNECTED);
		}
#else
		WIFIMGR_SET_STATE(WIFIMGR_STA_DISCONNECTED);
#endif
	} else if (msg->event == WIFIMGR_CMD_DEINIT) {
		WIFIMGR_SET_SUBSTATE(WIFIMGR_DISCONN_DEINIT, msg->signal);
		WIFIMGR_SET_STATE(WIFIMGR_STA_DISCONNECTING);
	} else {
		WIFIADD_ERR_RECORD(ERR_WIFIMGR_INVALID_EVENT);
		return WIFI_MANAGER_BUSY;
	}

	return WIFI_MANAGER_SUCCESS;
}

wifi_manager_result_e _handler_on_connected_state(wifimgr_msg_s *msg)
{
	if (msg->event == WIFIMGR_CMD_DISCONNECT) {
		dhcpc_close_ipaddr();
		WIFIMGR_CHECK_RESULT(_wifimgr_disconnect_ap(), (TAG, "critical error\n"), WIFI_MANAGER_FAIL);
		WIFIMGR_SET_STATE(WIFIMGR_STA_DISCONNECTING);
	} else if (msg->event == WIFIMGR_EVT_STA_DISCONNECTED) {
		dhcpc_close_ipaddr();
		wifimgr_call_cb(CB_STA_DISCONNECTED, msg->param);
		WIFIMGR_SET_STATE(WIFIMGR_STA_DISCONNECTED);
	} else if (msg->event == WIFIMGR_CMD_SET_SOFTAP) {
		dhcpc_close_ipaddr();
		WIFIMGR_COPY_SOFTAP_CONFIG(g_manager_info.softap_config, (wifi_manager_softap_config_s *)msg->param);
		WIFIMGR_CHECK_RESULT(_wifimgr_disconnect_ap(), (TAG, "critical error\n"), WIFI_MANAGER_FAIL);
		WIFIMGR_SET_SUBSTATE(WIFIMGR_DISCONN_SOFTAP, msg->signal);
		WIFIMGR_SET_STATE(WIFIMGR_STA_DISCONNECTING);
	} else if (msg->event == WIFIMGR_CMD_DEINIT) {
		dhcpc_close_ipaddr();
		WIFIMGR_CHECK_RESULT(_wifimgr_disconnect_ap(), (TAG, "critical error\n"), WIFI_MANAGER_FAIL);
		WIFIMGR_SET_SUBSTATE(WIFIMGR_DISCONN_DEINIT, msg->signal);
		WIFIMGR_SET_STATE(WIFIMGR_STA_DISCONNECTING);
	} else if (msg->event == WIFIMGR_CMD_SCAN) {
		WIFIMGR_CHECK_RESULT(_wifimgr_scan((wifi_manager_scan_config_s *)msg->param), (TAG, "fail scan\n"), WIFI_MANAGER_FAIL);
		WIFIMGR_STORE_PREV_STATE;
		WIFIMGR_SET_STATE(WIFIMGR_SCANNING);
	} else if (msg->event == WIFIMGR_CMD_SCAN_MULTI_APS) {
		WIFIMGR_CHECK_RESULT(_wifimgr_scan_multi_aps((wifi_manager_scan_multi_configs_s *)msg->param), (TAG, "fail scan\n"), WIFI_MANAGER_FAIL);
		WIFIMGR_STORE_PREV_STATE;
		WIFIMGR_SET_STATE(WIFIMGR_SCANNING);
	} else if (msg->event == WIFIMGR_CMD_CONNECT) {
		WIFIADD_ERR_RECORD(ERR_WIFIMGR_INVALID_EVENT);
		return WIFI_MANAGER_ALREADY_CONNECTED;
#if defined(CONFIG_ENABLE_HOMELYNK) && (CONFIG_ENABLE_HOMELYNK == 1)
	} else if (msg->event == WIFIMGR_CMD_SET_BRIDGE) {
		wifi_manager_bridge_config_s *config = (wifi_manager_bridge_config_s *)msg->param;
		uint8_t enable = config->enable;
		if (!enable) {
			WIFIADD_ERR_RECORD(ERR_WIFIMGR_INVALID_EVENT);
			return WIFI_MANAGER_FAIL;
		}

		WIFIMGR_CHECK_RESULT(_wifimgr_control_bridge(enable), (TAG, "critical error\n"), WIFI_MANAGER_FAIL);
		WIFIMGR_CHECK_RESULT(_wifimgr_run_softap((wifi_manager_softap_config_s *)&config->softap_config),
							(TAG, "run_softap fail\n"), WIFI_MANAGER_FAIL);
		WIFIMGR_SET_BRIDGE_STATE_ON();
		WIFIMGR_SET_BRIDGE_STA_CONNECTED(true);
		WIFIMGR_SET_IS_SOFTAP_AFTER_BRIDGE(false);
		WIFIMGR_SEND_API_SIGNAL(msg->signal);
		WIFIMGR_SET_STATE(WIFIMGR_BRIDGE);
#endif
	} else {
		WIFIADD_ERR_RECORD(ERR_WIFIMGR_INVALID_EVENT);
		return WIFI_MANAGER_FAIL;
	}
	return WIFI_MANAGER_SUCCESS;
}

wifi_manager_result_e _handler_on_softap_state(wifimgr_msg_s *msg)
{
	if (msg->event == WIFIMGR_CMD_SET_STA) {
		WIFIMGR_CHECK_RESULT(_wifimgr_stop_softap(), (TAG, "critical error\n"), WIFI_MANAGER_FAIL);
		WIFIMGR_CHECK_RESULT(_wifimgr_run_sta(), (TAG, "critical error\n"), WIFI_MANAGER_FAIL);
		WIFIMGR_SEND_API_SIGNAL(msg->signal);
		WIFIMGR_SET_STATE(WIFIMGR_STA_DISCONNECTED);
	} else if (msg->event == WIFIMGR_CMD_SCAN) {
		WIFIMGR_CHECK_RESULT(_wifimgr_scan((wifi_manager_scan_config_s *)msg->param), (TAG, "fail scan\n"), WIFI_MANAGER_FAIL);
		WIFIMGR_STORE_PREV_STATE;
		WIFIMGR_SET_STATE(WIFIMGR_SCANNING);
	} else if (msg->event == WIFIMGR_CMD_SCAN_MULTI_APS) {
		WIFIMGR_CHECK_RESULT(_wifimgr_scan_multi_aps((wifi_manager_scan_multi_configs_s *)msg->param), (TAG, "fail scan\n"), WIFI_MANAGER_FAIL);
		WIFIMGR_STORE_PREV_STATE;
		WIFIMGR_SET_STATE(WIFIMGR_SCANNING);
#ifdef CONFIG_WIFIMGR_DISABLE_DHCPS
	} else if (msg->event == WIFIMGR_EVT_JOINED) {
#else
		/* wifi manager passes the callback after the dhcp server gives a station an IP address*/
	} else if (msg->event == WIFIMGR_EVT_DHCPS_ASSIGN_IP) {
		if (dhcps_add_node((dhcp_node_s *)msg->param) == DHCP_EXIST) {
			return WIFI_MANAGER_SUCCESS;
		}
		dhcps_inc_num();
#endif
		wifimgr_call_cb(CB_STA_JOINED, msg->param);
	} else if (msg->event == WIFIMGR_EVT_LEFT) {
#ifndef CONFIG_WIFIMGR_DISABLE_DHCPS
		dhcps_del_node();
		dhcps_dec_num();
#endif
		wifimgr_call_cb(CB_STA_LEFT, msg->param);
	} else if (msg->event == WIFIMGR_CMD_DEINIT) {
		WIFIMGR_CHECK_RESULT(_wifimgr_stop_softap(), (TAG, "critical error\n"), WIFI_MANAGER_FAIL);
		WIFIMGR_CHECK_RESULT(_wifimgr_deinit(), (TAG, "critical error\n"), WIFI_MANAGER_FAIL);
		WIFIMGR_SEND_API_SIGNAL(msg->signal);
		WIFIMGR_SET_STATE(WIFIMGR_UNINITIALIZED);
#if defined(CONFIG_ENABLE_HOMELYNK) && (CONFIG_ENABLE_HOMELYNK == 1)
	} else if (msg->event == WIFIMGR_CMD_SET_BRIDGE) {
		wifi_manager_bridge_config_s *config = (wifi_manager_bridge_config_s *)msg->param;
		uint8_t enable = config->enable;
		if (!enable) {
			WIFIADD_ERR_RECORD(ERR_WIFIMGR_INVALID_EVENT);
			return WIFI_MANAGER_FAIL;
		}

		WIFIMGR_CHECK_RESULT(_wifimgr_control_bridge(enable), (TAG, "critical error\n"), WIFI_MANAGER_FAIL);
		WIFIMGR_SET_BRIDGE_STATE_ON();
		WIFIMGR_SET_BRIDGE_STA_CONNECTED(false);
		WIFIMGR_SET_IS_SOFTAP_AFTER_BRIDGE(true);
		WIFIMGR_SEND_API_SIGNAL(msg->signal);
		WIFIMGR_SET_STATE(WIFIMGR_BRIDGE);
	} else {
		WIFIADD_ERR_RECORD(ERR_WIFIMGR_INVALID_EVENT);
		return WIFI_MANAGER_FAIL;
	}

	return WIFI_MANAGER_SUCCESS;
}

wifi_manager_result_e _handler_on_bridge_state(wifimgr_msg_s *msg)
{
	if (msg->event == WIFIMGR_CMD_DEINIT) {
		WIFIMGR_CHECK_RESULT(_wifimgr_stop_softap(), (TAG, "critical error\n"), WIFI_MANAGER_FAIL);

		if (g_bridge_state.is_sta_connected == true) {
			dhcpc_close_ipaddr();
			WIFIMGR_CHECK_RESULT(_wifimgr_disconnect_ap(), (TAG, "critical error\n"), WIFI_MANAGER_FAIL);
			WIFIMGR_SET_SUBSTATE(WIFIMGR_DISCONN_DEINIT, msg->signal);
			WIFIMGR_SET_STATE(WIFIMGR_STA_DISCONNECTING);
		} else {
			WIFIMGR_CHECK_RESULT(_wifimgr_deinit(), (TAG, "critical error\n"), WIFI_MANAGER_FAIL);
			WIFIMGR_SEND_API_SIGNAL(msg->signal);
			WIFIMGR_SET_STATE(WIFIMGR_UNINITIALIZED);
		}
	} else if (msg->event == WIFIMGR_CMD_CONNECT && g_bridge_state.is_sta_connected == false) {
		wifi_manager_ap_config_s *apinfo = (wifi_manager_ap_config_s *)msg->param;
		WIFIMGR_CHECK_RESULT(_wifimgr_connect_ap(apinfo), (TAG, "connect ap fail\n"), WIFI_MANAGER_FAIL);
		WIFIMGR_SET_STATE(WIFIMGR_STA_CONNECTING);
	} else if (msg->event == WIFIMGR_CMD_DISCONNECT && g_bridge_state.is_sta_connected == true) {
		dhcpc_close_ipaddr();
		WIFIMGR_CHECK_RESULT(_wifimgr_disconnect_ap(), (TAG, "critical error\n"), WIFI_MANAGER_FAIL);
		WIFIMGR_SET_STATE(WIFIMGR_STA_DISCONNECTING);
	} else if (msg->event == WIFIMGR_CMD_SCAN) {
		WIFIMGR_CHECK_RESULT(_wifimgr_scan((wifi_manager_scan_config_s *)msg->param), (TAG, "fail scan\n"), WIFI_MANAGER_FAIL);
		WIFIMGR_STORE_PREV_STATE;
		WIFIMGR_SET_STATE(WIFIMGR_SCANNING);
	} else if (msg->event == WIFIMGR_CMD_SCAN_MULTI_APS) {
		WIFIMGR_CHECK_RESULT(_wifimgr_scan_multi_aps((wifi_manager_scan_multi_configs_s *)msg->param), (TAG, "fail scan\n"), WIFI_MANAGER_FAIL);
		WIFIMGR_STORE_PREV_STATE;
		WIFIMGR_SET_STATE(WIFIMGR_SCANNING);
	} else if (msg->event == WIFIMGR_CMD_SET_BRIDGE) {
		wifi_manager_bridge_config_s *config = (wifi_manager_bridge_config_s *)msg->param;
		uint8_t enable = config->enable;
		if (enable) {
			WIFIADD_ERR_RECORD(ERR_WIFIMGR_INVALID_EVENT);
			return WIFI_MANAGER_FAIL;
		}

		if (g_bridge_state.is_softap_after_bridge == true && g_bridge_state.is_sta_connected == true) {
			dhcpc_close_ipaddr();
			WIFIMGR_CHECK_RESULT(_wifimgr_disconnect_ap(), (TAG, "critical error\n"), WIFI_MANAGER_FAIL);
			WIFIMGR_SET_SUBSTATE(WIFIMGR_DISCONN_SOFTAP, msg->signal);
			WIFIMGR_SET_STATE(WIFIMGR_STA_DISCONNECTING);
			return WIFI_MANAGER_SUCCESS;
		} else if (g_bridge_state.is_softap_after_bridge == false) {
			WIFIMGR_CHECK_RESULT(_wifimgr_stop_softap(), (TAG, "critical error\n"), WIFI_MANAGER_FAIL);
		}
		
		WIFIMGR_CHECK_RESULT(_wifimgr_control_bridge(enable), (TAG, "critical error\n"), WIFI_MANAGER_FAIL);

		WIFIMGR_SEND_API_SIGNAL(msg->signal);
		if (g_bridge_state.is_softap_after_bridge == true) {
			WIFIMGR_SET_STATE(WIFIMGR_SOFTAP);
		} else if (g_bridge_state.is_sta_connected == true) {
			WIFIMGR_SET_STATE(WIFIMGR_STA_CONNECTED);
		} else {
			WIFIMGR_SET_STATE(WIFIMGR_STA_DISCONNECTED);
		}
		WIFIMGR_RESET_BRIDGE_STATE();
	} else if (msg->event == WIFIMGR_EVT_STA_DISCONNECTED) {
		dhcpc_close_ipaddr();
		wifimgr_call_cb(CB_STA_DISCONNECTED, msg->param);
		WIFIMGR_SET_BRIDGE_STA_CONNECTED(false);
#ifdef CONFIG_WIFIMGR_DISABLE_DHCPS
	} else if (msg->event == WIFIMGR_EVT_JOINED) {
#else
		/* wifi manager passes the callback after the dhcp server gives a station an IP address*/
	} else if (msg->event == WIFIMGR_EVT_DHCPS_ASSIGN_IP) {
		if (dhcps_add_node((dhcp_node_s *)msg->param) == DHCP_EXIST) {
			return WIFI_MANAGER_SUCCESS;
		}
		dhcps_inc_num();
#endif
		wifimgr_call_cb(CB_STA_JOINED, msg->param);
	} else if (msg->event == WIFIMGR_EVT_LEFT) {
#ifndef CONFIG_WIFIMGR_DISABLE_DHCPS
		dhcps_del_node();
		dhcps_dec_num();
#endif
		wifimgr_call_cb(CB_STA_LEFT, msg->param);
	} else if (msg->event == WIFIMGR_EVT_SCAN_DONE) {
		wifimgr_call_cb(CB_SCAN_DONE, msg->param);
		_free_scan_list((trwifi_scan_list_s *)msg->param);
#endif // #if defined(CONFIG_ENABLE_HOMELYNK) && (CONFIG_ENABLE_HOMELYNK == 1)
	} else {
		WIFIADD_ERR_RECORD(ERR_WIFIMGR_INVALID_EVENT);
		return WIFI_MANAGER_FAIL;
	}

	return WIFI_MANAGER_SUCCESS;
}

wifi_manager_result_e _handler_on_scanning_state(wifimgr_msg_s *msg)
{
	wifi_manager_result_e wret = WIFI_MANAGER_FAIL;
	if (msg->event == WIFIMGR_EVT_SCAN_DONE) {
		wifimgr_call_cb(CB_SCAN_DONE, msg->param);
		_free_scan_list((trwifi_scan_list_s *)msg->param);
		WIFIMGR_RESTORE_STATE;
		wret = WIFI_MANAGER_SUCCESS;
	} else if (msg->event == WIFIMGR_CMD_DEINIT) {
		WIFIMGR_SET_SUBSTATE(WIFIMGR_DISCONN_DEINIT, msg->signal);
		WIFIMGR_SET_STATE(WIFIMGR_STA_DISCONNECTING);
		wret = WIFI_MANAGER_SUCCESS;
	} else if (msg->event == WIFIMGR_EVT_STA_DISCONNECTED) {
		dhcpc_close_ipaddr();
		wifimgr_call_cb(CB_STA_DISCONNECTED, msg->param);
#if defined(CONFIG_ENABLE_HOMELYNK) && (CONFIG_ENABLE_HOMELYNK == 1)
		if (g_bridge_state.is_on == true) {
			WIFIMGR_SET_BRIDGE_STA_CONNECTED(false);
			WIFIMGR_SET_STATE(WIFIMGR_BRIDGE);
		}
		else {
			WIFIMGR_SET_STATE(WIFIMGR_STA_DISCONNECTED);
		}
#else
		WIFIMGR_SET_STATE(WIFIMGR_STA_DISCONNECTED);
#endif
		wret = WIFI_MANAGER_SUCCESS;
#ifdef CONFIG_WIFIMGR_DISABLE_DHCPS
	} else if (msg->event == WIFIMGR_EVT_JOINED) {
#else
		/* wifi manager passes the callback after the dhcp server gives a station an IP address*/
	} else if (msg->event == WIFIMGR_EVT_DHCPS_ASSIGN_IP) {
		if (dhcps_add_node((dhcp_node_s *)msg->param) == DHCP_EXIST) {
			return WIFI_MANAGER_SUCCESS;
		}
		dhcps_inc_num();
#endif
		wifimgr_call_cb(CB_STA_JOINED, msg->param);
		wret = WIFI_MANAGER_SUCCESS;
	} else if (msg->event == WIFIMGR_EVT_LEFT) {
#ifndef CONFIG_WIFIMGR_DISABLE_DHCPS
		dhcps_del_node();
		dhcps_dec_num();
#endif
		wifimgr_call_cb(CB_STA_LEFT, msg->param);
		wret = WIFI_MANAGER_SUCCESS;
	} else {
		WIFIADD_ERR_RECORD(ERR_WIFIMGR_INVALID_EVENT);
	}
	return wret;
}

wifi_manager_result_e _handler_get_stats(wifimgr_msg_s *msg)
{
	trwifi_msg_stats_s stats;
	stats.cmd = TRWIFI_MSG_GET_STATS;
	trwifi_result_e res = wifi_utils_ioctl((trwifi_msg_s *)&stats);
	if (res == TRWIFI_SUCCESS) {
		/* update msg
		 * msg->param was checked in wifi_manager_get_stats()
		 * So doesn't need to check msg->param in here.
		 */
		wifi_manager_stats_s *wstats = (wifi_manager_stats_s *)msg->param;
		wstats->start = stats.start;
		wstats->end = stats.end;
		wstats->tx_retransmit = stats.tx_retransmit;
		wstats->tx_drop = stats.tx_drop;
		wstats->rx_drop = stats.rx_drop;
		wstats->tx_success_cnt = stats.tx_success_cnt;
		wstats->tx_success_bytes = stats.tx_success_bytes;
		wstats->rx_cnt = stats.rx_cnt;
		wstats->rx_bytes = stats.rx_bytes;
		wstats->tx_try = stats.tx_try;
		wstats->rssi_avg = stats.rssi_avg;
		wstats->rssi_min = stats.rssi_min;
		wstats->rssi_max = stats.rssi_max;
		wstats->beacon_miss_cnt = stats.beacon_miss_cnt;
	}
	return wifimgr_convert2wifimgr_res(res);
}

wifi_manager_result_e _handler_set_powermode(wifimgr_msg_s *msg)
{
	wifi_manager_powermode_e mode = *((wifi_manager_powermode_e *)(msg->param));
	int imode = TRWIFI_POWERMODE_OFF;
	if (mode == WIFI_MANAGER_POWERMODE_ENABLE) {
		imode = TRWIFI_POWERMODE_ON;
	}
	trwifi_msg_s tmsg = {TRWIFI_MSG_SET_POWERMODE, (void *)(&imode)};
	trwifi_result_e res = wifi_utils_ioctl(&tmsg);
	return wifimgr_convert2wifimgr_res(res);
}

wifi_manager_result_e wifimgr_handle_request(wifimgr_msg_s *msg)
{
	wifi_manager_result_e res = WIFI_MANAGER_FAIL;

	NET_LOGI(TAG, "handle request state(%s) evt(%s)\n",
			 wifimgr_get_state_str(WIFIMGR_GET_STATE),
			 wifimgr_get_evt_str(msg->event));
	if (msg->event == WIFIMGR_CMD_GETSTATS) {
		res = _handler_get_stats(msg);
	} else if (msg->event == WIFIMGR_CMD_SETPOWER) {
		res = _handler_set_powermode(msg);
	} else {
		res = g_handler[WIFIMGR_GET_STATE](msg);
	}
#ifdef CONFIG_WIFIMGR_ERROR_REPORT
	_set_error_code(res);
#endif
	NET_LOGV(TAG, "<-- _handle_request\n");
	return res;
}
