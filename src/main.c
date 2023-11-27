#include <stdio.h>
#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(main, CONFIG_APP_LOG_LEVEL);
#include <zephyr/drivers/gpio.h>
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_uart.h>
#include "network_common.h"
#include "nrf_errno.h"
#include "nrf_modem_at.h"

#define MAX_PLMN_LIST_CNT (32)
#define AT_CMD_RESP_BUF_LEN (1024)
#define AT_PARAM_STR_MAX_LEN (16)
#define AT_CMD_PDP_ACT_READ "AT+CGACT?"

/* For having a numeric constant in scanf string length */
#define L_(x) #x
#define L(x) L_(x)

K_SEM_DEFINE(network_sem, 0, 1);

enum main_job {
	AT_REQ_PLMN,
	AT_NO_REQ,
};

struct plmn_info {
	uint32_t plmn;
	uint8_t stat;
};

static enum main_job mainJob = AT_NO_REQ;

struct plmn_info list_plmn[MAX_PLMN_LIST_CNT] = {0x00};
size_t list_plmn_counter = 0;
char resp_buf[AT_CMD_RESP_BUF_LEN] = {0x00};

void at_get_plmn(const char* cops) {
	const char* token = strtok((char*)cops, ",");
	int counter = 0;
	bool found = false;
	while (token != NULL) {
		counter += 1;
		if (counter == 1) {
			list_plmn[list_plmn_counter].stat = atoi(token);
		} else if (counter == 4) {
			list_plmn[list_plmn_counter].plmn = (uint32_t)atoi(token + 1);
			found = true;
		}
		token = strtok(NULL, ",");
	}

	if (found) {
		list_plmn_counter += 1;
	}
}

void at_parse_cops(const char* response) {
	const char* token = strtok((char*)response, "+COPS: ");
	
	while (token != NULL) {
		// Extract the numeric value within the parentheses
		const char* start = strchr(token, '(');
		const char* end = strchr(token, ')');
		
		if (start != NULL && end != NULL) {
			char operator[32] = {0x00};
			snprintf(operator, sizeof(operator), "%.*s", (int)(end - start - 1), start + 1);
			at_get_plmn(operator);
		} else {
			break;
		}
		token = end + 1;
	}
}

static void plmn_selection_cb(const char *resp)
{
	strcpy(resp_buf, resp);
	k_sem_give(&network_sem);
}

static void monitor_cb(const char *resp)
{
	LOG_INF("Response %s", resp);
}

void network_connected(void)
{
	LOG_INF("Network connected");
}

void network_disconnected(void)
{
	LOG_INF("The network connection was lost");
}

bool is_default_pdn_active(void)
{
	char at_response_str[128];
	const char *p;
	int err;
	bool is_active = false;

	err = nrf_modem_at_cmd(at_response_str, sizeof(at_response_str), AT_CMD_PDP_ACT_READ);
	if (err) {
		LOG_ERR("Cannot get PDP contexts activation states, err: %d", err);
		return false;
	}

	/* Search for a string +CGACT: <cid>,<state> */
	p = strstr(at_response_str, "+CGACT: 0,1");
	if (p) {
		is_active = true;
	}
	return is_active;
}

int main(void)
{
	network_init();
	
	while (1) {
		switch (mainJob) {
			case AT_REQ_PLMN: {
				k_sem_take(&network_sem, K_SECONDS(5 * 60));
				at_parse_cops(resp_buf);
				if (list_plmn_counter == 0) {
					shell_print(shell_backend_uart_get_ptr(), "No PLMN is available here");
				} else {
					shell_print(shell_backend_uart_get_ptr(), "List PLMN available as below:");
					for (int i = 0; i < list_plmn_counter; i++) {
						shell_print(shell_backend_uart_get_ptr(), "PLMN %d %d %d", i, list_plmn[i].stat, list_plmn[i].plmn);
					}
				}
				mainJob = AT_NO_REQ;
				break;
			}
			case AT_NO_REQ: {
				/* No action required */
				break;
			}
		}
		k_sleep(K_MSEC(1000));
	}
	return 0;
}

static int cmd_scan_plmn(const struct shell *shell, size_t argc, char **argv)
{
	int ret = 0;
	ret = nrf_modem_at_printf("AT+CFUN=1");
	if (ret < 0) {
		shell_error(shell, "AT+CFUN failed: %d", ret);
		return 0;
	}

	ret = nrf_modem_at_cmd_async(plmn_selection_cb, "AT+COPS=?");
	if (ret < 0 && ret != -NRF_EINPROGRESS) {
		shell_error(shell, "AT cmd async error : %d", ret);
		return 0;
	}

	mainJob = AT_REQ_PLMN;
	shell_print(shell, "Please wait around 5 minutes to get result");
	return 0;
}

static int cmd_analyze_plmn(const struct shell *shell, size_t argc, char **argv)
{
	int err = 0;
	if (!is_default_pdn_active()) {
		shell_print(shell, "Enable modem");
		err = nrf_modem_at_printf("AT+CFUN=1");
		if (err < 0) {
			shell_error(shell, "AT+CFUN failed: %d", err);
			return 0;
		}
		shell_print(shell, "Wait 10 seconds after initializing network");
		k_sleep(K_SECONDS(10));
	}

	if (list_plmn_counter == 0) {
		shell_print(shell, "No PLMN is available here");
	} else {
		shell_print(shell, "List PLMN available as below:");
		for (int i = 0; i < list_plmn_counter; i++) {
			shell_print(shell, "Active PLMN %d", list_plmn[i].plmn);
			err = nrf_modem_at_cmd(resp_buf, sizeof(resp_buf), "AT+COPS=1,2,\"%d\"", list_plmn[i].plmn);
			if (err) {
				shell_error(shell, "Can't query the command to switch PLMN");
				return 0;
			}
			shell_print(shell, "Response: %s", resp_buf);
			shell_print(shell, "Monitor the status for PLMN %d", list_plmn[i].plmn);
			err = nrf_modem_at_cmd(resp_buf, sizeof(resp_buf), "AT%%XMONITOR");
			if (err) {
				shell_error(shell, "Can't query the command to switch PLMN");
				return 0;
			}
			shell_print(shell, "Response: %s", resp_buf);
			k_sleep(K_SECONDS(5));
		}
	}
	shell_print(shell, "Done");
	return 0;
}

static int cmd_monitor_plmn(const struct shell *shell, size_t argc, char **argv) {
	/* Parsed strings include double quotes */
	
	char plmn_str[AT_PARAM_STR_MAX_LEN + 1] = { 0 };
	char tac_str[AT_PARAM_STR_MAX_LEN + 1] = { 0 };
	char cell_id_str[AT_PARAM_STR_MAX_LEN + 1] = { 0 };
	uint16_t phys_cell_id = 0;
	int err = 0;

	if (!is_default_pdn_active()) {
		shell_print(shell, "Enable modem");
		err = nrf_modem_at_printf("AT+CFUN=1");
		if (err < 0) {
			shell_error(shell, "AT+CFUN failed: %d", err);
			return 0;
		}
		shell_print(shell, "Wait 10 seconds after initializing network");
		k_sleep(K_SECONDS(10));
	}

	err = nrf_modem_at_cmd(resp_buf, sizeof(resp_buf), "AT%%XMONITOR");
	shell_print(shell, "Response %s", resp_buf);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_network,
	SHELL_CMD(scan, NULL, "Scan all PLMN these are available in current area", cmd_scan_plmn),
	SHELL_CMD(analyze, NULL, "Analyze all PLMN in current area", cmd_analyze_plmn),
	SHELL_CMD(monitor, NULL, "Monitor current assigned PLMN", cmd_monitor_plmn),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(network, &sub_network, "Kama Network Command Management", NULL);